//===--- SemaStmt.cpp - Semantic Analysis for Statements ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaInternal.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ExprObjC.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtCXX.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Scope.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
using namespace clang;
using namespace sema;

StmtResult Sema::ActOnExprStmt(ExprResult FE) {
  if (FE.isInvalid())
    return StmtError();

  FE = ActOnFinishFullExpr(FE.get(), FE.get()->getExprLoc(),
                           /*DiscardedValue*/ true);
  if (FE.isInvalid())
    return StmtError();

  // C99 6.8.3p2: The expression in an expression statement is evaluated as a
  // void expression for its side effects.  Conversion to void allows any
  // operand, even incomplete types.

  // Same thing in for stmt first clause (when expr) and third clause.
  return Owned(static_cast<Stmt*>(FE.take()));
}


StmtResult Sema::ActOnExprStmtError() {
  DiscardCleanupsInEvaluationContext();
  return StmtError();
}

StmtResult Sema::ActOnNullStmt(SourceLocation SemiLoc,
                               bool HasLeadingEmptyMacro) {
  return Owned(new (Context) NullStmt(SemiLoc, HasLeadingEmptyMacro));
}

StmtResult Sema::ActOnDeclStmt(DeclGroupPtrTy dg, SourceLocation StartLoc,
                               SourceLocation EndLoc) {
  DeclGroupRef DG = dg.getAsVal<DeclGroupRef>();

  // If we have an invalid decl, just return an error.
  if (DG.isNull()) return StmtError();

  return Owned(new (Context) DeclStmt(DG, StartLoc, EndLoc));
}

void Sema::ActOnForEachDeclStmt(DeclGroupPtrTy dg) {
  DeclGroupRef DG = dg.getAsVal<DeclGroupRef>();
  
  // If we don't have a declaration, or we have an invalid declaration,
  // just return.
  if (DG.isNull() || !DG.isSingleDecl())
    return;

  Decl *decl = DG.getSingleDecl();
  if (!decl || decl->isInvalidDecl())
    return;

  // Only variable declarations are permitted.
  VarDecl *var = dyn_cast<VarDecl>(decl);
  if (!var) {
    Diag(decl->getLocation(), diag::err_non_variable_decl_in_for);
    decl->setInvalidDecl();
    return;
  }

  // suppress any potential 'unused variable' warning.
  var->setUsed();

  // foreach variables are never actually initialized in the way that
  // the parser came up with.
  var->setInit(0);

  // In ARC, we don't need to retain the iteration variable of a fast
  // enumeration loop.  Rather than actually trying to catch that
  // during declaration processing, we remove the consequences here.
  if (getLangOpts().ObjCAutoRefCount) {
    QualType type = var->getType();

    // Only do this if we inferred the lifetime.  Inferred lifetime
    // will show up as a local qualifier because explicit lifetime
    // should have shown up as an AttributedType instead.
    if (type.getLocalQualifiers().getObjCLifetime() == Qualifiers::OCL_Strong) {
      // Add 'const' and mark the variable as pseudo-strong.
      var->setType(type.withConst());
      var->setARCPseudoStrong(true);
    }
  }
}

/// \brief Diagnose unused '==' and '!=' as likely typos for '=' or '|='.
///
/// Adding a cast to void (or other expression wrappers) will prevent the
/// warning from firing.
static bool DiagnoseUnusedComparison(Sema &S, const Expr *E) {
  SourceLocation Loc;
  bool IsNotEqual, CanAssign;

  if (const BinaryOperator *Op = dyn_cast<BinaryOperator>(E)) {
    if (Op->getOpcode() != BO_EQ && Op->getOpcode() != BO_NE)
      return false;

    Loc = Op->getOperatorLoc();
    IsNotEqual = Op->getOpcode() == BO_NE;
    CanAssign = Op->getLHS()->IgnoreParenImpCasts()->isLValue();
  } else if (const CXXOperatorCallExpr *Op = dyn_cast<CXXOperatorCallExpr>(E)) {
    if (Op->getOperator() != OO_EqualEqual &&
        Op->getOperator() != OO_ExclaimEqual)
      return false;

    Loc = Op->getOperatorLoc();
    IsNotEqual = Op->getOperator() == OO_ExclaimEqual;
    CanAssign = Op->getArg(0)->IgnoreParenImpCasts()->isLValue();
  } else {
    // Not a typo-prone comparison.
    return false;
  }

  // Suppress warnings when the operator, suspicious as it may be, comes from
  // a macro expansion.
  if (S.SourceMgr.isMacroBodyExpansion(Loc))
    return false;

  S.Diag(Loc, diag::warn_unused_comparison)
    << (unsigned)IsNotEqual << E->getSourceRange();

  // If the LHS is a plausible entity to assign to, provide a fixit hint to
  // correct common typos.
  if (CanAssign) {
    if (IsNotEqual)
      S.Diag(Loc, diag::note_inequality_comparison_to_or_assign)
        << FixItHint::CreateReplacement(Loc, "|=");
    else
      S.Diag(Loc, diag::note_equality_comparison_to_assign)
        << FixItHint::CreateReplacement(Loc, "=");
  }

  return true;
}

void Sema::DiagnoseUnusedExprResult(const Stmt *S) {
  if (const LabelStmt *Label = dyn_cast_or_null<LabelStmt>(S))
    return DiagnoseUnusedExprResult(Label->getSubStmt());

  const Expr *E = dyn_cast_or_null<Expr>(S);
  if (!E)
    return;
  SourceLocation ExprLoc = E->IgnoreParens()->getExprLoc();
  // In most cases, we don't want to warn if the expression is written in a
  // macro body, or if the macro comes from a system header. If the offending
  // expression is a call to a function with the warn_unused_result attribute,
  // we warn no matter the location. Because of the order in which the various
  // checks need to happen, we factor out the macro-related test here.
  bool ShouldSuppress = 
      SourceMgr.isMacroBodyExpansion(ExprLoc) ||
      SourceMgr.isInSystemMacro(ExprLoc);

  const Expr *WarnExpr;
  SourceLocation Loc;
  SourceRange R1, R2;
  if (!E->isUnusedResultAWarning(WarnExpr, Loc, R1, R2, Context))
    return;

  // If this is a GNU statement expression expanded from a macro, it is probably
  // unused because it is a function-like macro that can be used as either an
  // expression or statement.  Don't warn, because it is almost certainly a
  // false positive.
  if (isa<StmtExpr>(E) && Loc.isMacroID())
    return;

  // Okay, we have an unused result.  Depending on what the base expression is,
  // we might want to make a more specific diagnostic.  Check for one of these
  // cases now.
  unsigned DiagID = diag::warn_unused_expr;
  if (const ExprWithCleanups *Temps = dyn_cast<ExprWithCleanups>(E))
    E = Temps->getSubExpr();
  if (const CXXBindTemporaryExpr *TempExpr = dyn_cast<CXXBindTemporaryExpr>(E))
    E = TempExpr->getSubExpr();

  if (DiagnoseUnusedComparison(*this, E))
    return;

  E = WarnExpr;
  if (const CallExpr *CE = dyn_cast<CallExpr>(E)) {
    if (E->getType()->isVoidType())
      return;

    // If the callee has attribute pure, const, or warn_unused_result, warn with
    // a more specific message to make it clear what is happening. If the call
    // is written in a macro body, only warn if it has the warn_unused_result
    // attribute.
    if (const Decl *FD = CE->getCalleeDecl()) {
      if (FD->getAttr<WarnUnusedResultAttr>()) {
        Diag(Loc, diag::warn_unused_result) << R1 << R2;
        return;
      }
      if (ShouldSuppress)
        return;
      if (FD->getAttr<PureAttr>()) {
        Diag(Loc, diag::warn_unused_call) << R1 << R2 << "pure";
        return;
      }
      if (FD->getAttr<ConstAttr>()) {
        Diag(Loc, diag::warn_unused_call) << R1 << R2 << "const";
        return;
      }
    }
  } else if (ShouldSuppress)
    return;

  if (const ObjCMessageExpr *ME = dyn_cast<ObjCMessageExpr>(E)) {
    if (getLangOpts().ObjCAutoRefCount && ME->isDelegateInitCall()) {
      Diag(Loc, diag::err_arc_unused_init_message) << R1;
      return;
    }
    const ObjCMethodDecl *MD = ME->getMethodDecl();
    if (MD && MD->getAttr<WarnUnusedResultAttr>()) {
      Diag(Loc, diag::warn_unused_result) << R1 << R2;
      return;
    }
  } else if (const PseudoObjectExpr *POE = dyn_cast<PseudoObjectExpr>(E)) {
    const Expr *Source = POE->getSyntacticForm();
    if (isa<ObjCSubscriptRefExpr>(Source))
      DiagID = diag::warn_unused_container_subscript_expr;
    else
      DiagID = diag::warn_unused_property_expr;
  } else if (const CXXFunctionalCastExpr *FC
                                       = dyn_cast<CXXFunctionalCastExpr>(E)) {
    if (isa<CXXConstructExpr>(FC->getSubExpr()) ||
        isa<CXXTemporaryObjectExpr>(FC->getSubExpr()))
      return;
  }
  // Diagnose "(void*) blah" as a typo for "(void) blah".
  else if (const CStyleCastExpr *CE = dyn_cast<CStyleCastExpr>(E)) {
    TypeSourceInfo *TI = CE->getTypeInfoAsWritten();
    QualType T = TI->getType();

    // We really do want to use the non-canonical type here.
    if (T == Context.VoidPtrTy) {
      PointerTypeLoc TL = TI->getTypeLoc().castAs<PointerTypeLoc>();

      Diag(Loc, diag::warn_unused_voidptr)
        << FixItHint::CreateRemoval(TL.getStarLoc());
      return;
    }
  }

  if (E->isGLValue() && E->getType().isVolatileQualified()) {
    Diag(Loc, diag::warn_unused_volatile) << R1 << R2;
    return;
  }

  DiagRuntimeBehavior(Loc, 0, PDiag(DiagID) << R1 << R2);
}

void Sema::ActOnStartOfCompoundStmt() {
  PushCompoundScope();
}

void Sema::ActOnFinishOfCompoundStmt() {
  PopCompoundScope();
}

sema::CompoundScopeInfo &Sema::getCurCompoundScope() const {
  // For a Cilk for statement, skip the CilkForScopeInfo and return
  // its enclosing CompoundScope. For example,
  //
  // void foo() {
  //   _Cilk_for (int i = 0; i < 10; ++i)
  //     bar();
  // }
  //
  // The body of 'foo()' is returned.
  //
  if (getLangOpts().CilkPlus) {
    unsigned I = FunctionScopes.size() - 1;
    while (isa<CilkForScopeInfo>(FunctionScopes[I]))
      --I;
    assert((I < FunctionScopes.size()) && "unwrap unexpected");
    return FunctionScopes[I]->CompoundScopes.back();
  }

  return getCurFunction()->CompoundScopes.back();
}

namespace {
// Diagnose any _Cilk_spawn expressions (see comment below). InSpawn indicates
// that S is contained within a spawn, e.g. _Cilk_spawn foo(_Cilk_spawn bar())
class DiagnoseCilkSpawnHelper
  : public RecursiveASTVisitor<DiagnoseCilkSpawnHelper> {
  Sema &SemaRef;
  bool &HasError;
public:
  DiagnoseCilkSpawnHelper(Sema &S, bool &HasError)
    : SemaRef(S), HasError(HasError) { }

  bool TraverseCompoundStmt(CompoundStmt *) { return true; }
  bool VisitCallExpr(CallExpr *E) {
    if (E->isCilkSpawnCall()) {
      SemaRef.Diag(E->getCilkSpawnLoc(), SemaRef.PDiag(diag::err_spawn_not_whole_expr)
                                         << E->getSourceRange());
      HasError = true;
    }
    return true;
  }
};
} // anonymous namespace


// Check that _Cilk_spawn is used only:
//  - as the entire body of an expression statement,
//  - as the entire right hand side of an assignment expression that is the
//    entire body of an expression statement, or
//  - as the entire initializer-clause in a simple declaration.
//
// Since this is run per-compound scope stmt, we don't traverse into sub-
// compound scopes, but we do need to traverse into loops, ifs, etc. in case of:
// if (cond) _Cilk_spawn foo();
//           ^~~~~~~~~~~~~~~~~ not a compound scope
void Sema::DiagnoseCilkSpawn(Stmt *S, bool &HasError) {
  DiagnoseCilkSpawnHelper D(*this, HasError);

  VarDecl *LHS = 0;
  Expr *RHS = 0;
  switch (S->getStmtClass()) {
  case Stmt::CompoundStmtClass:
    return; // already checked
  case Stmt::CXXCatchStmtClass:
    DiagnoseCilkSpawn(cast<CXXCatchStmt>(S)->getHandlerBlock(), HasError);
    break;
  case Stmt::CXXForRangeStmtClass: {
    CXXForRangeStmt *FR = cast<CXXForRangeStmt>(S);
    D.TraverseStmt(FR->getRangeInit());
    DiagnoseCilkSpawn(FR->getBody(), HasError);
    break;
  }
  case Stmt::CXXBindTemporaryExprClass:
    DiagnoseCilkSpawn(cast<CXXBindTemporaryExpr>(S)->getSubExpr(), HasError);
    break;
  case Stmt::ExprWithCleanupsClass:
    DiagnoseCilkSpawn(cast<ExprWithCleanups>(S)->getSubExpr(), HasError);
    break;
  case Stmt::CXXTryStmtClass:
    DiagnoseCilkSpawn(cast<CXXTryStmt>(S)->getTryBlock(), HasError);
    break;
  case Stmt::DeclStmtClass: {
    DeclStmt *DS = cast<DeclStmt>(S);
    if (DS->isSingleDecl() && isa<VarDecl>(DS->getSingleDecl())) {
      VarDecl *VD = cast<VarDecl>(DS->getSingleDecl());
      if (VD->hasInit()) {
        LHS = VD;
        RHS = VD->getInit();
      }
    } else
      D.TraverseStmt(DS);
    break;
  }
  case Stmt::BinaryOperatorClass: {
    BinaryOperator *B = cast<BinaryOperator>(S);
    if (B->getOpcode() == BO_Assign) {
      D.TraverseStmt(B->getLHS());
      RHS = B->getRHS();
    } else
      D.TraverseStmt(B);
    break;
  }
  case Stmt::CXXOperatorCallExprClass: {
    CXXOperatorCallExpr *OC = cast<CXXOperatorCallExpr>(S);
    if (OC->isCilkSpawnCall()) {
      for (CallExpr::arg_iterator I = OC->arg_begin(),
           End = OC->arg_end(); I != End; ++I) {
        D.TraverseStmt(*I);
      }
    } else {
      if (OC->getOperator() == OO_Equal) {
        D.TraverseStmt(OC->getArg(0));
        RHS = OC->getArg(1);
      } else
        D.TraverseStmt(OC);
    }
    break;
  }
  case Stmt::DoStmtClass: {
    DoStmt *DS = cast<DoStmt>(S);
    D.TraverseStmt(DS->getCond());
    DiagnoseCilkSpawn(DS->getBody(), HasError);
    break;
  }
  case Stmt::ForStmtClass: {
    ForStmt *F = cast<ForStmt>(S);
    if (F->getInit())
      D.TraverseStmt(F->getInit());
    if (F->getCond())
      D.TraverseStmt(F->getCond());
    if (F->getInc())
      D.TraverseStmt(F->getInc());
    DiagnoseCilkSpawn(F->getBody(), HasError);
    break;
  }
  case Stmt::IfStmtClass: {
    IfStmt *I = cast<IfStmt>(S);
    D.TraverseStmt(I->getCond());
    DiagnoseCilkSpawn(I->getThen(), HasError);
    if (I->getElse())
      DiagnoseCilkSpawn(I->getElse(), HasError);
    break;
  }
  case Stmt::LabelStmtClass:
    DiagnoseCilkSpawn(cast<LabelStmt>(S)->getSubStmt(), HasError);
    break;
  case Stmt::CaseStmtClass:
  case Stmt::DefaultStmtClass:
    DiagnoseCilkSpawn(cast<SwitchCase>(S)->getSubStmt(), HasError);
    break;
  case Stmt::WhileStmtClass: {
    WhileStmt *W = cast<WhileStmt>(S);
    D.TraverseStmt(W->getCond());
    DiagnoseCilkSpawn(W->getBody(), HasError);
    break;
  }
  case Stmt::CXXMemberCallExprClass:
  case Stmt::CallExprClass: {
    CallExpr *C = cast<CallExpr>(S);
    if (C->isCilkSpawnCall() && C->isBuiltinCall())
      Diag(C->getCilkSpawnLoc(), diag::err_cannot_spawn_builtin)
           << C->getSourceRange();
    for (CallExpr::arg_iterator I = C->arg_begin(),
         End = C->arg_end(); I != End; ++I) {
      D.TraverseStmt(*I);
    }
    break;
  }
  default:
    D.TraverseStmt(S);
    break;
  }

  if (!RHS) return;

  if (LHS) {
    switch (LHS->getStorageClass()) {
    case SC_None:
    case SC_Auto:
    case SC_Register:
      break;
    case SC_Static:
      HasError = true;
      Diag(LHS->getLocation(), diag::err_cannot_init_static_variable)
        << LHS->getSourceRange();
      break;
    default:
      llvm_unreachable("variable with an unexpected storage class");
    }
  }

  // assignment or initializer
  // - the RHS may be wrapped in casts and/or involve object constructors
  while (true) {
    if (ImplicitCastExpr *E = dyn_cast<ImplicitCastExpr>(RHS))
      RHS = E->getSubExprAsWritten();
    else if (ExprWithCleanups *E = dyn_cast<ExprWithCleanups>(RHS))
      RHS = E->getSubExpr();
    else if (MaterializeTemporaryExpr *E = dyn_cast<MaterializeTemporaryExpr>(RHS))
      RHS = E->GetTemporaryExpr();
    else if (CXXBindTemporaryExpr *E = dyn_cast<CXXBindTemporaryExpr>(RHS))
      RHS = E->getSubExpr();
    else if (CXXConstructExpr *E = dyn_cast<CXXConstructExpr>(RHS)) {
      // CXXTempoaryObjectExpr represents a functional cast with != 1 arguments
      // so handle it the same way as CXXFunctionalCastExpr
      if (isa<CXXTemporaryObjectExpr>(E)) break;
      if (E->getNumArgs() >= 1)
        RHS = E->getArg(0);
      else break;
    } else
      break;
  }

  CallExpr *E = dyn_cast<CallExpr>(RHS);
  if (E && E->isCilkSpawnCall()) {
    if (E->isBuiltinCall())
      Diag(E->getCilkSpawnLoc(), diag::err_cannot_spawn_builtin)
           << E->getSourceRange();

    if (isa<UserDefinedLiteral>(E) || isa<CUDAKernelCallExpr>(E))
      Diag(E->getCilkSpawnLoc(), diag::err_cannot_spawn_function)
           << E->getSourceRange();

    for (CallExpr::arg_iterator I = E->arg_begin(),
         End = E->arg_end(); I != End; ++I) {
      D.TraverseStmt(*I);
    }
  } else
    D.TraverseStmt(RHS);
}

StmtResult
Sema::ActOnCompoundStmt(SourceLocation L, SourceLocation R,
                        MultiStmtArg elts, bool isStmtExpr) {
  unsigned NumElts = elts.size();
  Stmt **Elts = elts.data();
  // If we're in C89 mode, check that we don't have any decls after stmts.  If
  // so, emit an extension diagnostic.
  if (!getLangOpts().C99 && !getLangOpts().CPlusPlus) {
    // Note that __extension__ can be around a decl.
    unsigned i = 0;
    // Skip over all declarations.
    for (; i != NumElts && isa<DeclStmt>(Elts[i]); ++i)
      /*empty*/;

    // We found the end of the list or a statement.  Scan for another declstmt.
    for (; i != NumElts && !isa<DeclStmt>(Elts[i]); ++i)
      /*empty*/;

    if (i != NumElts) {
      Decl *D = *cast<DeclStmt>(Elts[i])->decl_begin();
      Diag(D->getLocation(), diag::ext_mixed_decls_code);
    }
  }

  // Warn about unused expressions in statements.
  for (unsigned i = 0; i != NumElts; ++i) {
    // Ignore statements that are last in a statement expression.
    if (isStmtExpr && i == NumElts - 1)
      continue;

    DiagnoseUnusedExprResult(Elts[i]);
  }

  // Check for suspicious empty body (null statement) in `for' and `while'
  // statements.  Don't do anything for template instantiations, this just adds
  // noise.
  if (NumElts != 0 && !CurrentInstantiationScope &&
      getCurCompoundScope().HasEmptyLoopBodies) {
    for (unsigned i = 0; i != NumElts - 1; ++i)
      DiagnoseEmptyLoopBody(Elts[i], Elts[i + 1]);
  }

  // If there are _Cilk_spawn expressions in this compound statement, check
  // whether they are used correctly.
  if (getCurCompoundScope().HasCilkSpawn) {
    // The function or method that has a spawn should emit a Cilk stack frame.
    DeclContext *DC = CurContext;
    while (!DC->isFunctionOrMethod())
      DC = DC->getParent();

    FunctionDecl::castFromDeclContext(DC)->setSpawning(true);

    assert(getLangOpts().CilkPlus && "_Cilk_spawn created without -fcilkplus");
    bool Dependent = CurContext->isDependentContext();
    for (unsigned i = 0; i != NumElts; ++i) {
      bool HasError = false;
      DiagnoseCilkSpawn(Elts[i], HasError);
      if (!Dependent && !HasError) {
        StmtResult Spawn = ActOnCilkSpawnStmt(Elts[i]);
        if (!Spawn.isInvalid() && isa<CilkSpawnCapturedStmt>(Spawn.get()))
          Elts[i] = Spawn.take();
      }
    }
  }

  return Owned(new (Context) CompoundStmt(Context,
                                          llvm::makeArrayRef(Elts, NumElts),
                                          L, R));
}

StmtResult
Sema::ActOnCaseStmt(SourceLocation CaseLoc, Expr *LHSVal,
                    SourceLocation DotDotDotLoc, Expr *RHSVal,
                    SourceLocation ColonLoc) {
  assert((LHSVal != 0) && "missing expression in case statement");

  if (getCurFunction()->SwitchStack.empty()) {
    Diag(CaseLoc, diag::err_case_not_in_switch);
    return StmtError();
  }

  if (!getLangOpts().CPlusPlus11) {
    // C99 6.8.4.2p3: The expression shall be an integer constant.
    // However, GCC allows any evaluatable integer expression.
    if (!LHSVal->isTypeDependent() && !LHSVal->isValueDependent()) {
      LHSVal = VerifyIntegerConstantExpression(LHSVal).take();
      if (!LHSVal)
        return StmtError();
    }

    // GCC extension: The expression shall be an integer constant.

    if (RHSVal && !RHSVal->isTypeDependent() && !RHSVal->isValueDependent()) {
      RHSVal = VerifyIntegerConstantExpression(RHSVal).take();
      // Recover from an error by just forgetting about it.
    }
  }
  
  LHSVal = ActOnFinishFullExpr(LHSVal, LHSVal->getExprLoc(), false,
                               getLangOpts().CPlusPlus11).take();
  if (RHSVal)
    RHSVal = ActOnFinishFullExpr(RHSVal, RHSVal->getExprLoc(), false,
                                 getLangOpts().CPlusPlus11).take();

  CaseStmt *CS = new (Context) CaseStmt(LHSVal, RHSVal, CaseLoc, DotDotDotLoc,
                                        ColonLoc);
  getCurFunction()->SwitchStack.back()->addSwitchCase(CS);
  return Owned(CS);
}

/// ActOnCaseStmtBody - This installs a statement as the body of a case.
void Sema::ActOnCaseStmtBody(Stmt *caseStmt, Stmt *SubStmt) {
  DiagnoseUnusedExprResult(SubStmt);

  CaseStmt *CS = static_cast<CaseStmt*>(caseStmt);
  CS->setSubStmt(SubStmt);
}

StmtResult
Sema::ActOnDefaultStmt(SourceLocation DefaultLoc, SourceLocation ColonLoc,
                       Stmt *SubStmt, Scope *CurScope) {
  DiagnoseUnusedExprResult(SubStmt);

  if (getCurFunction()->SwitchStack.empty()) {
    Diag(DefaultLoc, diag::err_default_not_in_switch);
    return Owned(SubStmt);
  }

  DefaultStmt *DS = new (Context) DefaultStmt(DefaultLoc, ColonLoc, SubStmt);
  getCurFunction()->SwitchStack.back()->addSwitchCase(DS);
  return Owned(DS);
}

StmtResult
Sema::ActOnLabelStmt(SourceLocation IdentLoc, LabelDecl *TheDecl,
                     SourceLocation ColonLoc, Stmt *SubStmt) {
  // If the label was multiply defined, reject it now.
  if (TheDecl->getStmt()) {
    Diag(IdentLoc, diag::err_redefinition_of_label) << TheDecl->getDeclName();
    Diag(TheDecl->getLocation(), diag::note_previous_definition);
    return Owned(SubStmt);
  }

  // Otherwise, things are good.  Fill in the declaration and return it.
  LabelStmt *LS = new (Context) LabelStmt(IdentLoc, TheDecl, SubStmt);
  TheDecl->setStmt(LS);
  if (!TheDecl->isGnuLocal()) {
    TheDecl->setLocStart(IdentLoc);
    TheDecl->setLocation(IdentLoc);
  }
  return Owned(LS);
}

StmtResult Sema::ActOnAttributedStmt(SourceLocation AttrLoc,
                                     ArrayRef<const Attr*> Attrs,
                                     Stmt *SubStmt) {
  // Fill in the declaration and return it.
  AttributedStmt *LS = AttributedStmt::Create(Context, AttrLoc, Attrs, SubStmt);
  return Owned(LS);
}

StmtResult
Sema::ActOnIfStmt(SourceLocation IfLoc, FullExprArg CondVal, Decl *CondVar,
                  Stmt *thenStmt, SourceLocation ElseLoc,
                  Stmt *elseStmt) {
  // If the condition was invalid, discard the if statement.  We could recover
  // better by replacing it with a valid expr, but don't do that yet.
  if (!CondVal.get() && !CondVar) {
    getCurFunction()->setHasDroppedStmt();
    return StmtError();
  }

  ExprResult CondResult(CondVal.release());

  VarDecl *ConditionVar = 0;
  if (CondVar) {
    ConditionVar = cast<VarDecl>(CondVar);
    CondResult = CheckConditionVariable(ConditionVar, IfLoc, true);
    if (CondResult.isInvalid())
      return StmtError();
  }
  Expr *ConditionExpr = CondResult.takeAs<Expr>();
  if (!ConditionExpr)
    return StmtError();

  DiagnoseUnusedExprResult(thenStmt);

  if (!elseStmt) {
    DiagnoseEmptyStmtBody(ConditionExpr->getLocEnd(), thenStmt,
                          diag::warn_empty_if_body);
  }

  DiagnoseUnusedExprResult(elseStmt);

  return Owned(new (Context) IfStmt(Context, IfLoc, ConditionVar, ConditionExpr,
                                    thenStmt, ElseLoc, elseStmt));
}

/// ConvertIntegerToTypeWarnOnOverflow - Convert the specified APInt to have
/// the specified width and sign.  If an overflow occurs, detect it and emit
/// the specified diagnostic.
void Sema::ConvertIntegerToTypeWarnOnOverflow(llvm::APSInt &Val,
                                              unsigned NewWidth, bool NewSign,
                                              SourceLocation Loc,
                                              unsigned DiagID) {
  // Perform a conversion to the promoted condition type if needed.
  if (NewWidth > Val.getBitWidth()) {
    // If this is an extension, just do it.
    Val = Val.extend(NewWidth);
    Val.setIsSigned(NewSign);

    // If the input was signed and negative and the output is
    // unsigned, don't bother to warn: this is implementation-defined
    // behavior.
    // FIXME: Introduce a second, default-ignored warning for this case?
  } else if (NewWidth < Val.getBitWidth()) {
    // If this is a truncation, check for overflow.
    llvm::APSInt ConvVal(Val);
    ConvVal = ConvVal.trunc(NewWidth);
    ConvVal.setIsSigned(NewSign);
    ConvVal = ConvVal.extend(Val.getBitWidth());
    ConvVal.setIsSigned(Val.isSigned());
    if (ConvVal != Val)
      Diag(Loc, DiagID) << Val.toString(10) << ConvVal.toString(10);

    // Regardless of whether a diagnostic was emitted, really do the
    // truncation.
    Val = Val.trunc(NewWidth);
    Val.setIsSigned(NewSign);
  } else if (NewSign != Val.isSigned()) {
    // Convert the sign to match the sign of the condition.  This can cause
    // overflow as well: unsigned(INTMIN)
    // We don't diagnose this overflow, because it is implementation-defined
    // behavior.
    // FIXME: Introduce a second, default-ignored warning for this case?
    llvm::APSInt OldVal(Val);
    Val.setIsSigned(NewSign);
  }
}

namespace {
  struct CaseCompareFunctor {
    bool operator()(const std::pair<llvm::APSInt, CaseStmt*> &LHS,
                    const llvm::APSInt &RHS) {
      return LHS.first < RHS;
    }
    bool operator()(const std::pair<llvm::APSInt, CaseStmt*> &LHS,
                    const std::pair<llvm::APSInt, CaseStmt*> &RHS) {
      return LHS.first < RHS.first;
    }
    bool operator()(const llvm::APSInt &LHS,
                    const std::pair<llvm::APSInt, CaseStmt*> &RHS) {
      return LHS < RHS.first;
    }
  };
}

/// CmpCaseVals - Comparison predicate for sorting case values.
///
static bool CmpCaseVals(const std::pair<llvm::APSInt, CaseStmt*>& lhs,
                        const std::pair<llvm::APSInt, CaseStmt*>& rhs) {
  if (lhs.first < rhs.first)
    return true;

  if (lhs.first == rhs.first &&
      lhs.second->getCaseLoc().getRawEncoding()
       < rhs.second->getCaseLoc().getRawEncoding())
    return true;
  return false;
}

/// CmpEnumVals - Comparison predicate for sorting enumeration values.
///
static bool CmpEnumVals(const std::pair<llvm::APSInt, EnumConstantDecl*>& lhs,
                        const std::pair<llvm::APSInt, EnumConstantDecl*>& rhs)
{
  return lhs.first < rhs.first;
}

/// EqEnumVals - Comparison preficate for uniqing enumeration values.
///
static bool EqEnumVals(const std::pair<llvm::APSInt, EnumConstantDecl*>& lhs,
                       const std::pair<llvm::APSInt, EnumConstantDecl*>& rhs)
{
  return lhs.first == rhs.first;
}

/// GetTypeBeforeIntegralPromotion - Returns the pre-promotion type of
/// potentially integral-promoted expression @p expr.
static QualType GetTypeBeforeIntegralPromotion(Expr *&expr) {
  if (ExprWithCleanups *cleanups = dyn_cast<ExprWithCleanups>(expr))
    expr = cleanups->getSubExpr();
  while (ImplicitCastExpr *impcast = dyn_cast<ImplicitCastExpr>(expr)) {
    if (impcast->getCastKind() != CK_IntegralCast) break;
    expr = impcast->getSubExpr();
  }
  return expr->getType();
}

StmtResult
Sema::ActOnStartOfSwitchStmt(SourceLocation SwitchLoc, Expr *Cond,
                             Decl *CondVar) {
  ExprResult CondResult;

  VarDecl *ConditionVar = 0;
  if (CondVar) {
    ConditionVar = cast<VarDecl>(CondVar);
    CondResult = CheckConditionVariable(ConditionVar, SourceLocation(), false);
    if (CondResult.isInvalid())
      return StmtError();

    Cond = CondResult.release();
  }

  if (!Cond)
    return StmtError();

  class SwitchConvertDiagnoser : public ICEConvertDiagnoser {
    Expr *Cond;

  public:
    SwitchConvertDiagnoser(Expr *Cond)
      : ICEConvertDiagnoser(false, true), Cond(Cond) { }

    virtual DiagnosticBuilder diagnoseNotInt(Sema &S, SourceLocation Loc,
                                             QualType T) {
      return S.Diag(Loc, diag::err_typecheck_statement_requires_integer) << T;
    }

    virtual DiagnosticBuilder diagnoseIncomplete(Sema &S, SourceLocation Loc,
                                                 QualType T) {
      return S.Diag(Loc, diag::err_switch_incomplete_class_type)
               << T << Cond->getSourceRange();
    }

    virtual DiagnosticBuilder diagnoseExplicitConv(Sema &S, SourceLocation Loc,
                                                   QualType T,
                                                   QualType ConvTy) {
      return S.Diag(Loc, diag::err_switch_explicit_conversion) << T << ConvTy;
    }

    virtual DiagnosticBuilder noteExplicitConv(Sema &S, CXXConversionDecl *Conv,
                                               QualType ConvTy) {
      return S.Diag(Conv->getLocation(), diag::note_switch_conversion)
        << ConvTy->isEnumeralType() << ConvTy;
    }

    virtual DiagnosticBuilder diagnoseAmbiguous(Sema &S, SourceLocation Loc,
                                                QualType T) {
      return S.Diag(Loc, diag::err_switch_multiple_conversions) << T;
    }

    virtual DiagnosticBuilder noteAmbiguous(Sema &S, CXXConversionDecl *Conv,
                                            QualType ConvTy) {
      return S.Diag(Conv->getLocation(), diag::note_switch_conversion)
      << ConvTy->isEnumeralType() << ConvTy;
    }

    virtual DiagnosticBuilder diagnoseConversion(Sema &S, SourceLocation Loc,
                                                 QualType T,
                                                 QualType ConvTy) {
      return DiagnosticBuilder::getEmpty();
    }
  } SwitchDiagnoser(Cond);

  CondResult
    = ConvertToIntegralOrEnumerationType(SwitchLoc, Cond, SwitchDiagnoser,
                                         /*AllowScopedEnumerations*/ true);
  if (CondResult.isInvalid()) return StmtError();
  Cond = CondResult.take();

  // C99 6.8.4.2p5 - Integer promotions are performed on the controlling expr.
  CondResult = UsualUnaryConversions(Cond);
  if (CondResult.isInvalid()) return StmtError();
  Cond = CondResult.take();

  if (!CondVar) {
    CondResult = ActOnFinishFullExpr(Cond, SwitchLoc);
    if (CondResult.isInvalid())
      return StmtError();
    Cond = CondResult.take();
  }

  getCurFunction()->setHasBranchIntoScope();

  SwitchStmt *SS = new (Context) SwitchStmt(Context, ConditionVar, Cond);
  getCurFunction()->SwitchStack.push_back(SS);
  return Owned(SS);
}

static void AdjustAPSInt(llvm::APSInt &Val, unsigned BitWidth, bool IsSigned) {
  if (Val.getBitWidth() < BitWidth)
    Val = Val.extend(BitWidth);
  else if (Val.getBitWidth() > BitWidth)
    Val = Val.trunc(BitWidth);
  Val.setIsSigned(IsSigned);
}

StmtResult
Sema::ActOnFinishSwitchStmt(SourceLocation SwitchLoc, Stmt *Switch,
                            Stmt *BodyStmt) {
  SwitchStmt *SS = cast<SwitchStmt>(Switch);
  assert(SS == getCurFunction()->SwitchStack.back() &&
         "switch stack missing push/pop!");

  SS->setBody(BodyStmt, SwitchLoc);
  getCurFunction()->SwitchStack.pop_back();

  Expr *CondExpr = SS->getCond();
  if (!CondExpr) return StmtError();

  QualType CondType = CondExpr->getType();

  Expr *CondExprBeforePromotion = CondExpr;
  QualType CondTypeBeforePromotion =
      GetTypeBeforeIntegralPromotion(CondExprBeforePromotion);

  // C++ 6.4.2.p2:
  // Integral promotions are performed (on the switch condition).
  //
  // A case value unrepresentable by the original switch condition
  // type (before the promotion) doesn't make sense, even when it can
  // be represented by the promoted type.  Therefore we need to find
  // the pre-promotion type of the switch condition.
  if (!CondExpr->isTypeDependent()) {
    // We have already converted the expression to an integral or enumeration
    // type, when we started the switch statement. If we don't have an
    // appropriate type now, just return an error.
    if (!CondType->isIntegralOrEnumerationType())
      return StmtError();

    if (CondExpr->isKnownToHaveBooleanValue()) {
      // switch(bool_expr) {...} is often a programmer error, e.g.
      //   switch(n && mask) { ... }  // Doh - should be "n & mask".
      // One can always use an if statement instead of switch(bool_expr).
      Diag(SwitchLoc, diag::warn_bool_switch_condition)
          << CondExpr->getSourceRange();
    }
  }

  // Get the bitwidth of the switched-on value before promotions.  We must
  // convert the integer case values to this width before comparison.
  bool HasDependentValue
    = CondExpr->isTypeDependent() || CondExpr->isValueDependent();
  unsigned CondWidth
    = HasDependentValue ? 0 : Context.getIntWidth(CondTypeBeforePromotion);
  bool CondIsSigned
    = CondTypeBeforePromotion->isSignedIntegerOrEnumerationType();

  // Accumulate all of the case values in a vector so that we can sort them
  // and detect duplicates.  This vector contains the APInt for the case after
  // it has been converted to the condition type.
  typedef SmallVector<std::pair<llvm::APSInt, CaseStmt*>, 64> CaseValsTy;
  CaseValsTy CaseVals;

  // Keep track of any GNU case ranges we see.  The APSInt is the low value.
  typedef std::vector<std::pair<llvm::APSInt, CaseStmt*> > CaseRangesTy;
  CaseRangesTy CaseRanges;

  DefaultStmt *TheDefaultStmt = 0;

  bool CaseListIsErroneous = false;

  for (SwitchCase *SC = SS->getSwitchCaseList(); SC && !HasDependentValue;
       SC = SC->getNextSwitchCase()) {

    if (DefaultStmt *DS = dyn_cast<DefaultStmt>(SC)) {
      if (TheDefaultStmt) {
        Diag(DS->getDefaultLoc(), diag::err_multiple_default_labels_defined);
        Diag(TheDefaultStmt->getDefaultLoc(), diag::note_duplicate_case_prev);

        // FIXME: Remove the default statement from the switch block so that
        // we'll return a valid AST.  This requires recursing down the AST and
        // finding it, not something we are set up to do right now.  For now,
        // just lop the entire switch stmt out of the AST.
        CaseListIsErroneous = true;
      }
      TheDefaultStmt = DS;

    } else {
      CaseStmt *CS = cast<CaseStmt>(SC);

      Expr *Lo = CS->getLHS();

      if (Lo->isTypeDependent() || Lo->isValueDependent()) {
        HasDependentValue = true;
        break;
      }

      llvm::APSInt LoVal;

      if (getLangOpts().CPlusPlus11) {
        // C++11 [stmt.switch]p2: the constant-expression shall be a converted
        // constant expression of the promoted type of the switch condition.
        ExprResult ConvLo =
          CheckConvertedConstantExpression(Lo, CondType, LoVal, CCEK_CaseValue);
        if (ConvLo.isInvalid()) {
          CaseListIsErroneous = true;
          continue;
        }
        Lo = ConvLo.take();
      } else {
        // We already verified that the expression has a i-c-e value (C99
        // 6.8.4.2p3) - get that value now.
        LoVal = Lo->EvaluateKnownConstInt(Context);

        // If the LHS is not the same type as the condition, insert an implicit
        // cast.
        Lo = DefaultLvalueConversion(Lo).take();
        Lo = ImpCastExprToType(Lo, CondType, CK_IntegralCast).take();
      }

      // Convert the value to the same width/sign as the condition had prior to
      // integral promotions.
      //
      // FIXME: This causes us to reject valid code:
      //   switch ((char)c) { case 256: case 0: return 0; }
      // Here we claim there is a duplicated condition value, but there is not.
      ConvertIntegerToTypeWarnOnOverflow(LoVal, CondWidth, CondIsSigned,
                                         Lo->getLocStart(),
                                         diag::warn_case_value_overflow);

      CS->setLHS(Lo);

      // If this is a case range, remember it in CaseRanges, otherwise CaseVals.
      if (CS->getRHS()) {
        if (CS->getRHS()->isTypeDependent() ||
            CS->getRHS()->isValueDependent()) {
          HasDependentValue = true;
          break;
        }
        CaseRanges.push_back(std::make_pair(LoVal, CS));
      } else
        CaseVals.push_back(std::make_pair(LoVal, CS));
    }
  }

  if (!HasDependentValue) {
    // If we don't have a default statement, check whether the
    // condition is constant.
    llvm::APSInt ConstantCondValue;
    bool HasConstantCond = false;
    if (!HasDependentValue && !TheDefaultStmt) {
      HasConstantCond
        = CondExprBeforePromotion->EvaluateAsInt(ConstantCondValue, Context,
                                                 Expr::SE_AllowSideEffects);
      assert(!HasConstantCond ||
             (ConstantCondValue.getBitWidth() == CondWidth &&
              ConstantCondValue.isSigned() == CondIsSigned));
    }
    bool ShouldCheckConstantCond = HasConstantCond;

    // Sort all the scalar case values so we can easily detect duplicates.
    std::stable_sort(CaseVals.begin(), CaseVals.end(), CmpCaseVals);

    if (!CaseVals.empty()) {
      for (unsigned i = 0, e = CaseVals.size(); i != e; ++i) {
        if (ShouldCheckConstantCond &&
            CaseVals[i].first == ConstantCondValue)
          ShouldCheckConstantCond = false;

        if (i != 0 && CaseVals[i].first == CaseVals[i-1].first) {
          // If we have a duplicate, report it.
          // First, determine if either case value has a name
          StringRef PrevString, CurrString;
          Expr *PrevCase = CaseVals[i-1].second->getLHS()->IgnoreParenCasts();
          Expr *CurrCase = CaseVals[i].second->getLHS()->IgnoreParenCasts();
          if (DeclRefExpr *DeclRef = dyn_cast<DeclRefExpr>(PrevCase)) {
            PrevString = DeclRef->getDecl()->getName();
          }
          if (DeclRefExpr *DeclRef = dyn_cast<DeclRefExpr>(CurrCase)) {
            CurrString = DeclRef->getDecl()->getName();
          }
          SmallString<16> CaseValStr;
          CaseVals[i-1].first.toString(CaseValStr);

          if (PrevString == CurrString)
            Diag(CaseVals[i].second->getLHS()->getLocStart(),
                 diag::err_duplicate_case) <<
                 (PrevString.empty() ? CaseValStr.str() : PrevString);
          else
            Diag(CaseVals[i].second->getLHS()->getLocStart(),
                 diag::err_duplicate_case_differing_expr) <<
                 (PrevString.empty() ? CaseValStr.str() : PrevString) <<
                 (CurrString.empty() ? CaseValStr.str() : CurrString) <<
                 CaseValStr;

          Diag(CaseVals[i-1].second->getLHS()->getLocStart(),
               diag::note_duplicate_case_prev);
          // FIXME: We really want to remove the bogus case stmt from the
          // substmt, but we have no way to do this right now.
          CaseListIsErroneous = true;
        }
      }
    }

    // Detect duplicate case ranges, which usually don't exist at all in
    // the first place.
    if (!CaseRanges.empty()) {
      // Sort all the case ranges by their low value so we can easily detect
      // overlaps between ranges.
      std::stable_sort(CaseRanges.begin(), CaseRanges.end());

      // Scan the ranges, computing the high values and removing empty ranges.
      std::vector<llvm::APSInt> HiVals;
      for (unsigned i = 0, e = CaseRanges.size(); i != e; ++i) {
        llvm::APSInt &LoVal = CaseRanges[i].first;
        CaseStmt *CR = CaseRanges[i].second;
        Expr *Hi = CR->getRHS();
        llvm::APSInt HiVal;

        if (getLangOpts().CPlusPlus11) {
          // C++11 [stmt.switch]p2: the constant-expression shall be a converted
          // constant expression of the promoted type of the switch condition.
          ExprResult ConvHi =
            CheckConvertedConstantExpression(Hi, CondType, HiVal,
                                             CCEK_CaseValue);
          if (ConvHi.isInvalid()) {
            CaseListIsErroneous = true;
            continue;
          }
          Hi = ConvHi.take();
        } else {
          HiVal = Hi->EvaluateKnownConstInt(Context);

          // If the RHS is not the same type as the condition, insert an
          // implicit cast.
          Hi = DefaultLvalueConversion(Hi).take();
          Hi = ImpCastExprToType(Hi, CondType, CK_IntegralCast).take();
        }

        // Convert the value to the same width/sign as the condition.
        ConvertIntegerToTypeWarnOnOverflow(HiVal, CondWidth, CondIsSigned,
                                           Hi->getLocStart(),
                                           diag::warn_case_value_overflow);

        CR->setRHS(Hi);

        // If the low value is bigger than the high value, the case is empty.
        if (LoVal > HiVal) {
          Diag(CR->getLHS()->getLocStart(), diag::warn_case_empty_range)
            << SourceRange(CR->getLHS()->getLocStart(),
                           Hi->getLocEnd());
          CaseRanges.erase(CaseRanges.begin()+i);
          --i, --e;
          continue;
        }

        if (ShouldCheckConstantCond &&
            LoVal <= ConstantCondValue &&
            ConstantCondValue <= HiVal)
          ShouldCheckConstantCond = false;

        HiVals.push_back(HiVal);
      }

      // Rescan the ranges, looking for overlap with singleton values and other
      // ranges.  Since the range list is sorted, we only need to compare case
      // ranges with their neighbors.
      for (unsigned i = 0, e = CaseRanges.size(); i != e; ++i) {
        llvm::APSInt &CRLo = CaseRanges[i].first;
        llvm::APSInt &CRHi = HiVals[i];
        CaseStmt *CR = CaseRanges[i].second;

        // Check to see whether the case range overlaps with any
        // singleton cases.
        CaseStmt *OverlapStmt = 0;
        llvm::APSInt OverlapVal(32);

        // Find the smallest value >= the lower bound.  If I is in the
        // case range, then we have overlap.
        CaseValsTy::iterator I = std::lower_bound(CaseVals.begin(),
                                                  CaseVals.end(), CRLo,
                                                  CaseCompareFunctor());
        if (I != CaseVals.end() && I->first < CRHi) {
          OverlapVal  = I->first;   // Found overlap with scalar.
          OverlapStmt = I->second;
        }

        // Find the smallest value bigger than the upper bound.
        I = std::upper_bound(I, CaseVals.end(), CRHi, CaseCompareFunctor());
        if (I != CaseVals.begin() && (I-1)->first >= CRLo) {
          OverlapVal  = (I-1)->first;      // Found overlap with scalar.
          OverlapStmt = (I-1)->second;
        }

        // Check to see if this case stmt overlaps with the subsequent
        // case range.
        if (i && CRLo <= HiVals[i-1]) {
          OverlapVal  = HiVals[i-1];       // Found overlap with range.
          OverlapStmt = CaseRanges[i-1].second;
        }

        if (OverlapStmt) {
          // If we have a duplicate, report it.
          Diag(CR->getLHS()->getLocStart(), diag::err_duplicate_case)
            << OverlapVal.toString(10);
          Diag(OverlapStmt->getLHS()->getLocStart(),
               diag::note_duplicate_case_prev);
          // FIXME: We really want to remove the bogus case stmt from the
          // substmt, but we have no way to do this right now.
          CaseListIsErroneous = true;
        }
      }
    }

    // Complain if we have a constant condition and we didn't find a match.
    if (!CaseListIsErroneous && ShouldCheckConstantCond) {
      // TODO: it would be nice if we printed enums as enums, chars as
      // chars, etc.
      Diag(CondExpr->getExprLoc(), diag::warn_missing_case_for_condition)
        << ConstantCondValue.toString(10)
        << CondExpr->getSourceRange();
    }

    // Check to see if switch is over an Enum and handles all of its
    // values.  We only issue a warning if there is not 'default:', but
    // we still do the analysis to preserve this information in the AST
    // (which can be used by flow-based analyes).
    //
    const EnumType *ET = CondTypeBeforePromotion->getAs<EnumType>();

    // If switch has default case, then ignore it.
    if (!CaseListIsErroneous  && !HasConstantCond && ET) {
      const EnumDecl *ED = ET->getDecl();
      typedef SmallVector<std::pair<llvm::APSInt, EnumConstantDecl*>, 64>
        EnumValsTy;
      EnumValsTy EnumVals;

      // Gather all enum values, set their type and sort them,
      // allowing easier comparison with CaseVals.
      for (EnumDecl::enumerator_iterator EDI = ED->enumerator_begin();
           EDI != ED->enumerator_end(); ++EDI) {
        llvm::APSInt Val = EDI->getInitVal();
        AdjustAPSInt(Val, CondWidth, CondIsSigned);
        EnumVals.push_back(std::make_pair(Val, *EDI));
      }
      std::stable_sort(EnumVals.begin(), EnumVals.end(), CmpEnumVals);
      EnumValsTy::iterator EIend =
        std::unique(EnumVals.begin(), EnumVals.end(), EqEnumVals);

      // See which case values aren't in enum.
      EnumValsTy::const_iterator EI = EnumVals.begin();
      for (CaseValsTy::const_iterator CI = CaseVals.begin();
           CI != CaseVals.end(); CI++) {
        while (EI != EIend && EI->first < CI->first)
          EI++;
        if (EI == EIend || EI->first > CI->first)
          Diag(CI->second->getLHS()->getExprLoc(), diag::warn_not_in_enum)
            << CondTypeBeforePromotion;
      }
      // See which of case ranges aren't in enum
      EI = EnumVals.begin();
      for (CaseRangesTy::const_iterator RI = CaseRanges.begin();
           RI != CaseRanges.end() && EI != EIend; RI++) {
        while (EI != EIend && EI->first < RI->first)
          EI++;

        if (EI == EIend || EI->first != RI->first) {
          Diag(RI->second->getLHS()->getExprLoc(), diag::warn_not_in_enum)
            << CondTypeBeforePromotion;
        }

        llvm::APSInt Hi =
          RI->second->getRHS()->EvaluateKnownConstInt(Context);
        AdjustAPSInt(Hi, CondWidth, CondIsSigned);
        while (EI != EIend && EI->first < Hi)
          EI++;
        if (EI == EIend || EI->first != Hi)
          Diag(RI->second->getRHS()->getExprLoc(), diag::warn_not_in_enum)
            << CondTypeBeforePromotion;
      }

      // Check which enum vals aren't in switch
      CaseValsTy::const_iterator CI = CaseVals.begin();
      CaseRangesTy::const_iterator RI = CaseRanges.begin();
      bool hasCasesNotInSwitch = false;

      SmallVector<DeclarationName,8> UnhandledNames;

      for (EI = EnumVals.begin(); EI != EIend; EI++){
        // Drop unneeded case values
        llvm::APSInt CIVal;
        while (CI != CaseVals.end() && CI->first < EI->first)
          CI++;

        if (CI != CaseVals.end() && CI->first == EI->first)
          continue;

        // Drop unneeded case ranges
        for (; RI != CaseRanges.end(); RI++) {
          llvm::APSInt Hi =
            RI->second->getRHS()->EvaluateKnownConstInt(Context);
          AdjustAPSInt(Hi, CondWidth, CondIsSigned);
          if (EI->first <= Hi)
            break;
        }

        if (RI == CaseRanges.end() || EI->first < RI->first) {
          hasCasesNotInSwitch = true;
          UnhandledNames.push_back(EI->second->getDeclName());
        }
      }

      if (TheDefaultStmt && UnhandledNames.empty())
        Diag(TheDefaultStmt->getDefaultLoc(), diag::warn_unreachable_default);

      // Produce a nice diagnostic if multiple values aren't handled.
      switch (UnhandledNames.size()) {
      case 0: break;
      case 1:
        Diag(CondExpr->getExprLoc(), TheDefaultStmt
          ? diag::warn_def_missing_case1 : diag::warn_missing_case1)
          << UnhandledNames[0];
        break;
      case 2:
        Diag(CondExpr->getExprLoc(), TheDefaultStmt
          ? diag::warn_def_missing_case2 : diag::warn_missing_case2)
          << UnhandledNames[0] << UnhandledNames[1];
        break;
      case 3:
        Diag(CondExpr->getExprLoc(), TheDefaultStmt
          ? diag::warn_def_missing_case3 : diag::warn_missing_case3)
          << UnhandledNames[0] << UnhandledNames[1] << UnhandledNames[2];
        break;
      default:
        Diag(CondExpr->getExprLoc(), TheDefaultStmt
          ? diag::warn_def_missing_cases : diag::warn_missing_cases)
          << (unsigned)UnhandledNames.size()
          << UnhandledNames[0] << UnhandledNames[1] << UnhandledNames[2];
        break;
      }

      if (!hasCasesNotInSwitch)
        SS->setAllEnumCasesCovered();
    }
  }

  DiagnoseEmptyStmtBody(CondExpr->getLocEnd(), BodyStmt,
                        diag::warn_empty_switch_body);

  // FIXME: If the case list was broken is some way, we don't have a good system
  // to patch it up.  Instead, just return the whole substmt as broken.
  if (CaseListIsErroneous)
    return StmtError();

  return Owned(SS);
}

void
Sema::DiagnoseAssignmentEnum(QualType DstType, QualType SrcType,
                             Expr *SrcExpr) {
  unsigned DIAG = diag::warn_not_in_enum_assignement;
  if (Diags.getDiagnosticLevel(DIAG, SrcExpr->getExprLoc())
      == DiagnosticsEngine::Ignored)
    return;

  if (const EnumType *ET = DstType->getAs<EnumType>())
    if (!Context.hasSameType(SrcType, DstType) &&
        SrcType->isIntegerType()) {
      if (!SrcExpr->isTypeDependent() && !SrcExpr->isValueDependent() &&
          SrcExpr->isIntegerConstantExpr(Context)) {
        // Get the bitwidth of the enum value before promotions.
        unsigned DstWith = Context.getIntWidth(DstType);
        bool DstIsSigned = DstType->isSignedIntegerOrEnumerationType();

        llvm::APSInt RhsVal = SrcExpr->EvaluateKnownConstInt(Context);
        const EnumDecl *ED = ET->getDecl();
        typedef SmallVector<std::pair<llvm::APSInt, EnumConstantDecl*>, 64>
        EnumValsTy;
        EnumValsTy EnumVals;

        // Gather all enum values, set their type and sort them,
        // allowing easier comparison with rhs constant.
        for (EnumDecl::enumerator_iterator EDI = ED->enumerator_begin();
             EDI != ED->enumerator_end(); ++EDI) {
          llvm::APSInt Val = EDI->getInitVal();
          AdjustAPSInt(Val, DstWith, DstIsSigned);
          EnumVals.push_back(std::make_pair(Val, *EDI));
        }
        if (EnumVals.empty())
          return;
        std::stable_sort(EnumVals.begin(), EnumVals.end(), CmpEnumVals);
        EnumValsTy::iterator EIend =
        std::unique(EnumVals.begin(), EnumVals.end(), EqEnumVals);

        // See which case values aren't in enum.
        EnumValsTy::const_iterator EI = EnumVals.begin();
        while (EI != EIend && EI->first < RhsVal)
          EI++;
        if (EI == EIend || EI->first != RhsVal) {
          Diag(SrcExpr->getExprLoc(), diag::warn_not_in_enum_assignement)
          << DstType;
        }
      }
    }
}

StmtResult
Sema::ActOnWhileStmt(SourceLocation WhileLoc, FullExprArg Cond,
                     Decl *CondVar, Stmt *Body) {
  ExprResult CondResult(Cond.release());

  VarDecl *ConditionVar = 0;
  if (CondVar) {
    ConditionVar = cast<VarDecl>(CondVar);
    CondResult = CheckConditionVariable(ConditionVar, WhileLoc, true);
    if (CondResult.isInvalid())
      return StmtError();
  }
  Expr *ConditionExpr = CondResult.take();
  if (!ConditionExpr)
    return StmtError();

  DiagnoseUnusedExprResult(Body);

  if (isa<NullStmt>(Body))
    getCurCompoundScope().setHasEmptyLoopBodies();

  return Owned(new (Context) WhileStmt(Context, ConditionVar, ConditionExpr,
                                       Body, WhileLoc));
}

StmtResult
Sema::ActOnDoStmt(SourceLocation DoLoc, Stmt *Body,
                  SourceLocation WhileLoc, SourceLocation CondLParen,
                  Expr *Cond, SourceLocation CondRParen) {
  assert(Cond && "ActOnDoStmt(): missing expression");

  ExprResult CondResult = CheckBooleanCondition(Cond, DoLoc);
  if (CondResult.isInvalid())
    return StmtError();
  Cond = CondResult.take();

  CondResult = ActOnFinishFullExpr(Cond, DoLoc);
  if (CondResult.isInvalid())
    return StmtError();
  Cond = CondResult.take();

  DiagnoseUnusedExprResult(Body);

  return Owned(new (Context) DoStmt(Body, Cond, DoLoc, WhileLoc, CondRParen));
}

namespace {
  // This visitor will traverse a conditional statement and store all
  // the evaluated decls into a vector.  Simple is set to true if none
  // of the excluded constructs are used.
  class DeclExtractor : public EvaluatedExprVisitor<DeclExtractor> {
    llvm::SmallPtrSet<VarDecl*, 8> &Decls;
    SmallVector<SourceRange, 10> &Ranges;
    bool Simple;
public:
  typedef EvaluatedExprVisitor<DeclExtractor> Inherited;

  DeclExtractor(Sema &S, llvm::SmallPtrSet<VarDecl*, 8> &Decls,
                SmallVector<SourceRange, 10> &Ranges) :
      Inherited(S.Context),
      Decls(Decls),
      Ranges(Ranges),
      Simple(true) {}

  bool isSimple() { return Simple; }

  // Replaces the method in EvaluatedExprVisitor.
  void VisitMemberExpr(MemberExpr* E) {
    Simple = false;
  }

  // Any Stmt not whitelisted will cause the condition to be marked complex.
  void VisitStmt(Stmt *S) {
    Simple = false;
  }

  void VisitBinaryOperator(BinaryOperator *E) {
    Visit(E->getLHS());
    Visit(E->getRHS());
  }

  void VisitCastExpr(CastExpr *E) {
    Visit(E->getSubExpr());
  }

  void VisitUnaryOperator(UnaryOperator *E) {
    // Skip checking conditionals with derefernces.
    if (E->getOpcode() == UO_Deref)
      Simple = false;
    else
      Visit(E->getSubExpr());
  }

  void VisitConditionalOperator(ConditionalOperator *E) {
    Visit(E->getCond());
    Visit(E->getTrueExpr());
    Visit(E->getFalseExpr());
  }

  void VisitParenExpr(ParenExpr *E) {
    Visit(E->getSubExpr());
  }

  void VisitBinaryConditionalOperator(BinaryConditionalOperator *E) {
    Visit(E->getOpaqueValue()->getSourceExpr());
    Visit(E->getFalseExpr());
  }

  void VisitIntegerLiteral(IntegerLiteral *E) { }
  void VisitFloatingLiteral(FloatingLiteral *E) { }
  void VisitCXXBoolLiteralExpr(CXXBoolLiteralExpr *E) { }
  void VisitCharacterLiteral(CharacterLiteral *E) { }
  void VisitGNUNullExpr(GNUNullExpr *E) { }
  void VisitImaginaryLiteral(ImaginaryLiteral *E) { }

  void VisitDeclRefExpr(DeclRefExpr *E) {
    VarDecl *VD = dyn_cast<VarDecl>(E->getDecl());
    if (!VD) return;

    Ranges.push_back(E->getSourceRange());

    Decls.insert(VD);
  }

  }; // end class DeclExtractor

  // DeclMatcher checks to see if the decls are used in a non-evauluated
  // context.
  class DeclMatcher : public EvaluatedExprVisitor<DeclMatcher> {
    llvm::SmallPtrSet<VarDecl*, 8> &Decls;
    bool FoundDecl;

public:
  typedef EvaluatedExprVisitor<DeclMatcher> Inherited;

  DeclMatcher(Sema &S, llvm::SmallPtrSet<VarDecl*, 8> &Decls, Stmt *Statement) :
      Inherited(S.Context), Decls(Decls), FoundDecl(false) {
    if (!Statement) return;

    Visit(Statement);
  }

  void VisitReturnStmt(ReturnStmt *S) {
    FoundDecl = true;
  }

  void VisitBreakStmt(BreakStmt *S) {
    FoundDecl = true;
  }

  void VisitGotoStmt(GotoStmt *S) {
    FoundDecl = true;
  }

  void VisitCastExpr(CastExpr *E) {
    if (E->getCastKind() == CK_LValueToRValue)
      CheckLValueToRValueCast(E->getSubExpr());
    else
      Visit(E->getSubExpr());
  }

  void CheckLValueToRValueCast(Expr *E) {
    E = E->IgnoreParenImpCasts();

    if (isa<DeclRefExpr>(E)) {
      return;
    }

    if (ConditionalOperator *CO = dyn_cast<ConditionalOperator>(E)) {
      Visit(CO->getCond());
      CheckLValueToRValueCast(CO->getTrueExpr());
      CheckLValueToRValueCast(CO->getFalseExpr());
      return;
    }

    if (BinaryConditionalOperator *BCO =
            dyn_cast<BinaryConditionalOperator>(E)) {
      CheckLValueToRValueCast(BCO->getOpaqueValue()->getSourceExpr());
      CheckLValueToRValueCast(BCO->getFalseExpr());
      return;
    }

    Visit(E);
  }

  void VisitDeclRefExpr(DeclRefExpr *E) {
    if (VarDecl *VD = dyn_cast<VarDecl>(E->getDecl()))
      if (Decls.count(VD))
        FoundDecl = true;
  }

  bool FoundDeclInUse() { return FoundDecl; }

  };  // end class DeclMatcher

  void CheckForLoopConditionalStatement(Sema &S, Expr *Second,
                                        Expr *Third, Stmt *Body) {
    // Condition is empty
    if (!Second) return;

    if (S.Diags.getDiagnosticLevel(diag::warn_variables_not_in_loop_body,
                                   Second->getLocStart())
        == DiagnosticsEngine::Ignored)
      return;

    PartialDiagnostic PDiag = S.PDiag(diag::warn_variables_not_in_loop_body);
    llvm::SmallPtrSet<VarDecl*, 8> Decls;
    SmallVector<SourceRange, 10> Ranges;
    DeclExtractor DE(S, Decls, Ranges);
    DE.Visit(Second);

    // Don't analyze complex conditionals.
    if (!DE.isSimple()) return;

    // No decls found.
    if (Decls.size() == 0) return;

    // Don't warn on volatile, static, or global variables.
    for (llvm::SmallPtrSet<VarDecl*, 8>::iterator I = Decls.begin(),
                                                  E = Decls.end();
         I != E; ++I)
      if ((*I)->getType().isVolatileQualified() ||
          (*I)->hasGlobalStorage()) return;

    if (DeclMatcher(S, Decls, Second).FoundDeclInUse() ||
        DeclMatcher(S, Decls, Third).FoundDeclInUse() ||
        DeclMatcher(S, Decls, Body).FoundDeclInUse())
      return;

    // Load decl names into diagnostic.
    if (Decls.size() > 4)
      PDiag << 0;
    else {
      PDiag << Decls.size();
      for (llvm::SmallPtrSet<VarDecl*, 8>::iterator I = Decls.begin(),
                                                    E = Decls.end();
           I != E; ++I)
        PDiag << (*I)->getDeclName();
    }

    // Load SourceRanges into diagnostic if there is room.
    // Otherwise, load the SourceRange of the conditional expression.
    if (Ranges.size() <= PartialDiagnostic::MaxArguments)
      for (SmallVector<SourceRange, 10>::iterator I = Ranges.begin(),
                                                  E = Ranges.end();
           I != E; ++I)
        PDiag << *I;
    else
      PDiag << Second->getSourceRange();

    S.Diag(Ranges.begin()->getBegin(), PDiag);
  }

} // end namespace

StmtResult
Sema::ActOnForStmt(SourceLocation ForLoc, SourceLocation LParenLoc,
                   Stmt *First, FullExprArg second, Decl *secondVar,
                   FullExprArg third,
                   SourceLocation RParenLoc, Stmt *Body) {
  if (!getLangOpts().CPlusPlus) {
    if (DeclStmt *DS = dyn_cast_or_null<DeclStmt>(First)) {
      // C99 6.8.5p3: The declaration part of a 'for' statement shall only
      // declare identifiers for objects having storage class 'auto' or
      // 'register'.
      for (DeclStmt::decl_iterator DI=DS->decl_begin(), DE=DS->decl_end();
           DI!=DE; ++DI) {
        VarDecl *VD = dyn_cast<VarDecl>(*DI);
        if (VD && VD->isLocalVarDecl() && !VD->hasLocalStorage())
          VD = 0;
        if (VD == 0) {
          Diag((*DI)->getLocation(), diag::err_non_local_variable_decl_in_for);
          (*DI)->setInvalidDecl();
        }
      }
    }
  }

  CheckForLoopConditionalStatement(*this, second.get(), third.get(), Body);

  ExprResult SecondResult(second.release());
  VarDecl *ConditionVar = 0;
  if (secondVar) {
    ConditionVar = cast<VarDecl>(secondVar);
    SecondResult = CheckConditionVariable(ConditionVar, ForLoc, true);
    if (SecondResult.isInvalid())
      return StmtError();
  }

  Expr *Third  = third.release().takeAs<Expr>();

  DiagnoseUnusedExprResult(First);
  DiagnoseUnusedExprResult(Third);
  DiagnoseUnusedExprResult(Body);

  if (isa<NullStmt>(Body))
    getCurCompoundScope().setHasEmptyLoopBodies();

  return Owned(new (Context) ForStmt(Context, First,
                                     SecondResult.take(), ConditionVar,
                                     Third, Body, ForLoc, LParenLoc,
                                     RParenLoc));
}

/// In an Objective C collection iteration statement:
///   for (x in y)
/// x can be an arbitrary l-value expression.  Bind it up as a
/// full-expression.
StmtResult Sema::ActOnForEachLValueExpr(Expr *E) {
  // Reduce placeholder expressions here.  Note that this rejects the
  // use of pseudo-object l-values in this position.
  ExprResult result = CheckPlaceholderExpr(E);
  if (result.isInvalid()) return StmtError();
  E = result.take();

  ExprResult FullExpr = ActOnFinishFullExpr(E);
  if (FullExpr.isInvalid())
    return StmtError();
  return StmtResult(static_cast<Stmt*>(FullExpr.take()));
}

ExprResult
Sema::CheckObjCForCollectionOperand(SourceLocation forLoc, Expr *collection) {
  if (!collection)
    return ExprError();

  // Bail out early if we've got a type-dependent expression.
  if (collection->isTypeDependent()) return Owned(collection);

  // Perform normal l-value conversion.
  ExprResult result = DefaultFunctionArrayLvalueConversion(collection);
  if (result.isInvalid())
    return ExprError();
  collection = result.take();

  // The operand needs to have object-pointer type.
  // TODO: should we do a contextual conversion?
  const ObjCObjectPointerType *pointerType =
    collection->getType()->getAs<ObjCObjectPointerType>();
  if (!pointerType)
    return Diag(forLoc, diag::err_collection_expr_type)
             << collection->getType() << collection->getSourceRange();

  // Check that the operand provides
  //   - countByEnumeratingWithState:objects:count:
  const ObjCObjectType *objectType = pointerType->getObjectType();
  ObjCInterfaceDecl *iface = objectType->getInterface();

  // If we have a forward-declared type, we can't do this check.
  // Under ARC, it is an error not to have a forward-declared class.
  if (iface &&
      RequireCompleteType(forLoc, QualType(objectType, 0),
                          getLangOpts().ObjCAutoRefCount
                            ? diag::err_arc_collection_forward
                            : 0,
                          collection)) {
    // Otherwise, if we have any useful type information, check that
    // the type declares the appropriate method.
  } else if (iface || !objectType->qual_empty()) {
    IdentifierInfo *selectorIdents[] = {
      &Context.Idents.get("countByEnumeratingWithState"),
      &Context.Idents.get("objects"),
      &Context.Idents.get("count")
    };
    Selector selector = Context.Selectors.getSelector(3, &selectorIdents[0]);

    ObjCMethodDecl *method = 0;

    // If there's an interface, look in both the public and private APIs.
    if (iface) {
      method = iface->lookupInstanceMethod(selector);
      if (!method) method = iface->lookupPrivateMethod(selector);
    }

    // Also check protocol qualifiers.
    if (!method)
      method = LookupMethodInQualifiedType(selector, pointerType,
                                           /*instance*/ true);

    // If we didn't find it anywhere, give up.
    if (!method) {
      Diag(forLoc, diag::warn_collection_expr_type)
        << collection->getType() << selector << collection->getSourceRange();
    }

    // TODO: check for an incompatible signature?
  }

  // Wrap up any cleanups in the expression.
  return Owned(collection);
}

StmtResult
Sema::ActOnObjCForCollectionStmt(SourceLocation ForLoc,
                                 Stmt *First, Expr *collection,
                                 SourceLocation RParenLoc) {

  ExprResult CollectionExprResult =
    CheckObjCForCollectionOperand(ForLoc, collection);

  if (First) {
    QualType FirstType;
    if (DeclStmt *DS = dyn_cast<DeclStmt>(First)) {
      if (!DS->isSingleDecl())
        return StmtError(Diag((*DS->decl_begin())->getLocation(),
                         diag::err_toomany_element_decls));

      VarDecl *D = dyn_cast<VarDecl>(DS->getSingleDecl());
      if (!D || D->isInvalidDecl())
        return StmtError();
      
      FirstType = D->getType();
      // C99 6.8.5p3: The declaration part of a 'for' statement shall only
      // declare identifiers for objects having storage class 'auto' or
      // 'register'.
      if (!D->hasLocalStorage())
        return StmtError(Diag(D->getLocation(),
                              diag::err_non_local_variable_decl_in_for));

      // If the type contained 'auto', deduce the 'auto' to 'id'.
      if (FirstType->getContainedAutoType()) {
        TypeSourceInfo *DeducedType = 0;
        OpaqueValueExpr OpaqueId(D->getLocation(), Context.getObjCIdType(),
                                 VK_RValue);
        Expr *DeducedInit = &OpaqueId;
        if (DeduceAutoType(D->getTypeSourceInfo(), DeducedInit, DeducedType)
              == DAR_Failed) {
          DiagnoseAutoDeductionFailure(D, DeducedInit);
        }
        if (!DeducedType) {
          D->setInvalidDecl();
          return StmtError();
        }

        D->setTypeSourceInfo(DeducedType);
        D->setType(DeducedType->getType());
        FirstType = DeducedType->getType();

        if (ActiveTemplateInstantiations.empty()) {
          SourceLocation Loc = DeducedType->getTypeLoc().getBeginLoc();
          Diag(Loc, diag::warn_auto_var_is_id)
            << D->getDeclName();
        }
      }

    } else {
      Expr *FirstE = cast<Expr>(First);
      if (!FirstE->isTypeDependent() && !FirstE->isLValue())
        return StmtError(Diag(First->getLocStart(),
                   diag::err_selector_element_not_lvalue)
          << First->getSourceRange());

      FirstType = static_cast<Expr*>(First)->getType();
    }
    if (!FirstType->isDependentType() &&
        !FirstType->isObjCObjectPointerType() &&
        !FirstType->isBlockPointerType())
        return StmtError(Diag(ForLoc, diag::err_selector_element_type)
                           << FirstType << First->getSourceRange());
  }

  if (CollectionExprResult.isInvalid())
    return StmtError();

  CollectionExprResult = ActOnFinishFullExpr(CollectionExprResult.take());
  if (CollectionExprResult.isInvalid())
    return StmtError();

  return Owned(new (Context) ObjCForCollectionStmt(First,
                                                   CollectionExprResult.take(), 0,
                                                   ForLoc, RParenLoc));
}

/// Finish building a variable declaration for a for-range statement.
/// \return true if an error occurs.
static bool FinishForRangeVarDecl(Sema &SemaRef, VarDecl *Decl, Expr *Init,
                                  SourceLocation Loc, int diag) {
  // Deduce the type for the iterator variable now rather than leaving it to
  // AddInitializerToDecl, so we can produce a more suitable diagnostic.
  TypeSourceInfo *InitTSI = 0;
  if ((!isa<InitListExpr>(Init) && Init->getType()->isVoidType()) ||
      SemaRef.DeduceAutoType(Decl->getTypeSourceInfo(), Init, InitTSI) ==
          Sema::DAR_Failed)
    SemaRef.Diag(Loc, diag) << Init->getType();
  if (!InitTSI) {
    Decl->setInvalidDecl();
    return true;
  }
  Decl->setTypeSourceInfo(InitTSI);
  Decl->setType(InitTSI->getType());

  // In ARC, infer lifetime.
  // FIXME: ARC may want to turn this into 'const __unsafe_unretained' if
  // we're doing the equivalent of fast iteration.
  if (SemaRef.getLangOpts().ObjCAutoRefCount &&
      SemaRef.inferObjCARCLifetime(Decl))
    Decl->setInvalidDecl();

  SemaRef.AddInitializerToDecl(Decl, Init, /*DirectInit=*/false,
                               /*TypeMayContainAuto=*/false);
  SemaRef.FinalizeDeclaration(Decl);
  SemaRef.CurContext->addHiddenDecl(Decl);
  return false;
}

namespace {

/// Produce a note indicating which begin/end function was implicitly called
/// by a C++11 for-range statement. This is often not obvious from the code,
/// nor from the diagnostics produced when analysing the implicit expressions
/// required in a for-range statement.
void NoteForRangeBeginEndFunction(Sema &SemaRef, Expr *E,
                                  Sema::BeginEndFunction BEF) {
  CallExpr *CE = dyn_cast<CallExpr>(E);
  if (!CE)
    return;
  FunctionDecl *D = dyn_cast<FunctionDecl>(CE->getCalleeDecl());
  if (!D)
    return;
  SourceLocation Loc = D->getLocation();

  std::string Description;
  bool IsTemplate = false;
  if (FunctionTemplateDecl *FunTmpl = D->getPrimaryTemplate()) {
    Description = SemaRef.getTemplateArgumentBindingsText(
      FunTmpl->getTemplateParameters(), *D->getTemplateSpecializationArgs());
    IsTemplate = true;
  }

  SemaRef.Diag(Loc, diag::note_for_range_begin_end)
    << BEF << IsTemplate << Description << E->getType();
}

/// Build a variable declaration for a for-range statement.
VarDecl *BuildForRangeVarDecl(Sema &SemaRef, SourceLocation Loc,
                              QualType Type, const char *Name) {
  DeclContext *DC = SemaRef.CurContext;
  IdentifierInfo *II = &SemaRef.PP.getIdentifierTable().get(Name);
  TypeSourceInfo *TInfo = SemaRef.Context.getTrivialTypeSourceInfo(Type, Loc);
  VarDecl *Decl = VarDecl::Create(SemaRef.Context, DC, Loc, Loc, II, Type,
                                  TInfo, SC_None);
  Decl->setImplicit();
  return Decl;
}

}

static bool ObjCEnumerationCollection(Expr *Collection) {
  return !Collection->isTypeDependent()
          && Collection->getType()->getAs<ObjCObjectPointerType>() != 0;
}

/// ActOnCXXForRangeStmt - Check and build a C++11 for-range statement.
///
/// C++11 [stmt.ranged]:
///   A range-based for statement is equivalent to
///
///   {
///     auto && __range = range-init;
///     for ( auto __begin = begin-expr,
///           __end = end-expr;
///           __begin != __end;
///           ++__begin ) {
///       for-range-declaration = *__begin;
///       statement
///     }
///   }
///
/// The body of the loop is not available yet, since it cannot be analysed until
/// we have determined the type of the for-range-declaration.
StmtResult
Sema::ActOnCXXForRangeStmt(SourceLocation ForLoc,
                           Stmt *First, SourceLocation ColonLoc, Expr *Range,
                           SourceLocation RParenLoc, BuildForRangeKind Kind) {
  if (!First || !Range)
    return StmtError();

  if (ObjCEnumerationCollection(Range))
    return ActOnObjCForCollectionStmt(ForLoc, First, Range, RParenLoc);

  DeclStmt *DS = dyn_cast<DeclStmt>(First);
  assert(DS && "first part of for range not a decl stmt");

  if (!DS->isSingleDecl()) {
    Diag(DS->getStartLoc(), diag::err_type_defined_in_for_range);
    return StmtError();
  }
  if (DS->getSingleDecl()->isInvalidDecl())
    return StmtError();

  if (DiagnoseUnexpandedParameterPack(Range, UPPC_Expression))
    return StmtError();

  // Build  auto && __range = range-init
  SourceLocation RangeLoc = Range->getLocStart();
  VarDecl *RangeVar = BuildForRangeVarDecl(*this, RangeLoc,
                                           Context.getAutoRRefDeductType(),
                                           "__range");
  if (FinishForRangeVarDecl(*this, RangeVar, Range, RangeLoc,
                            diag::err_for_range_deduction_failure))
    return StmtError();

  // Claim the type doesn't contain auto: we've already done the checking.
  DeclGroupPtrTy RangeGroup =
    BuildDeclaratorGroup((Decl**)&RangeVar, 1, /*TypeMayContainAuto=*/false);
  StmtResult RangeDecl = ActOnDeclStmt(RangeGroup, RangeLoc, RangeLoc);
  if (RangeDecl.isInvalid())
    return StmtError();

  return BuildCXXForRangeStmt(ForLoc, ColonLoc, RangeDecl.get(),
                              /*BeginEndDecl=*/0, /*Cond=*/0, /*Inc=*/0, DS,
                              RParenLoc, Kind);
}

/// \brief Create the initialization, compare, and increment steps for
/// the range-based for loop expression.
/// This function does not handle array-based for loops,
/// which are created in Sema::BuildCXXForRangeStmt.
///
/// \returns a ForRangeStatus indicating success or what kind of error occurred.
/// BeginExpr and EndExpr are set and FRS_Success is returned on success;
/// CandidateSet and BEF are set and some non-success value is returned on
/// failure.
static Sema::ForRangeStatus BuildNonArrayForRange(Sema &SemaRef, Scope *S,
                                            Expr *BeginRange, Expr *EndRange,
                                            QualType RangeType,
                                            VarDecl *BeginVar,
                                            VarDecl *EndVar,
                                            SourceLocation ColonLoc,
                                            OverloadCandidateSet *CandidateSet,
                                            ExprResult *BeginExpr,
                                            ExprResult *EndExpr,
                                            Sema::BeginEndFunction *BEF) {
  DeclarationNameInfo BeginNameInfo(
      &SemaRef.PP.getIdentifierTable().get("begin"), ColonLoc);
  DeclarationNameInfo EndNameInfo(&SemaRef.PP.getIdentifierTable().get("end"),
                                  ColonLoc);

  LookupResult BeginMemberLookup(SemaRef, BeginNameInfo,
                                 Sema::LookupMemberName);
  LookupResult EndMemberLookup(SemaRef, EndNameInfo, Sema::LookupMemberName);

  if (CXXRecordDecl *D = RangeType->getAsCXXRecordDecl()) {
    // - if _RangeT is a class type, the unqualified-ids begin and end are
    //   looked up in the scope of class _RangeT as if by class member access
    //   lookup (3.4.5), and if either (or both) finds at least one
    //   declaration, begin-expr and end-expr are __range.begin() and
    //   __range.end(), respectively;
    SemaRef.LookupQualifiedName(BeginMemberLookup, D);
    SemaRef.LookupQualifiedName(EndMemberLookup, D);

    if (BeginMemberLookup.empty() != EndMemberLookup.empty()) {
      SourceLocation RangeLoc = BeginVar->getLocation();
      *BEF = BeginMemberLookup.empty() ? Sema::BEF_end : Sema::BEF_begin;

      SemaRef.Diag(RangeLoc, diag::err_for_range_member_begin_end_mismatch)
          << RangeLoc << BeginRange->getType() << *BEF;
      return Sema::FRS_DiagnosticIssued;
    }
  } else {
    // - otherwise, begin-expr and end-expr are begin(__range) and
    //   end(__range), respectively, where begin and end are looked up with
    //   argument-dependent lookup (3.4.2). For the purposes of this name
    //   lookup, namespace std is an associated namespace.

  }

  *BEF = Sema::BEF_begin;
  Sema::ForRangeStatus RangeStatus =
      SemaRef.BuildForRangeBeginEndCall(S, ColonLoc, ColonLoc, BeginVar,
                                        Sema::BEF_begin, BeginNameInfo,
                                        BeginMemberLookup, CandidateSet,
                                        BeginRange, BeginExpr);

  if (RangeStatus != Sema::FRS_Success)
    return RangeStatus;
  if (FinishForRangeVarDecl(SemaRef, BeginVar, BeginExpr->get(), ColonLoc,
                            diag::err_for_range_iter_deduction_failure)) {
    NoteForRangeBeginEndFunction(SemaRef, BeginExpr->get(), *BEF);
    return Sema::FRS_DiagnosticIssued;
  }

  *BEF = Sema::BEF_end;
  RangeStatus =
      SemaRef.BuildForRangeBeginEndCall(S, ColonLoc, ColonLoc, EndVar,
                                        Sema::BEF_end, EndNameInfo,
                                        EndMemberLookup, CandidateSet,
                                        EndRange, EndExpr);
  if (RangeStatus != Sema::FRS_Success)
    return RangeStatus;
  if (FinishForRangeVarDecl(SemaRef, EndVar, EndExpr->get(), ColonLoc,
                            diag::err_for_range_iter_deduction_failure)) {
    NoteForRangeBeginEndFunction(SemaRef, EndExpr->get(), *BEF);
    return Sema::FRS_DiagnosticIssued;
  }
  return Sema::FRS_Success;
}

/// Speculatively attempt to dereference an invalid range expression.
/// If the attempt fails, this function will return a valid, null StmtResult
/// and emit no diagnostics.
static StmtResult RebuildForRangeWithDereference(Sema &SemaRef, Scope *S,
                                                 SourceLocation ForLoc,
                                                 Stmt *LoopVarDecl,
                                                 SourceLocation ColonLoc,
                                                 Expr *Range,
                                                 SourceLocation RangeLoc,
                                                 SourceLocation RParenLoc) {
  // Determine whether we can rebuild the for-range statement with a
  // dereferenced range expression.
  ExprResult AdjustedRange;
  {
    Sema::SFINAETrap Trap(SemaRef);

    AdjustedRange = SemaRef.BuildUnaryOp(S, RangeLoc, UO_Deref, Range);
    if (AdjustedRange.isInvalid())
      return StmtResult();

    StmtResult SR =
      SemaRef.ActOnCXXForRangeStmt(ForLoc, LoopVarDecl, ColonLoc,
                                   AdjustedRange.get(), RParenLoc,
                                   Sema::BFRK_Check);
    if (SR.isInvalid())
      return StmtResult();
  }

  // The attempt to dereference worked well enough that it could produce a valid
  // loop. Produce a fixit, and rebuild the loop with diagnostics enabled, in
  // case there are any other (non-fatal) problems with it.
  SemaRef.Diag(RangeLoc, diag::err_for_range_dereference)
    << Range->getType() << FixItHint::CreateInsertion(RangeLoc, "*");
  return SemaRef.ActOnCXXForRangeStmt(ForLoc, LoopVarDecl, ColonLoc,
                                      AdjustedRange.get(), RParenLoc,
                                      Sema::BFRK_Rebuild);
}

/// BuildCXXForRangeStmt - Build or instantiate a C++11 for-range statement.
StmtResult
Sema::BuildCXXForRangeStmt(SourceLocation ForLoc, SourceLocation ColonLoc,
                           Stmt *RangeDecl, Stmt *BeginEnd, Expr *Cond,
                           Expr *Inc, Stmt *LoopVarDecl,
                           SourceLocation RParenLoc, BuildForRangeKind Kind) {
  Scope *S = getCurScope();

  DeclStmt *RangeDS = cast<DeclStmt>(RangeDecl);
  VarDecl *RangeVar = cast<VarDecl>(RangeDS->getSingleDecl());
  QualType RangeVarType = RangeVar->getType();

  DeclStmt *LoopVarDS = cast<DeclStmt>(LoopVarDecl);
  VarDecl *LoopVar = cast<VarDecl>(LoopVarDS->getSingleDecl());

  StmtResult BeginEndDecl = BeginEnd;
  ExprResult NotEqExpr = Cond, IncrExpr = Inc;

  if (!BeginEndDecl.get() && !RangeVarType->isDependentType()) {
    SourceLocation RangeLoc = RangeVar->getLocation();

    const QualType RangeVarNonRefType = RangeVarType.getNonReferenceType();

    ExprResult BeginRangeRef = BuildDeclRefExpr(RangeVar, RangeVarNonRefType,
                                                VK_LValue, ColonLoc);
    if (BeginRangeRef.isInvalid())
      return StmtError();

    ExprResult EndRangeRef = BuildDeclRefExpr(RangeVar, RangeVarNonRefType,
                                              VK_LValue, ColonLoc);
    if (EndRangeRef.isInvalid())
      return StmtError();

    QualType AutoType = Context.getAutoDeductType();
    Expr *Range = RangeVar->getInit();
    if (!Range)
      return StmtError();
    QualType RangeType = Range->getType();

    if (RequireCompleteType(RangeLoc, RangeType,
                            diag::err_for_range_incomplete_type))
      return StmtError();

    // Build auto __begin = begin-expr, __end = end-expr.
    VarDecl *BeginVar = BuildForRangeVarDecl(*this, ColonLoc, AutoType,
                                             "__begin");
    VarDecl *EndVar = BuildForRangeVarDecl(*this, ColonLoc, AutoType,
                                           "__end");

    // Build begin-expr and end-expr and attach to __begin and __end variables.
    ExprResult BeginExpr, EndExpr;
    if (const ArrayType *UnqAT = RangeType->getAsArrayTypeUnsafe()) {
      // - if _RangeT is an array type, begin-expr and end-expr are __range and
      //   __range + __bound, respectively, where __bound is the array bound. If
      //   _RangeT is an array of unknown size or an array of incomplete type,
      //   the program is ill-formed;

      // begin-expr is __range.
      BeginExpr = BeginRangeRef;
      if (FinishForRangeVarDecl(*this, BeginVar, BeginRangeRef.get(), ColonLoc,
                                diag::err_for_range_iter_deduction_failure)) {
        NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
        return StmtError();
      }

      // Find the array bound.
      ExprResult BoundExpr;
      if (const ConstantArrayType *CAT = dyn_cast<ConstantArrayType>(UnqAT))
        BoundExpr = Owned(IntegerLiteral::Create(Context, CAT->getSize(),
                                                 Context.getPointerDiffType(),
                                                 RangeLoc));
      else if (const VariableArrayType *VAT =
               dyn_cast<VariableArrayType>(UnqAT))
        BoundExpr = VAT->getSizeExpr();
      else {
        // Can't be a DependentSizedArrayType or an IncompleteArrayType since
        // UnqAT is not incomplete and Range is not type-dependent.
        llvm_unreachable("Unexpected array type in for-range");
      }

      // end-expr is __range + __bound.
      EndExpr = ActOnBinOp(S, ColonLoc, tok::plus, EndRangeRef.get(),
                           BoundExpr.get());
      if (EndExpr.isInvalid())
        return StmtError();
      if (FinishForRangeVarDecl(*this, EndVar, EndExpr.get(), ColonLoc,
                                diag::err_for_range_iter_deduction_failure)) {
        NoteForRangeBeginEndFunction(*this, EndExpr.get(), BEF_end);
        return StmtError();
      }
    } else {
      OverloadCandidateSet CandidateSet(RangeLoc);
      Sema::BeginEndFunction BEFFailure;
      ForRangeStatus RangeStatus =
          BuildNonArrayForRange(*this, S, BeginRangeRef.get(),
                                EndRangeRef.get(), RangeType,
                                BeginVar, EndVar, ColonLoc, &CandidateSet,
                                &BeginExpr, &EndExpr, &BEFFailure);

      // If building the range failed, try dereferencing the range expression
      // unless a diagnostic was issued or the end function is problematic.
      if (Kind == BFRK_Build && RangeStatus == FRS_NoViableFunction &&
          BEFFailure == BEF_begin) {
        StmtResult SR = RebuildForRangeWithDereference(*this, S, ForLoc,
                                                       LoopVarDecl, ColonLoc,
                                                       Range, RangeLoc,
                                                       RParenLoc);
        if (SR.isInvalid() || SR.isUsable())
          return SR;
      }

      // Otherwise, emit diagnostics if we haven't already.
      if (RangeStatus == FRS_NoViableFunction) {
        Expr *Range = BEFFailure ? EndRangeRef.get() : BeginRangeRef.get();
        Diag(Range->getLocStart(), diag::err_for_range_invalid)
            << RangeLoc << Range->getType() << BEFFailure;
        CandidateSet.NoteCandidates(*this, OCD_AllCandidates, Range);
      }
      // Return an error if no fix was discovered.
      if (RangeStatus != FRS_Success)
        return StmtError();
    }

    assert(!BeginExpr.isInvalid() && !EndExpr.isInvalid() &&
           "invalid range expression in for loop");

    // C++11 [dcl.spec.auto]p7: BeginType and EndType must be the same.
    QualType BeginType = BeginVar->getType(), EndType = EndVar->getType();
    if (!Context.hasSameType(BeginType, EndType)) {
      Diag(RangeLoc, diag::err_for_range_begin_end_types_differ)
        << BeginType << EndType;
      NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
      NoteForRangeBeginEndFunction(*this, EndExpr.get(), BEF_end);
    }

    Decl *BeginEndDecls[] = { BeginVar, EndVar };
    // Claim the type doesn't contain auto: we've already done the checking.
    DeclGroupPtrTy BeginEndGroup =
      BuildDeclaratorGroup(BeginEndDecls, 2, /*TypeMayContainAuto=*/false);
    BeginEndDecl = ActOnDeclStmt(BeginEndGroup, ColonLoc, ColonLoc);

    const QualType BeginRefNonRefType = BeginType.getNonReferenceType();
    ExprResult BeginRef = BuildDeclRefExpr(BeginVar, BeginRefNonRefType,
                                           VK_LValue, ColonLoc);
    if (BeginRef.isInvalid())
      return StmtError();

    ExprResult EndRef = BuildDeclRefExpr(EndVar, EndType.getNonReferenceType(),
                                         VK_LValue, ColonLoc);
    if (EndRef.isInvalid())
      return StmtError();

    // Build and check __begin != __end expression.
    NotEqExpr = ActOnBinOp(S, ColonLoc, tok::exclaimequal,
                           BeginRef.get(), EndRef.get());
    NotEqExpr = ActOnBooleanCondition(S, ColonLoc, NotEqExpr.get());
    NotEqExpr = ActOnFinishFullExpr(NotEqExpr.get());
    if (NotEqExpr.isInvalid()) {
      Diag(RangeLoc, diag::note_for_range_invalid_iterator)
        << RangeLoc << 0 << BeginRangeRef.get()->getType();
      NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
      if (!Context.hasSameType(BeginType, EndType))
        NoteForRangeBeginEndFunction(*this, EndExpr.get(), BEF_end);
      return StmtError();
    }

    // Build and check ++__begin expression.
    BeginRef = BuildDeclRefExpr(BeginVar, BeginRefNonRefType,
                                VK_LValue, ColonLoc);
    if (BeginRef.isInvalid())
      return StmtError();

    IncrExpr = ActOnUnaryOp(S, ColonLoc, tok::plusplus, BeginRef.get());
    IncrExpr = ActOnFinishFullExpr(IncrExpr.get());
    if (IncrExpr.isInvalid()) {
      Diag(RangeLoc, diag::note_for_range_invalid_iterator)
        << RangeLoc << 2 << BeginRangeRef.get()->getType() ;
      NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
      return StmtError();
    }

    // Build and check *__begin  expression.
    BeginRef = BuildDeclRefExpr(BeginVar, BeginRefNonRefType,
                                VK_LValue, ColonLoc);
    if (BeginRef.isInvalid())
      return StmtError();

    ExprResult DerefExpr = ActOnUnaryOp(S, ColonLoc, tok::star, BeginRef.get());
    if (DerefExpr.isInvalid()) {
      Diag(RangeLoc, diag::note_for_range_invalid_iterator)
        << RangeLoc << 1 << BeginRangeRef.get()->getType();
      NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
      return StmtError();
    }

    // Attach  *__begin  as initializer for VD. Don't touch it if we're just
    // trying to determine whether this would be a valid range.
    if (!LoopVar->isInvalidDecl() && Kind != BFRK_Check) {
      AddInitializerToDecl(LoopVar, DerefExpr.get(), /*DirectInit=*/false,
                           /*TypeMayContainAuto=*/true);
      if (LoopVar->isInvalidDecl())
        NoteForRangeBeginEndFunction(*this, BeginExpr.get(), BEF_begin);
    }
  } else {
    // The range is implicitly used as a placeholder when it is dependent.
    RangeVar->setUsed();
  }

  // Don't bother to actually allocate the result if we're just trying to
  // determine whether it would be valid.
  if (Kind == BFRK_Check)
    return StmtResult();

  return Owned(new (Context) CXXForRangeStmt(RangeDS,
                                     cast_or_null<DeclStmt>(BeginEndDecl.get()),
                                             NotEqExpr.take(), IncrExpr.take(),
                                             LoopVarDS, /*Body=*/0, ForLoc,
                                             ColonLoc, RParenLoc));
}

/// FinishObjCForCollectionStmt - Attach the body to a objective-C foreach
/// statement.
StmtResult Sema::FinishObjCForCollectionStmt(Stmt *S, Stmt *B) {
  if (!S || !B)
    return StmtError();
  ObjCForCollectionStmt * ForStmt = cast<ObjCForCollectionStmt>(S);

  ForStmt->setBody(B);
  return S;
}

/// FinishCXXForRangeStmt - Attach the body to a C++0x for-range statement.
/// This is a separate step from ActOnCXXForRangeStmt because analysis of the
/// body cannot be performed until after the type of the range variable is
/// determined.
StmtResult Sema::FinishCXXForRangeStmt(Stmt *S, Stmt *B) {
  if (!S || !B)
    return StmtError();

  if (isa<ObjCForCollectionStmt>(S))
    return FinishObjCForCollectionStmt(S, B);

  CXXForRangeStmt *ForStmt = cast<CXXForRangeStmt>(S);
  ForStmt->setBody(B);

  DiagnoseEmptyStmtBody(ForStmt->getRParenLoc(), B,
                        diag::warn_empty_range_based_for_body);

  return S;
}

StmtResult Sema::ActOnGotoStmt(SourceLocation GotoLoc,
                               SourceLocation LabelLoc,
                               LabelDecl *TheDecl) {
  getCurFunction()->setHasBranchIntoScope();
  TheDecl->setUsed();
  return Owned(new (Context) GotoStmt(TheDecl, GotoLoc, LabelLoc));
}

StmtResult
Sema::ActOnIndirectGotoStmt(SourceLocation GotoLoc, SourceLocation StarLoc,
                            Expr *E) {
  // Convert operand to void*
  if (!E->isTypeDependent()) {
    QualType ETy = E->getType();
    QualType DestTy = Context.getPointerType(Context.VoidTy.withConst());
    ExprResult ExprRes = Owned(E);
    AssignConvertType ConvTy =
      CheckSingleAssignmentConstraints(DestTy, ExprRes);
    if (ExprRes.isInvalid())
      return StmtError();
    E = ExprRes.take();
    if (DiagnoseAssignmentResult(ConvTy, StarLoc, DestTy, ETy, E, AA_Passing))
      return StmtError();
  }

  ExprResult ExprRes = ActOnFinishFullExpr(E);
  if (ExprRes.isInvalid())
    return StmtError();
  E = ExprRes.take();

  getCurFunction()->setHasIndirectGoto();

  return Owned(new (Context) IndirectGotoStmt(GotoLoc, StarLoc, E));
}

StmtResult
Sema::ActOnContinueStmt(SourceLocation ContinueLoc, Scope *CurScope) {
  Scope *S = CurScope->getContinueParent();
  if (!S) {
    // C99 6.8.6.2p1: A break shall appear only in or as a loop body.
    return StmtError(Diag(ContinueLoc, diag::err_continue_not_in_loop));
  }

  return Owned(new (Context) ContinueStmt(ContinueLoc));
}

StmtResult
Sema::ActOnBreakStmt(SourceLocation BreakLoc, Scope *CurScope) {
  Scope *S = CurScope->getBreakParent();
  if (!S) {
    // Break from a Cilk for loop is not allowed unless the break is
    // inside a nested loop or switch statement.
    if (isa<CilkForScopeInfo>(getCurFunction())) {
      Diag(BreakLoc, diag::err_cilk_for_cannot_break);
      return StmtError();
    }

    // C99 6.8.6.3p1: A break shall appear only in or as a switch/loop body.
    return StmtError(Diag(BreakLoc, diag::err_break_not_in_loop_or_switch));
  }

  return Owned(new (Context) BreakStmt(BreakLoc));
}

StmtResult
Sema::ActOnCilkSyncStmt(SourceLocation SyncLoc) {
  return Owned(new (Context) CilkSyncStmt(SyncLoc));
}


/// \brief Determine whether the given expression is a candidate for
/// copy elision in either a return statement or a throw expression.
///
/// \param ReturnType If we're determining the copy elision candidate for
/// a return statement, this is the return type of the function. If we're
/// determining the copy elision candidate for a throw expression, this will
/// be a NULL type.
///
/// \param E The expression being returned from the function or block, or
/// being thrown.
///
/// \param AllowFunctionParameter Whether we allow function parameters to
/// be considered NRVO candidates. C++ prohibits this for NRVO itself, but
/// we re-use this logic to determine whether we should try to move as part of
/// a return or throw (which does allow function parameters).
///
/// \returns The NRVO candidate variable, if the return statement may use the
/// NRVO, or NULL if there is no such candidate.
const VarDecl *Sema::getCopyElisionCandidate(QualType ReturnType,
                                             Expr *E,
                                             bool AllowFunctionParameter) {
  QualType ExprType = E->getType();
  // - in a return statement in a function with ...
  // ... a class return type ...
  if (!ReturnType.isNull()) {
    if (!ReturnType->isRecordType())
      return 0;
    // ... the same cv-unqualified type as the function return type ...
    if (!Context.hasSameUnqualifiedType(ReturnType, ExprType))
      return 0;
  }

  // ... the expression is the name of a non-volatile automatic object
  // (other than a function or catch-clause parameter)) ...
  const DeclRefExpr *DR = dyn_cast<DeclRefExpr>(E->IgnoreParens());
  if (!DR || DR->refersToEnclosingLocal())
    return 0;
  const VarDecl *VD = dyn_cast<VarDecl>(DR->getDecl());
  if (!VD)
    return 0;

  // ...object (other than a function or catch-clause parameter)...
  if (VD->getKind() != Decl::Var &&
      !(AllowFunctionParameter && VD->getKind() == Decl::ParmVar))
    return 0;
  if (VD->isExceptionVariable()) return 0;

  // ...automatic...
  if (!VD->hasLocalStorage()) return 0;

  // ...non-volatile...
  if (VD->getType().isVolatileQualified()) return 0;
  if (VD->getType()->isReferenceType()) return 0;

  // __block variables can't be allocated in a way that permits NRVO.
  if (VD->hasAttr<BlocksAttr>()) return 0;

  // Variables with higher required alignment than their type's ABI
  // alignment cannot use NRVO.
  if (VD->hasAttr<AlignedAttr>() &&
      Context.getDeclAlign(VD) > Context.getTypeAlignInChars(VD->getType()))
    return 0;

  return VD;
}

/// \brief Perform the initialization of a potentially-movable value, which
/// is the result of return value.
///
/// This routine implements C++0x [class.copy]p33, which attempts to treat
/// returned lvalues as rvalues in certain cases (to prefer move construction),
/// then falls back to treating them as lvalues if that failed.
ExprResult
Sema::PerformMoveOrCopyInitialization(const InitializedEntity &Entity,
                                      const VarDecl *NRVOCandidate,
                                      QualType ResultType,
                                      Expr *Value,
                                      bool AllowNRVO) {
  // C++0x [class.copy]p33:
  //   When the criteria for elision of a copy operation are met or would
  //   be met save for the fact that the source object is a function
  //   parameter, and the object to be copied is designated by an lvalue,
  //   overload resolution to select the constructor for the copy is first
  //   performed as if the object were designated by an rvalue.
  ExprResult Res = ExprError();
  if (AllowNRVO &&
      (NRVOCandidate || getCopyElisionCandidate(ResultType, Value, true))) {
    ImplicitCastExpr AsRvalue(ImplicitCastExpr::OnStack,
                              Value->getType(), CK_NoOp, Value, VK_XValue);

    Expr *InitExpr = &AsRvalue;
    InitializationKind Kind
      = InitializationKind::CreateCopy(Value->getLocStart(),
                                       Value->getLocStart());
    InitializationSequence Seq(*this, Entity, Kind, &InitExpr, 1);

    //   [...] If overload resolution fails, or if the type of the first
    //   parameter of the selected constructor is not an rvalue reference
    //   to the object's type (possibly cv-qualified), overload resolution
    //   is performed again, considering the object as an lvalue.
    if (Seq) {
      for (InitializationSequence::step_iterator Step = Seq.step_begin(),
           StepEnd = Seq.step_end();
           Step != StepEnd; ++Step) {
        if (Step->Kind != InitializationSequence::SK_ConstructorInitialization)
          continue;

        CXXConstructorDecl *Constructor
        = cast<CXXConstructorDecl>(Step->Function.Function);

        const RValueReferenceType *RRefType
          = Constructor->getParamDecl(0)->getType()
                                                 ->getAs<RValueReferenceType>();

        // If we don't meet the criteria, break out now.
        if (!RRefType ||
            !Context.hasSameUnqualifiedType(RRefType->getPointeeType(),
                            Context.getTypeDeclType(Constructor->getParent())))
          break;

        // Promote "AsRvalue" to the heap, since we now need this
        // expression node to persist.
        Value = ImplicitCastExpr::Create(Context, Value->getType(),
                                         CK_NoOp, Value, 0, VK_XValue);

        // Complete type-checking the initialization of the return type
        // using the constructor we found.
        Res = Seq.Perform(*this, Entity, Kind, MultiExprArg(&Value, 1));
      }
    }
  }

  // Either we didn't meet the criteria for treating an lvalue as an rvalue,
  // above, or overload resolution failed. Either way, we need to try
  // (again) now with the return value expression as written.
  if (Res.isInvalid())
    Res = PerformCopyInitialization(Entity, SourceLocation(), Value);

  return Res;
}

/// ActOnCapScopeReturnStmt - Utility routine to type-check return statements
/// for capturing scopes.
///
StmtResult
Sema::ActOnCapScopeReturnStmt(SourceLocation ReturnLoc, Expr *RetValExp) {
  // If this is the first return we've seen, infer the return type.
  // [expr.prim.lambda]p4 in C++11; block literals follow a superset of those
  // rules which allows multiple return statements.
  CapturingScopeInfo *CurCap = cast<CapturingScopeInfo>(getCurFunction());
  QualType FnRetType = CurCap->ReturnType;

  // It is not allowed to return from a Cilk for statement.
  if (isa<CilkForScopeInfo>(CurCap)) {
    Diag(ReturnLoc, diag::err_cilk_for_cannot_return);
    return StmtError();
  }

  // For blocks/lambdas with implicit return types, we check each return
  // statement individually, and deduce the common return type when the block
  // or lambda is completed.
  if (CurCap->HasImplicitReturnType) {
    if (RetValExp && !isa<InitListExpr>(RetValExp)) {
      ExprResult Result = DefaultFunctionArrayLvalueConversion(RetValExp);
      if (Result.isInvalid())
        return StmtError();
      RetValExp = Result.take();

      if (!RetValExp->isTypeDependent())
        FnRetType = RetValExp->getType();
      else
        FnRetType = CurCap->ReturnType = Context.DependentTy;
    } else {
      if (RetValExp) {
        // C++11 [expr.lambda.prim]p4 bans inferring the result from an
        // initializer list, because it is not an expression (even
        // though we represent it as one). We still deduce 'void'.
        Diag(ReturnLoc, diag::err_lambda_return_init_list)
          << RetValExp->getSourceRange();
      }

      FnRetType = Context.VoidTy;
    }

    // Although we'll properly infer the type of the block once it's completed,
    // make sure we provide a return type now for better error recovery.
    if (CurCap->ReturnType.isNull())
      CurCap->ReturnType = FnRetType;
  }
  assert(!FnRetType.isNull());

  if (BlockScopeInfo *CurBlock = dyn_cast<BlockScopeInfo>(CurCap)) {
    if (CurBlock->FunctionType->getAs<FunctionType>()->getNoReturnAttr()) {
      Diag(ReturnLoc, diag::err_noreturn_block_has_return_expr);
      return StmtError();
    }
  } else {
    LambdaScopeInfo *LSI = cast<LambdaScopeInfo>(CurCap);
    if (LSI->CallOperator->getType()->getAs<FunctionType>()->getNoReturnAttr()){
      Diag(ReturnLoc, diag::err_noreturn_lambda_has_return_expr);
      return StmtError();
    }
  }

  // Otherwise, verify that this result type matches the previous one.  We are
  // pickier with blocks than for normal functions because we don't have GCC
  // compatibility to worry about here.
  const VarDecl *NRVOCandidate = 0;
  if (FnRetType->isDependentType()) {
    // Delay processing for now.  TODO: there are lots of dependent
    // types we can conclusively prove aren't void.
  } else if (FnRetType->isVoidType()) {
    if (RetValExp && !isa<InitListExpr>(RetValExp) &&
        !(getLangOpts().CPlusPlus &&
          (RetValExp->isTypeDependent() ||
           RetValExp->getType()->isVoidType()))) {
      if (!getLangOpts().CPlusPlus &&
          RetValExp->getType()->isVoidType())
        Diag(ReturnLoc, diag::ext_return_has_void_expr) << "literal" << 2;
      else {
        Diag(ReturnLoc, diag::err_return_block_has_expr);
        RetValExp = 0;
      }
    }
  } else if (!RetValExp) {
    return StmtError(Diag(ReturnLoc, diag::err_block_return_missing_expr));
  } else if (!RetValExp->isTypeDependent()) {
    // we have a non-void block with an expression, continue checking

    // C99 6.8.6.4p3(136): The return statement is not an assignment. The
    // overlap restriction of subclause 6.5.16.1 does not apply to the case of
    // function return.

    // In C++ the return statement is handled via a copy initialization.
    // the C version of which boils down to CheckSingleAssignmentConstraints.
    NRVOCandidate = getCopyElisionCandidate(FnRetType, RetValExp, false);
    InitializedEntity Entity = InitializedEntity::InitializeResult(ReturnLoc,
                                                                   FnRetType,
                                                          NRVOCandidate != 0);
    ExprResult Res = PerformMoveOrCopyInitialization(Entity, NRVOCandidate,
                                                     FnRetType, RetValExp);
    if (Res.isInvalid()) {
      // FIXME: Cleanup temporaries here, anyway?
      return StmtError();
    }
    RetValExp = Res.take();
    CheckReturnStackAddr(RetValExp, FnRetType, ReturnLoc);
  }

  if (RetValExp) {
    ExprResult ER = ActOnFinishFullExpr(RetValExp, ReturnLoc);
    if (ER.isInvalid())
      return StmtError();
    RetValExp = ER.take();
  }
  ReturnStmt *Result = new (Context) ReturnStmt(ReturnLoc, RetValExp,
                                                NRVOCandidate);

  // If we need to check for the named return value optimization,
  // or if we need to infer the return type,
  // save the return statement in our scope for later processing.
  if (CurCap->HasImplicitReturnType ||
      (getLangOpts().CPlusPlus && FnRetType->isRecordType() &&
       !CurContext->isDependentContext()))
    FunctionScopes.back()->Returns.push_back(Result);

  return Owned(Result);
}

StmtResult
Sema::ActOnReturnStmt(SourceLocation ReturnLoc, Expr *RetValExp) {
  // Check for unexpanded parameter packs.
  if (RetValExp && DiagnoseUnexpandedParameterPack(RetValExp))
    return StmtError();

  if (isa<CapturingScopeInfo>(getCurFunction()))
    return ActOnCapScopeReturnStmt(ReturnLoc, RetValExp);

  QualType FnRetType;
  QualType RelatedRetType;
  if (const FunctionDecl *FD = getCurFunctionDecl()) {
    FnRetType = FD->getResultType();
    if (FD->isNoReturn())
      Diag(ReturnLoc, diag::warn_noreturn_function_has_return_expr)
        << FD->getDeclName();
  } else if (ObjCMethodDecl *MD = getCurMethodDecl()) {
    FnRetType = MD->getResultType();
    if (MD->hasRelatedResultType() && MD->getClassInterface()) {
      // In the implementation of a method with a related return type, the
      // type used to type-check the validity of return statements within the
      // method body is a pointer to the type of the class being implemented.
      RelatedRetType = Context.getObjCInterfaceType(MD->getClassInterface());
      RelatedRetType = Context.getObjCObjectPointerType(RelatedRetType);
    }
  } else // If we don't have a function/method context, bail.
    return StmtError();

  ReturnStmt *Result = 0;
  if (FnRetType->isVoidType()) {
    if (RetValExp) {
      if (isa<InitListExpr>(RetValExp)) {
        // We simply never allow init lists as the return value of void
        // functions. This is compatible because this was never allowed before,
        // so there's no legacy code to deal with.
        NamedDecl *CurDecl = getCurFunctionOrMethodDecl();
        int FunctionKind = 0;
        if (isa<ObjCMethodDecl>(CurDecl))
          FunctionKind = 1;
        else if (isa<CXXConstructorDecl>(CurDecl))
          FunctionKind = 2;
        else if (isa<CXXDestructorDecl>(CurDecl))
          FunctionKind = 3;

        Diag(ReturnLoc, diag::err_return_init_list)
          << CurDecl->getDeclName() << FunctionKind
          << RetValExp->getSourceRange();

        // Drop the expression.
        RetValExp = 0;
      } else if (!RetValExp->isTypeDependent()) {
        // C99 6.8.6.4p1 (ext_ since GCC warns)
        unsigned D = diag::ext_return_has_expr;
        if (RetValExp->getType()->isVoidType())
          D = diag::ext_return_has_void_expr;
        else {
          ExprResult Result = Owned(RetValExp);
          Result = IgnoredValueConversions(Result.take());
          if (Result.isInvalid())
            return StmtError();
          RetValExp = Result.take();
          RetValExp = ImpCastExprToType(RetValExp,
                                        Context.VoidTy, CK_ToVoid).take();
        }

        // return (some void expression); is legal in C++.
        if (D != diag::ext_return_has_void_expr ||
            !getLangOpts().CPlusPlus) {
          NamedDecl *CurDecl = getCurFunctionOrMethodDecl();

          int FunctionKind = 0;
          if (isa<ObjCMethodDecl>(CurDecl))
            FunctionKind = 1;
          else if (isa<CXXConstructorDecl>(CurDecl))
            FunctionKind = 2;
          else if (isa<CXXDestructorDecl>(CurDecl))
            FunctionKind = 3;

          Diag(ReturnLoc, D)
            << CurDecl->getDeclName() << FunctionKind
            << RetValExp->getSourceRange();
        }
      }

      if (RetValExp) {
        ExprResult ER = ActOnFinishFullExpr(RetValExp, ReturnLoc);
        if (ER.isInvalid())
          return StmtError();
        RetValExp = ER.take();
      }
    }

    Result = new (Context) ReturnStmt(ReturnLoc, RetValExp, 0);
  } else if (!RetValExp && !FnRetType->isDependentType()) {
    unsigned DiagID = diag::warn_return_missing_expr;  // C90 6.6.6.4p4
    // C99 6.8.6.4p1 (ext_ since GCC warns)
    if (getLangOpts().C99) DiagID = diag::ext_return_missing_expr;

    if (FunctionDecl *FD = getCurFunctionDecl())
      Diag(ReturnLoc, DiagID) << FD->getIdentifier() << 0/*fn*/;
    else
      Diag(ReturnLoc, DiagID) << getCurMethodDecl()->getDeclName() << 1/*meth*/;
    Result = new (Context) ReturnStmt(ReturnLoc);
  } else {
    assert(RetValExp || FnRetType->isDependentType());
    const VarDecl *NRVOCandidate = 0;
    if (!FnRetType->isDependentType() && !RetValExp->isTypeDependent()) {
      // we have a non-void function with an expression, continue checking

      QualType RetType = (RelatedRetType.isNull() ? FnRetType : RelatedRetType);

      // C99 6.8.6.4p3(136): The return statement is not an assignment. The
      // overlap restriction of subclause 6.5.16.1 does not apply to the case of
      // function return.

      // In C++ the return statement is handled via a copy initialization,
      // the C version of which boils down to CheckSingleAssignmentConstraints.
      NRVOCandidate = getCopyElisionCandidate(FnRetType, RetValExp, false);
      InitializedEntity Entity = InitializedEntity::InitializeResult(ReturnLoc,
                                                                     RetType,
                                                            NRVOCandidate != 0);
      ExprResult Res = PerformMoveOrCopyInitialization(Entity, NRVOCandidate,
                                                       RetType, RetValExp);
      if (Res.isInvalid()) {
        // FIXME: Clean up temporaries here anyway?
        return StmtError();
      }
      RetValExp = Res.takeAs<Expr>();

      // If we have a related result type, we need to implicitly
      // convert back to the formal result type.  We can't pretend to
      // initialize the result again --- we might end double-retaining
      // --- so instead we initialize a notional temporary; this can
      // lead to less-than-great diagnostics, but this stage is much
      // less likely to fail than the previous stage.
      if (!RelatedRetType.isNull()) {
        Entity = InitializedEntity::InitializeTemporary(FnRetType);
        Res = PerformCopyInitialization(Entity, ReturnLoc, RetValExp);
        if (Res.isInvalid()) {
          // FIXME: Clean up temporaries here anyway?
          return StmtError();
        }
        RetValExp = Res.takeAs<Expr>();
      }

      CheckReturnStackAddr(RetValExp, FnRetType, ReturnLoc);
    }

    if (RetValExp) {
      ExprResult ER = ActOnFinishFullExpr(RetValExp, ReturnLoc);
      if (ER.isInvalid())
        return StmtError();
      RetValExp = ER.take();
    }
    Result = new (Context) ReturnStmt(ReturnLoc, RetValExp, NRVOCandidate);
  }

  // If we need to check for the named return value optimization, save the
  // return statement in our scope for later processing.
  if (getLangOpts().CPlusPlus && FnRetType->isRecordType() &&
      !CurContext->isDependentContext())
    FunctionScopes.back()->Returns.push_back(Result);

  return Owned(Result);
}

StmtResult
Sema::ActOnObjCAtCatchStmt(SourceLocation AtLoc,
                           SourceLocation RParen, Decl *Parm,
                           Stmt *Body) {
  VarDecl *Var = cast_or_null<VarDecl>(Parm);
  if (Var && Var->isInvalidDecl())
    return StmtError();

  return Owned(new (Context) ObjCAtCatchStmt(AtLoc, RParen, Var, Body));
}

StmtResult
Sema::ActOnObjCAtFinallyStmt(SourceLocation AtLoc, Stmt *Body) {
  return Owned(new (Context) ObjCAtFinallyStmt(AtLoc, Body));
}

StmtResult
Sema::ActOnObjCAtTryStmt(SourceLocation AtLoc, Stmt *Try,
                         MultiStmtArg CatchStmts, Stmt *Finally) {
  if (!getLangOpts().ObjCExceptions)
    Diag(AtLoc, diag::err_objc_exceptions_disabled) << "@try";

  getCurFunction()->setHasBranchProtectedScope();
  unsigned NumCatchStmts = CatchStmts.size();
  return Owned(ObjCAtTryStmt::Create(Context, AtLoc, Try,
                                     CatchStmts.data(),
                                     NumCatchStmts,
                                     Finally));
}

StmtResult Sema::BuildObjCAtThrowStmt(SourceLocation AtLoc, Expr *Throw) {
  if (Throw) {
    ExprResult Result = DefaultLvalueConversion(Throw);
    if (Result.isInvalid())
      return StmtError();

    Result = ActOnFinishFullExpr(Result.take());
    if (Result.isInvalid())
      return StmtError();
    Throw = Result.take();

    QualType ThrowType = Throw->getType();
    // Make sure the expression type is an ObjC pointer or "void *".
    if (!ThrowType->isDependentType() &&
        !ThrowType->isObjCObjectPointerType()) {
      const PointerType *PT = ThrowType->getAs<PointerType>();
      if (!PT || !PT->getPointeeType()->isVoidType())
        return StmtError(Diag(AtLoc, diag::error_objc_throw_expects_object)
                         << Throw->getType() << Throw->getSourceRange());
    }
  }

  return Owned(new (Context) ObjCAtThrowStmt(AtLoc, Throw));
}

StmtResult
Sema::ActOnObjCAtThrowStmt(SourceLocation AtLoc, Expr *Throw,
                           Scope *CurScope) {
  if (!getLangOpts().ObjCExceptions)
    Diag(AtLoc, diag::err_objc_exceptions_disabled) << "@throw";

  if (!Throw) {
    // @throw without an expression designates a rethrow (which much occur
    // in the context of an @catch clause).
    Scope *AtCatchParent = CurScope;
    while (AtCatchParent && !AtCatchParent->isAtCatchScope())
      AtCatchParent = AtCatchParent->getParent();
    if (!AtCatchParent)
      return StmtError(Diag(AtLoc, diag::error_rethrow_used_outside_catch));
  }
  return BuildObjCAtThrowStmt(AtLoc, Throw);
}

ExprResult
Sema::ActOnObjCAtSynchronizedOperand(SourceLocation atLoc, Expr *operand) {
  ExprResult result = DefaultLvalueConversion(operand);
  if (result.isInvalid())
    return ExprError();
  operand = result.take();

  // Make sure the expression type is an ObjC pointer or "void *".
  QualType type = operand->getType();
  if (!type->isDependentType() &&
      !type->isObjCObjectPointerType()) {
    const PointerType *pointerType = type->getAs<PointerType>();
    if (!pointerType || !pointerType->getPointeeType()->isVoidType())
      return Diag(atLoc, diag::error_objc_synchronized_expects_object)
               << type << operand->getSourceRange();
  }

  // The operand to @synchronized is a full-expression.
  return ActOnFinishFullExpr(operand);
}

StmtResult
Sema::ActOnObjCAtSynchronizedStmt(SourceLocation AtLoc, Expr *SyncExpr,
                                  Stmt *SyncBody) {
  // We can't jump into or indirect-jump out of a @synchronized block.
  getCurFunction()->setHasBranchProtectedScope();
  return Owned(new (Context) ObjCAtSynchronizedStmt(AtLoc, SyncExpr, SyncBody));
}

/// ActOnCXXCatchBlock - Takes an exception declaration and a handler block
/// and creates a proper catch handler from them.
StmtResult
Sema::ActOnCXXCatchBlock(SourceLocation CatchLoc, Decl *ExDecl,
                         Stmt *HandlerBlock) {
  // There's nothing to test that ActOnExceptionDecl didn't already test.
  return Owned(new (Context) CXXCatchStmt(CatchLoc,
                                          cast_or_null<VarDecl>(ExDecl),
                                          HandlerBlock));
}

StmtResult
Sema::ActOnObjCAutoreleasePoolStmt(SourceLocation AtLoc, Stmt *Body) {
  getCurFunction()->setHasBranchProtectedScope();
  return Owned(new (Context) ObjCAutoreleasePoolStmt(AtLoc, Body));
}

namespace {

class TypeWithHandler {
  QualType t;
  CXXCatchStmt *stmt;
public:
  TypeWithHandler(const QualType &type, CXXCatchStmt *statement)
  : t(type), stmt(statement) {}

  // An arbitrary order is fine as long as it places identical
  // types next to each other.
  bool operator<(const TypeWithHandler &y) const {
    if (t.getAsOpaquePtr() < y.t.getAsOpaquePtr())
      return true;
    if (t.getAsOpaquePtr() > y.t.getAsOpaquePtr())
      return false;
    else
      return getTypeSpecStartLoc() < y.getTypeSpecStartLoc();
  }

  bool operator==(const TypeWithHandler& other) const {
    return t == other.t;
  }

  CXXCatchStmt *getCatchStmt() const { return stmt; }
  SourceLocation getTypeSpecStartLoc() const {
    return stmt->getExceptionDecl()->getTypeSpecStartLoc();
  }
};

}

/// ActOnCXXTryBlock - Takes a try compound-statement and a number of
/// handlers and creates a try statement from them.
StmtResult
Sema::ActOnCXXTryBlock(SourceLocation TryLoc, Stmt *TryBlock,
                       MultiStmtArg RawHandlers) {
  // Don't report an error if 'try' is used in system headers.
  if (!getLangOpts().CXXExceptions &&
      !getSourceManager().isInSystemHeader(TryLoc))
      Diag(TryLoc, diag::err_exceptions_disabled) << "try";

  unsigned NumHandlers = RawHandlers.size();
  assert(NumHandlers > 0 &&
         "The parser shouldn't call this if there are no handlers.");
  Stmt **Handlers = RawHandlers.data();

  SmallVector<TypeWithHandler, 8> TypesWithHandlers;

  for (unsigned i = 0; i < NumHandlers; ++i) {
    CXXCatchStmt *Handler = cast<CXXCatchStmt>(Handlers[i]);
    if (!Handler->getExceptionDecl()) {
      if (i < NumHandlers - 1)
        return StmtError(Diag(Handler->getLocStart(),
                              diag::err_early_catch_all));

      continue;
    }

    const QualType CaughtType = Handler->getCaughtType();
    const QualType CanonicalCaughtType = Context.getCanonicalType(CaughtType);
    TypesWithHandlers.push_back(TypeWithHandler(CanonicalCaughtType, Handler));
  }

  // Detect handlers for the same type as an earlier one.
  if (NumHandlers > 1) {
    llvm::array_pod_sort(TypesWithHandlers.begin(), TypesWithHandlers.end());

    TypeWithHandler prev = TypesWithHandlers[0];
    for (unsigned i = 1; i < TypesWithHandlers.size(); ++i) {
      TypeWithHandler curr = TypesWithHandlers[i];

      if (curr == prev) {
        Diag(curr.getTypeSpecStartLoc(),
             diag::warn_exception_caught_by_earlier_handler)
          << curr.getCatchStmt()->getCaughtType().getAsString();
        Diag(prev.getTypeSpecStartLoc(),
             diag::note_previous_exception_handler)
          << prev.getCatchStmt()->getCaughtType().getAsString();
      }

      prev = curr;
    }
  }

  getCurFunction()->setHasBranchProtectedScope();

  // FIXME: We should detect handlers that cannot catch anything because an
  // earlier handler catches a superclass. Need to find a method that is not
  // quadratic for this.
  // Neither of these are explicitly forbidden, but every compiler detects them
  // and warns.

  return Owned(CXXTryStmt::Create(Context, TryLoc, TryBlock,
                                  llvm::makeArrayRef(Handlers, NumHandlers)));
}

StmtResult
Sema::ActOnSEHTryBlock(bool IsCXXTry,
                       SourceLocation TryLoc,
                       Stmt *TryBlock,
                       Stmt *Handler) {
  assert(TryBlock && Handler);

  getCurFunction()->setHasBranchProtectedScope();

  return Owned(SEHTryStmt::Create(Context,IsCXXTry,TryLoc,TryBlock,Handler));
}

StmtResult
Sema::ActOnSEHExceptBlock(SourceLocation Loc,
                          Expr *FilterExpr,
                          Stmt *Block) {
  assert(FilterExpr && Block);

  if(!FilterExpr->getType()->isIntegerType()) {
    return StmtError(Diag(FilterExpr->getExprLoc(),
                     diag::err_filter_expression_integral)
                     << FilterExpr->getType());
  }

  return Owned(SEHExceptStmt::Create(Context,Loc,FilterExpr,Block));
}

StmtResult
Sema::ActOnSEHFinallyBlock(SourceLocation Loc,
                           Stmt *Block) {
  assert(Block);
  return Owned(SEHFinallyStmt::Create(Context,Loc,Block));
}

StmtResult Sema::BuildMSDependentExistsStmt(SourceLocation KeywordLoc,
                                            bool IsIfExists,
                                            NestedNameSpecifierLoc QualifierLoc,
                                            DeclarationNameInfo NameInfo,
                                            Stmt *Nested)
{
  return new (Context) MSDependentExistsStmt(KeywordLoc, IsIfExists,
                                             QualifierLoc, NameInfo,
                                             cast<CompoundStmt>(Nested));
}


StmtResult Sema::ActOnMSDependentExistsStmt(SourceLocation KeywordLoc,
                                            bool IsIfExists,
                                            CXXScopeSpec &SS,
                                            UnqualifiedId &Name,
                                            Stmt *Nested) {
  return BuildMSDependentExistsStmt(KeywordLoc, IsIfExists,
                                    SS.getWithLocInContext(Context),
                                    GetNameFromUnqualifiedId(Name),
                                    Nested);
}

namespace {

class SpawnHelper : public RecursiveASTVisitor<SpawnHelper> {
  bool HasSpawn;
public:
  SpawnHelper() : HasSpawn(false) {}
  bool TraverseCompoundStmt(CompoundStmt *) { return true; }
  bool VisitCallExpr(CallExpr *E) {
    if (E->isCilkSpawnCall()) {
      HasSpawn = true;
      return false; // terminate if found
    }
    return true;
  }

  bool hasSpawn() const { return HasSpawn; }
};

class CaptureBuilder: public RecursiveASTVisitor<CaptureBuilder> {
  Sema &S;

public:
  CaptureBuilder(Sema &S) : S(S) {}

  bool VisitDeclRefExpr(DeclRefExpr *E) {
    S.MarkDeclRefReferenced(E);
    return true;
  }

  bool TraverseLambdaExpr(LambdaExpr *E) {
    LambdaExpr::capture_init_iterator CI = E->capture_init_begin();

    for (LambdaExpr::capture_iterator C = E->capture_begin(),
                                   CEnd = E->capture_end();
                                     C != CEnd; ++C, ++CI) {
      if (C->capturesVariable())
        S.MarkVariableReferenced((*CI)->getLocStart(), C->getCapturedVar());
      else {
        assert(C->capturesThis() && "Capturing this expected");
        assert(isa<CXXThisExpr>(*CI) && "CXXThisExpr expected");
        S.CheckCXXThisCapture((*CI)->getLocStart(), /*explicit*/false);
      }
    }
    assert(CI == E->capture_init_end() && "out of sync");
    
    // Only traverse the captures, and skip the body.
    return true;
  }

  /// Skip captured statements
  bool TraverseCapturedStmt(CapturedStmt *) { return true; }

  bool VisitCXXThisExpr(CXXThisExpr *E) {
    S.CheckCXXThisCapture(E->getLocStart(), /*explicit*/false);
    return true;
  }
};

} // anonymous namespace

RecordDecl*
Sema::CreateCapturedStmtRecordDecl(FunctionDecl *&FD, SourceLocation Loc,
                                   IdentifierInfo *MangledName) {
  DeclContext *DC = CurContext;
  while (!(DC->isFunctionOrMethod() || DC->isRecord() || DC->isFileContext()))
    DC = DC->getParent();

  IdentifierInfo *Id = &PP.getIdentifierTable().get("capture");
  RecordDecl *RD = RecordDecl::Create(Context, TTK_Struct, DC, Loc, Loc, Id);

  DC->addDecl(RD);
  RD->setImplicit();
  RD->startDefinition();

  QualType CapParamType = Context.getPointerType(Context.getTagDeclType(RD));

  QualType FunctionTy;
  FunctionProtoType::ExtProtoInfo EPI;
  FunctionTy = Context.getFunctionType(Context.VoidTy, CapParamType, EPI);

  TypeSourceInfo *FuncTyInfo = Context.getTrivialTypeSourceInfo(FunctionTy);
  FD = FunctionDecl::Create(Context, CurContext, SourceLocation(),
                            SourceLocation(), MangledName, FunctionTy,
                            FuncTyInfo, SC_None, SC_None);
  ParmVarDecl *CapParam = 0;
  {
    IdentifierInfo *IdThis = &PP.getIdentifierTable().get("this");
    TypeSourceInfo *TyInfo = Context.getTrivialTypeSourceInfo(CapParamType);
    CapParam = ParmVarDecl::Create(Context, FD, SourceLocation(),
                                   SourceLocation(), IdThis, CapParamType,
                                   TyInfo, SC_None, /* DefaultArg =*/0);
  }

  FD->setParams(CapParam);

  FD->setImplicit(true);
  FD->setUsed(true);
  FD->setParallelRegion();
  DC->addDecl(FD);

  return RD;
}

SmallVector<CapturedStmt::Capture, 4>
Sema::buildCapturedStmtCaptureList(SmallVector<CapturingScopeInfo::Capture, 4>
                                     &Candidates) {

  SmallVector<CapturedStmt::Capture, 4> Captures;
  for (unsigned I = 0, N = Candidates.size(); I != N; I++) {
    CapturingScopeInfo::Capture &Cap = Candidates[I];

    if (Cap.isThisCapture()) {
      Captures.push_back(CapturedStmt::Capture(CapturedStmt::LCK_This,
                                               Cap.getCopyExpr()));
      continue;
    }

    VarDecl *Var = Cap.getVariable();
    assert(!Cap.isCopyCapture() &&
           "CapturedStmt by-copy capture not implemented yet");
    Captures.push_back(CapturedStmt::Capture(CapturedStmt::LCK_ByRef,
                                             Cap.getCopyExpr(), Var));
  }

  return Captures;
}

/// Helper functions are required to be internal,  not mangling accross
/// translation units.
static IdentifierInfo *GetMangledHelperName(Sema &S) {
  static unsigned count = 0;
  StringRef name("__cilk_spawn_helperV");
  return &S.PP.getIdentifierTable().get((name + llvm::Twine(count++)).str());
}

static QualType GetReceiverTmpType(const Expr *E) {
  do {
    if (const ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(E))
      E = EWC->getSubExpr();
    const MaterializeTemporaryExpr *M = NULL;
    E = E->findMaterializedTemporary(M);
  } while (isa<ExprWithCleanups>(E));

  // Skip any implicit casts.
  SmallVector<SubobjectAdjustment, 2> Adjustments;
  E = E->skipRValueSubobjectAdjustments(Adjustments);

  return E->getType();
}

static void GetReceiverType(ASTContext &Context, CilkSpawnCapturedStmt *S,
                            QualType &ReceiverType, QualType &ReceiverTmpType) {
  Stmt *SubStmt = S->getSubStmt();
  if (DeclStmt *DS = dyn_cast_or_null<DeclStmt>(SubStmt)) {
    if (VarDecl *VD = cast<VarDecl>(DS->getSingleDecl())) {
      ReceiverType = Context.getCanonicalType(VD->getType());
      if (VD->getType()->isReferenceType() &&
          VD->extendsLifetimeOfTemporary())
        ReceiverTmpType = GetReceiverTmpType(VD->getInit());
    }
  }
}

static FieldDecl *CreateReceiverField(ASTContext& Context, RecordDecl *RD,
                                      QualType ReceiverType) {
  FieldDecl *Field = FieldDecl::Create(
      Context, RD, SourceLocation(), SourceLocation(), 0, ReceiverType,
      Context.getTrivialTypeSourceInfo(ReceiverType, SourceLocation()),
      0, false, ICIS_NoInit);

  Field->setImplicit(true);
  return Field;
}

static void buildCilkSpawnCaptures(Sema &S, Scope *CurScope,
                                   CilkSpawnCapturedStmt *Spawn) {
  // Create a caputred recored decl and start its definition
  FunctionDecl *FD = 0;
  RecordDecl *RD = S.CreateCapturedStmtRecordDecl(FD, SourceLocation(),
                                                  GetMangledHelperName(S));

  // Enter the capturing scope for this parallel region
  S.PushParallelRegionScope(CurScope, FD, RD);

  if (CurScope)
    S.PushDeclContext(CurScope, FD);
  else
    S.CurContext = FD;

  // Scan the statement to find variables to be captured.
  ParallelRegionScopeInfo *RSI
    = cast<ParallelRegionScopeInfo>(S.FunctionScopes.back());

  CaptureBuilder Builder(S);
  Builder.TraverseStmt(Spawn->getSubStmt());


  // Build the CilkSpawnCapturedStmt
  SmallVector<CapturedStmt::Capture, 4> Captures
      = S.buildCapturedStmtCaptureList(RSI->Captures);



  // Add implicit captures for receiver and/or receiver temporary.
  if (DeclStmt *DS = dyn_cast_or_null<DeclStmt>(Spawn->getSubStmt())) {
    VarDecl *VD = cast<VarDecl>(DS->getSingleDecl());
    QualType ReceiverType;
    QualType ReceiverTmpType;
    GetReceiverType(S.Context, Spawn, ReceiverType, ReceiverTmpType);
    ReceiverType = S.Context.getPointerType(ReceiverType);

    Captures.push_back(CapturedStmt::Capture(CapturedStmt::LCK_Receiver, 0, VD));
    RD->addDecl(CreateReceiverField(S.Context, RD, ReceiverType));

    if (!ReceiverTmpType.isNull()) {
      ReceiverTmpType = S.Context.getPointerType(ReceiverTmpType);
      Captures.push_back(CapturedStmt::Capture(CapturedStmt::LCK_ReceiverTmp,
                                               0, VD));
      RD->addDecl(CreateReceiverField(S.Context, RD, ReceiverTmpType));
    }
  }

  Spawn->setCaptures(S.Context, Captures.begin(), Captures.end());
  Spawn->setRecordDecl(RD);
  Spawn->setFunctionDecl(FD);

  FD->setBody(Spawn->getSubStmt());
  RD->completeDefinition();

  S.PopDeclContext();
  S.PopFunctionScopeInfo();
}

static Stmt *tryCreateCilkSpawnCapturedStmt(Sema &SemaRef, Stmt *S) {
  if (!S)
    return S;

  SpawnHelper Helper;
  Helper.TraverseStmt(S);
  if (!Helper.hasSpawn())
    return S;

  CilkSpawnCapturedStmt *R = new (SemaRef.Context) CilkSpawnCapturedStmt(S);
  buildCilkSpawnCaptures(SemaRef, SemaRef.getCurScope(), R);

  return R;
}

static void BuildCilkSpawnStmt(Sema &SemaRef, Stmt *&S) {
  switch (S->getStmtClass()) {
  default:
    break; // No need to tranform
  case Stmt::CXXForRangeStmtClass: {
    CXXForRangeStmt *FR = cast<CXXForRangeStmt>(S);
    if (Stmt *Body = FR->getBody()) {
      BuildCilkSpawnStmt(SemaRef, Body);
      FR->setBody(Body);
    }
    break;
  }
  case Stmt::DeclStmtClass:
  case Stmt::BinaryOperatorClass:
  case Stmt::ExprWithCleanupsClass:
  case Stmt::CallExprClass:
  case Stmt::CXXOperatorCallExprClass:
  case Stmt::CXXMemberCallExprClass:
    S = tryCreateCilkSpawnCapturedStmt(SemaRef, S);
    break;
  case Stmt::DoStmtClass: {
    DoStmt *DS = cast<DoStmt>(S);
    if (Stmt *Body = DS->getBody()) {
      BuildCilkSpawnStmt(SemaRef, Body);
      DS->setBody(Body);
    }
    break;
  }
  case Stmt::ForStmtClass: {
    ForStmt *F = cast<ForStmt>(S);
    if (Stmt *Body = F->getBody()) {
      BuildCilkSpawnStmt(SemaRef, Body);
      F->setBody(Body);
    }
    break;
  }
  case Stmt::IfStmtClass: {
    if (Stmt *Then = cast<IfStmt>(S)->getThen()) {
      BuildCilkSpawnStmt(SemaRef, Then);
      cast<IfStmt>(S)->setThen(Then);
    }
    if (Stmt *Else = cast<IfStmt>(S)->getElse()) {
      BuildCilkSpawnStmt(SemaRef, Else);
      cast<IfStmt>(S)->setElse(Else);
    }
    break;
  }
  case Stmt::LabelStmtClass: {
    LabelStmt *LS = cast<LabelStmt>(S);
    if (Stmt *SS = LS->getSubStmt()) {
      BuildCilkSpawnStmt(SemaRef, SS);
      LS->setSubStmt(SS);
    }
    break;
  }
  case Stmt::CaseStmtClass: {
    CaseStmt *CS = cast<CaseStmt>(S);
    if (Stmt *SS = CS->getSubStmt()) {
      BuildCilkSpawnStmt(SemaRef, SS);
      CS->setSubStmt(SS);
    }
    break;
  }
  case Stmt::DefaultStmtClass: {
    DefaultStmt *DS = cast<DefaultStmt>(S);
    if (Stmt *SS = DS->getSubStmt()) {
      BuildCilkSpawnStmt(SemaRef, SS);
      DS->setSubStmt(SS);
    }
    break;
  }
  case Stmt::WhileStmtClass: {
    WhileStmt *W = cast<WhileStmt>(S);
    if (Stmt *Body = W->getBody()) {
      BuildCilkSpawnStmt(SemaRef, Body);
      W->setBody(Body);
    }
    break;
  }
  }
}

StmtResult Sema::ActOnCilkSpawnStmt(Stmt *S) {
  if (!S)
    return StmtError();

  BuildCilkSpawnStmt(*this, S);
  return Owned(S);
}

static bool CheckCilkForInitStmt(Sema &S, Stmt *InitStmt,
                                 VarDecl *&ControlVar) {
  // Location of loop control variable/expression in the initializer
  SourceLocation InitLoc;
  bool IsDeclStmt = false;

  if (DeclStmt *DS = dyn_cast<DeclStmt>(InitStmt)) {
    // The initialization shall declare or initialize a single variable,
    // called the control variable.
    if (!DS->isSingleDecl()) {
      DeclStmt::decl_iterator DI = DS->decl_begin();
      ++DI;
      S.Diag((*DI)->getLocation(), diag::err_cilk_for_decl_multiple_variables);
      return false;
    }

    ControlVar = dyn_cast<VarDecl>(*DS->decl_begin());
    // Only allow VarDecls in the initializer
    if (!ControlVar) {
      S.Diag(InitStmt->getLocStart(),
             diag::err_cilk_for_initializer_expected_decl)
        << InitStmt->getSourceRange();
      return false;
    }

    // Ignore invalid decls.
    if (ControlVar->isInvalidDecl())
      return false;

    // The control variable shall be declared and initialized within the
    // initialization clause of the _Cilk_for loop.
    if (!ControlVar->getInit()) {
      S.Diag(ControlVar->getLocation(),
          diag::err_cilk_for_control_variable_not_initialized);
      return false;
    }

    InitLoc = ControlVar->getLocation();
    IsDeclStmt = true;
  } else {
    // In C++, the control variable shall be declared and initialized within
    // the initialization clause of the _Cilk_for loop.
    if (S.getLangOpts().CPlusPlus) {
      S.Diag(InitStmt->getLocStart(),
             diag::err_cilk_for_initialization_must_be_decl);
      return false;
    }

    // In C only, the control variable may be previously declared, but if so
    // shall be reinitialized, i.e., assigned, in the initialization clause.
    BinaryOperator *Op = 0;
    if (Expr *E = dyn_cast<Expr>(InitStmt)) {
      E = E->IgnoreParenNoopCasts(S.Context);
      Op = dyn_cast_or_null<BinaryOperator>(E);
    }

    if (!Op) {
      S.Diag(InitStmt->getLocStart(),
          diag::err_cilk_for_control_variable_not_initialized);
      return false;
    }

    // The initialization shall declare or initialize a single variable,
    // called the control variable.
    if (Op->getOpcode() == BO_Comma) {
      S.Diag(Op->getRHS()->getExprLoc(),
             diag::err_cilk_for_init_multiple_variables);
      return false;
    }

    if (!Op->isAssignmentOp()) {
      S.Diag(Op->getLHS()->getExprLoc(),
             diag::err_cilk_for_control_variable_not_initialized);
      return false;
    }

    // Get the decl for the LHS of the control variable initialization
    assert(Op->getLHS() && "BinaryOperator has no LHS!");
    DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
      Op->getLHS()->IgnoreParenNoopCasts(S.Context));
    if (!LHS) {
      S.Diag(Op->getLHS()->getExprLoc(),
             diag::err_cilk_for_initializer_expected_variable);
      return false;
    }

    // But, use the source location of the LHS for diagnostics
    InitLoc = LHS->getLocation();

    // Only a VarDecl may be used in the initializer
    ControlVar = dyn_cast<VarDecl>(LHS->getDecl());
    if (!ControlVar) {
      S.Diag(Op->getLHS()->getExprLoc(),
             diag::err_cilk_for_initializer_expected_variable);
      return false;
    }
  }

  // No storage class may be specified for the variable within the
  // initialization clause.
  StorageClass SC = ControlVar->getStorageClass();
  if (SC != SC_None) {
    S.Diag(InitLoc, diag::err_cilk_for_control_variable_storage_class)
      << ControlVar->getStorageClassSpecifierString(SC);
    if (!IsDeclStmt)
      S.Diag(ControlVar->getLocation(), diag::note_local_variable_declared_here)
        << ControlVar->getIdentifier();
    return false;
  }

  QualType VarType = ControlVar->getType();
  // FIXME: incomplete types not supported
  if (VarType->isDependentType())
    return false;

  // For decltype types, get the actual type
  const Type *VarTyPtr = VarType.getTypePtrOrNull();
  if (VarTyPtr && isa<DecltypeType>(VarTyPtr))
    VarType = cast<DecltypeType>(VarTyPtr)->getUnderlyingType();

  // The variable may not be const or volatile.
  // Assignment to const variables is checked before sema for cilk_for
  if (VarType.isVolatileQualified()) {
    S.Diag(InitLoc, diag::err_cilk_for_control_variable_qualifier)
      << "volatile";
    if (!IsDeclStmt)
      S.Diag(ControlVar->getLocation(), diag::note_local_variable_declared_here)
        << ControlVar->getIdentifier();
    return false;
  }

  // Don't allow non-local variables to be used as the control variable
  if (!ControlVar->isLocalVarDecl()) {
    S.Diag(InitLoc, diag::err_cilk_for_control_variable_not_local);
    return false;
  }

  // The variable shall have integral, pointer, or class type.
  // struct/class types only allowed in C++
  bool ValidType = false;
  if (S.getLangOpts().CPlusPlus &&
      (VarTyPtr->isClassType() || VarTyPtr->isStructureType()))
    ValidType = true;
  else if (VarTyPtr->isIntegralType(S.Context) || VarTyPtr->isPointerType())
    ValidType = true;

  if (!ValidType) {
    S.Diag(InitLoc, diag::err_cilk_for_control_variable_type);
    if (!IsDeclStmt)
      S.Diag(ControlVar->getLocation(), diag::note_local_variable_declared_here)
        << ControlVar->getIdentifier();
    return false;
  }

  return true;
}

static bool ExtractCilkForCondition(Sema &S,
                                    Expr *Cond,
                                    BinaryOperatorKind &CondOp,
                                    SourceLocation &OpLoc,
                                    Expr *&LHS,
                                    Expr *&RHS) {
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(Cond)) {
    CondOp = BO->getOpcode();
    OpLoc = BO->getOperatorLoc();
    LHS = BO->getLHS();
    RHS = BO->getRHS();
    return true;
  } else if (CXXOperatorCallExpr *OO = dyn_cast<CXXOperatorCallExpr>(Cond)) {
    CondOp = BinaryOperator::getOverloadedOpcode(OO->getOperator());
    if (OO->getNumArgs() == 2) {
      OpLoc = OO->getOperatorLoc();
      LHS = OO->getArg(0);
      RHS = OO->getArg(1);
      return true;
    }
  } else if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(Cond)) {
    switch (ICE->getCastKind()) {
    case CK_ConstructorConversion:
    case CK_UserDefinedConversion:
      S.Diag(Cond->getExprLoc(), diag::warn_cilk_for_cond_user_defined_conv)
        << (ICE->getCastKind() != CK_ConstructorConversion)
        << Cond->getSourceRange();
      // fallthrough
    default:
      break;
    }
    return ExtractCilkForCondition(S, ICE->getSubExpr(), CondOp, OpLoc, LHS, RHS);
  } else if (CXXMemberCallExpr *MC = dyn_cast<CXXMemberCallExpr>(Cond)) {
    CXXMethodDecl *MD = MC->getMethodDecl();
    if (isa<CXXConversionDecl>(MD))
      return ExtractCilkForCondition(S, MC->getImplicitObjectArgument(), CondOp,
                                     OpLoc, LHS, RHS);
  } else if (CXXBindTemporaryExpr *BT = dyn_cast<CXXBindTemporaryExpr>(Cond)) {
    return ExtractCilkForCondition(S, BT->getSubExpr(), CondOp, OpLoc, LHS, RHS);
  } else if (ExprWithCleanups *EWC = dyn_cast<ExprWithCleanups>(Cond))
    return ExtractCilkForCondition(S, EWC->getSubExpr(), CondOp, OpLoc, LHS, RHS);

  S.Diag(Cond->getExprLoc(), diag::err_cilk_for_invalid_cond_expr)
    << Cond->getSourceRange();
  return false;
}

static bool IsCilkForControlVarRef(Expr *E, VarDecl *ControlVar,
                                   CastKind &HasCast) {
  E = E->IgnoreParenNoopCasts(ControlVar->getASTContext());
  if (CXXConstructExpr *C = dyn_cast<CXXConstructExpr>(E)) {
    if (C->getConstructor()->isConvertingConstructor(false)) {
      HasCast = CK_ConstructorConversion;
      return IsCilkForControlVarRef(C->getArg(0), ControlVar, HasCast);
    }
  } else if (MaterializeTemporaryExpr *M = dyn_cast<MaterializeTemporaryExpr>(E)) {
    return IsCilkForControlVarRef(M->GetTemporaryExpr(), ControlVar, HasCast);
  } else if (CastExpr *C = dyn_cast<CastExpr>(E)) {
    HasCast = C->getCastKind();
    return IsCilkForControlVarRef(C->getSubExpr(), ControlVar, HasCast);
  } else if (DeclRefExpr *DR = dyn_cast<DeclRefExpr>(E)) {
    if (DR->getDecl() == ControlVar)
      return true;
  }

  return false;
}

static bool CanonicalizeCilkForCondOperands(Sema &S, VarDecl *ControlVar,
                                            Expr *Cond, Expr *&LHS,
                                            Expr *&RHS, int &Direction) {

  // The condition shall have one of the following two forms:
  //   var OP shift-expression
  //   shift-expression OP var
  // where var is the control variable, optionally enclosed in parentheses.
  CastKind HasCast = CK_NoOp;
  if (!IsCilkForControlVarRef(LHS, ControlVar, HasCast)) {
    HasCast = CK_NoOp;
    if (!IsCilkForControlVarRef(RHS, ControlVar, HasCast)) {
      S.Diag(Cond->getLocStart(), diag::err_cilk_for_cond_test_control_var)
        << ControlVar
        << Cond->getSourceRange();
      S.Diag(Cond->getLocStart(), diag::note_cilk_for_cond_allowed)
        << ControlVar;
      return false;
    } else {
      std::swap(LHS, RHS);
      Direction = -Direction;
    }
  }

  switch (HasCast) {
  case CK_ConstructorConversion:
  case CK_UserDefinedConversion:
    S.Diag(LHS->getLocStart(), diag::warn_cilk_for_cond_user_defined_conv)
      << (HasCast != CK_ConstructorConversion) << LHS->getSourceRange();
    // fallthrough
  default:
    break;
  }

  return true;
}

static void CheckCilkForCondition(Sema &S, SourceLocation CilkForLoc,
                                  VarDecl *ControlVar, Expr *Cond, Expr *&Limit,
                                  int &Direction, BinaryOperatorKind &Opcode) {
  SourceLocation OpLoc;
  Expr *LHS = 0;
  Expr *RHS = 0;

  if (!ExtractCilkForCondition(S, Cond, Opcode, OpLoc, LHS, RHS))
    return;

  // The operator denoted OP shall be one of !=, <=, <, >=, or >.
  switch (Opcode) {
  case BO_NE:
    Direction = 0;
    break;
  case BO_LT: case BO_LE:
    Direction = 1;
    break;
  case BO_GT: case BO_GE:
    Direction = -1;
    break;
  default:
    S.Diag(OpLoc, diag::err_cilk_for_invalid_cond_operator);
    return;
  }

  if (!CanonicalizeCilkForCondOperands(S, ControlVar, Cond, LHS, RHS, Direction))
    return;

  Limit = RHS;
}

// Returns true if OpSubExpr references ControlVar, false otherwise.
// If OpSubExpr does not reference ControlVar, a diagnostic is issued.
static bool CheckIncrementVar(Sema &S, const Expr *OpSubExpr,
                              const VarDecl *ControlVar)
{
  OpSubExpr = OpSubExpr->IgnoreImpCasts();
  const DeclRefExpr *VarRef = dyn_cast<DeclRefExpr>(OpSubExpr);
  if (!VarRef)
    return false;

  if (VarRef->getDecl() != ControlVar) {
    S.Diag(VarRef->getExprLoc(), diag::err_cilk_for_increment_not_control_var)
      << ControlVar;
    return false;
  }

  return true;
}

static bool IsValidCilkForIncrement(Sema &S, Expr *Increment,
                                    const VarDecl *ControlVar,
                                    bool &HasConstantIncrement,
                                    llvm::APSInt &Stride, Expr *&StrideExpr,
                                    SourceLocation &RHSLoc) {
  Increment = Increment->IgnoreParens();
  if (ExprWithCleanups *E = dyn_cast<ExprWithCleanups>(Increment))
    Increment = E->getSubExpr();

  // Simple increment or decrement -- always OK
  if (UnaryOperator *U = dyn_cast<UnaryOperator>(Increment)) {
    if (!CheckIncrementVar(S, U->getSubExpr(), ControlVar))
      return false;

    if (U->isIncrementDecrementOp()) {
      HasConstantIncrement = true;
      Stride = llvm::APInt(64, U->isIncrementOp() ? 1 : -1, true);
      StrideExpr = S.ActOnIntegerConstant(Increment->getExprLoc(), 1).get();
      if (U->isDecrementOp())
        StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                    UO_Minus, StrideExpr).get();
      return true;
    }
  }

  // In the case of += or -=, whether built-in or overloaded, we need to check
  // the type of the right-hand side. In that case, RHS will be set to a
  // non-null value.
  Expr *RHS = 0;
  // Direction is 1 if the operator is +=, -1 if it is -=
  int Direction = 0;
  StringRef OperatorName;

  if (CXXOperatorCallExpr *C = dyn_cast<CXXOperatorCallExpr>(Increment)) {
    OverloadedOperatorKind Overload = C->getOperator();

    // operator++() or operator--() -- always OK
    if (Overload == OO_PlusPlus || Overload == OO_MinusMinus) {
      HasConstantIncrement = true;
      Stride = llvm::APInt(64, Overload == OO_PlusPlus ? 1 : -1, true);
      StrideExpr = S.ActOnIntegerConstant(Increment->getExprLoc(), 1).get();
      if (Overload == OO_MinusMinus)
        StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                    UO_Minus, StrideExpr).get();
      return true;
    }

    // operator+=() or operator-=() -- defer checking of the RHS type
    if (Overload == OO_PlusEqual || Overload == OO_MinusEqual) {
      RHS = C->getArg(1);
      OperatorName = (Overload == OO_PlusEqual ? "+=" : "-=");
      Direction = Overload == OO_PlusEqual ? 1 : -1;
    }

    if (!CheckIncrementVar(S, C->getArg(0), ControlVar))
      return false;
  }

  if (BinaryOperator *B = dyn_cast<CompoundAssignOperator>(Increment)) {
    if (!CheckIncrementVar(S, B->getLHS(), ControlVar))
      return false;

    // += or -= -- defer checking of the RHS type
    if (B->isAdditiveAssignOp()) {
      RHS = B->getRHS();
      OperatorName = B->getOpcodeStr();
      Direction = B->getOpcode() == BO_AddAssign ? 1 : -1;
    }
  }

  // If RHS is non-null, it's a += or -=, either built-in or overloaded.
  // We need to check that the RHS has the correct type.
  if (RHS) {
    if (!RHS->getType()->isIntegralOrEnumerationType()) {
      S.Diag(Increment->getExprLoc(),
        diag::err_cilk_for_invalid_increment_rhs) << OperatorName;
      return false;
    }

    HasConstantIncrement = RHS->EvaluateAsInt(Stride, S.Context);
    StrideExpr = RHS;
    if (Direction == -1) {
      Stride = -Stride;
      StrideExpr = S.BuildUnaryOp(S.getCurScope(), Increment->getExprLoc(),
                                  UO_Minus, StrideExpr).get();
    }
    RHSLoc = RHS->getExprLoc();
    return true;
  }

  // If we reached this point, the basic form is invalid. Issue a diagnostic.
  S.Diag(Increment->getExprLoc(), diag::err_cilk_for_invalid_increment);
  return false;
}

StmtResult
Sema::ActOnCilkForStmt(SourceLocation CilkForLoc, SourceLocation LParenLoc,
                       Stmt *First, FullExprArg Second, FullExprArg Third,
                       SourceLocation RParenLoc, Stmt *Body) {
  assert(First && "expected init");
  assert(Second.get() && "expected cond");
  assert(Third.get() && "expected increment");

  Expr *Increment = Third.release().takeAs<Expr>();

  // Check loop initializer and get control variable
  VarDecl *ControlVar = 0;
  if (!CheckCilkForInitStmt(*this, First, ControlVar))
    return StmtError();

  if (ControlVar->getType()->isDependentType())
    return StmtError();

  if (ControlVar->getType()->isReferenceType())
    return StmtError();

  // Check loop condition
  CheckForLoopConditionalStatement(*this, Second.get(), Increment, Body);

  Expr *Limit = 0;
  int CondDirection = 0;
  BinaryOperatorKind Opcode;
  CheckCilkForCondition(*this, CilkForLoc, ControlVar, Second.get(),
                        Limit, CondDirection, Opcode);
  if (!Limit)
    return StmtError();
  if (Limit->getType()->isDependentType())
    return StmtError();

  // Check increment
  llvm::APSInt Stride;
  Expr *StrideExpr = 0;
  bool HasConstantIncrement;
  SourceLocation IncrementRHSLoc;
  if (!IsValidCilkForIncrement(*this, Increment, ControlVar,
                               HasConstantIncrement, Stride, StrideExpr,
                               IncrementRHSLoc))
    return StmtError();

  // Check consistency between loop condition and increment only if the
  // increment amount is known at compile-time.
  if (HasConstantIncrement) {
    if (!Stride) {
      Diag(IncrementRHSLoc, diag::err_cilk_for_increment_zero);
      return StmtError();
    }

    if ((CondDirection > 0 && Stride.isNegative()) ||
        (CondDirection < 0 && Stride.isStrictlyPositive())) {
      Diag(Increment->getExprLoc(), diag::err_cilk_for_increment_inconsistent)
        << (CondDirection > 0);
      Diag(Increment->getExprLoc(), diag::note_cilk_constant_stride)
        << Stride.toString(10, true)
        << SourceRange(Increment->getExprLoc(), Increment->getLocEnd());
      return StmtError();
    }
  }

  // Build end - begin
  Expr *Begin = BuildDeclRefExpr(ControlVar,
                                 ControlVar->getType().getNonReferenceType(),
                                 VK_LValue,
                                 ControlVar->getLocation()).release();
  Expr *End = Limit;
  if (CondDirection < 0)
    std::swap(Begin, End);

  ExprResult Span = BuildBinOp(CurScope, CilkForLoc, BO_Sub, End, Begin);

  if (Span.isInvalid()) {
    // error getting operator-()
    Diag(CilkForLoc, diag::err_cilk_for_difference_ill_formed);
    Diag(Begin->getLocStart(), diag::note_cilk_for_begin_expr)
      << Begin->getSourceRange();
    Diag(End->getLocStart(), diag::note_cilk_for_end_expr)
      << End->getSourceRange();
    return StmtError();
  }

  if (!Span.get()->getType()->isIntegralOrEnumerationType()) {
    // non-integral type
    Diag(CilkForLoc, diag::err_non_integral_cilk_for_difference_type)
      << Span.get()->getType();
    Diag(Begin->getLocStart(), diag::note_cilk_for_begin_expr)
      << Begin->getSourceRange();
    Diag(End->getLocStart(), diag::note_cilk_for_end_expr)
      << End->getSourceRange();
    return StmtError();
  }

  DiagnoseUnusedExprResult(First);
  DiagnoseUnusedExprResult(Increment);
  DiagnoseUnusedExprResult(Body);
  if (isa<NullStmt>(Body))
    getCurCompoundScope().setHasEmptyLoopBodies();

  // Generate the loop count expression according to the following:
  // ===========================================================================
  // |     Condition syntax             |       Loop count                     |
  // ===========================================================================
  // | if var < limit or limit > var    | (span+(stride-1))/stride             |
  // ---------------------------------------------------------------------------
  // | if var > limit or limit < var    | (span+(stride-1))/-stride            |
  // ---------------------------------------------------------------------------
  // | if var <= limit or limit >= var  | ((span+1)+(stride-1))/stride         |
  // ---------------------------------------------------------------------------
  // | if var >= limit or limit <= var  | ((span+1)+(stride-1))/-stride        |
  // ---------------------------------------------------------------------------
  // | if var != limit or limit != var  | if stride is positive,               |
  // |                                  |            span/stride               |
  // |                                  | otherwise, span/-stride              |
  // |                                  | We don't need "+(stride-1)" for the  |
  // |                                  | span in this case since the incr/decr|
  // |                                  | operator should add up to the        |
  // |                                  | limit exactly for a valid loop.      |
  // ---------------------------------------------------------------------------
  Expr *LoopCount = 0;
  // Build "-stride"
  Expr *NegativeStride = BuildUnaryOp(getCurScope(), Increment->getExprLoc(),
                                      UO_Minus, StrideExpr).get();
  // Build "stride-1"
  Expr *StrideMinusOne =
      BuildBinOp(getCurScope(), Increment->getExprLoc(), BO_Sub,
                 (CondDirection == 1) ? StrideExpr : NegativeStride,
                 ActOnIntegerConstant(CilkForLoc, 1).get()).get();

  if (Opcode == BO_NE) {
    // Build "stride<0"
    Expr *StrideLessThanZero =
        BuildBinOp(getCurScope(), CilkForLoc, BO_LT, StrideExpr,
                   ActOnIntegerConstant(CilkForLoc, 0).get()).get();
    // Build "(stride<0)?-stride:stride"
    ExprResult StrideCondExpr = ActOnConditionalOp(
        CilkForLoc, CilkForLoc, StrideLessThanZero, NegativeStride, StrideExpr);

    // Build "-span"
    Expr *NegativeSpan =
        BuildUnaryOp(getCurScope(), CilkForLoc, UO_Minus, Span.get()).get();

    // Updating span to be "(stride<0)?-span:span"
    Span = ActOnConditionalOp(CilkForLoc, CilkForLoc, StrideLessThanZero,
                              NegativeSpan, Span.get());

    // Build "span/(stride<0)?-stride:stride"
    LoopCount = BuildBinOp(getCurScope(), CilkForLoc, BO_Div, Span.get(),
                           StrideCondExpr.get()).get();
  } else {
    // Updating span to be "span+(stride-1)"
    Span = BuildBinOp(getCurScope(), CilkForLoc, BO_Add, Span.get(),
                      StrideMinusOne);
    if (Opcode == BO_LE || Opcode == BO_GE)
      // Updating span to be "span+1"
      Span = CreateBuiltinBinOp(CilkForLoc, BO_Add, Span.get(),
                                ActOnIntegerConstant(CilkForLoc, 1).get());
    // Build "span/stride" if CondDirection==1, otherwise "span/-stride"
    LoopCount =
        BuildBinOp(getCurScope(), CilkForLoc, BO_Div, Span.get(),
                   (CondDirection == 1) ? StrideExpr : NegativeStride).get();
  }

  QualType LoopCountExprType = LoopCount->getType();
  QualType LoopCountType = Context.UnsignedLongLongTy;
  // Loop count should be either u32 or u64 in Cilk Plus.
  if (Context.getTypeSize(LoopCountExprType) > 64) {
    // TODO: Emit warning about truncation to u64.
  } else if (Context.getTypeSize(LoopCountExprType) <= 32) {
    LoopCountType = Context.UnsignedIntTy;
  }
  // Implicitly casting LoopCount to u32/u64.
  LoopCount =
      ImpCastExprToType(LoopCount, LoopCountType, CK_IntegralCast).get();

  return BuildCilkForStmt(CilkForLoc, LParenLoc, First, Second.get(),
                          Third.get(), RParenLoc, Body, LoopCount, StrideExpr);
}

static
void buildCilkForCaptureLists(SmallVectorImpl<CilkForStmt::Capture> &Captures,
                              SmallVectorImpl<Expr *> &CaptureInits,
                              ArrayRef<CilkForScopeInfo::Capture> Candidates) {
  typedef ArrayRef<CilkForScopeInfo::Capture>::const_iterator CaptureIter;

  for (CaptureIter CI = Candidates.begin(), CE = Candidates.end();
       CI != CE; ++CI) {
    if (CI->isThisCapture()) {
      Captures.push_back(CilkForStmt::Capture(CI->getLocation(),
                                              CilkForStmt::VCK_This));
      CaptureInits.push_back(CI->getCopyExpr());
      continue;
    }

    CilkForStmt::VariableCaptureKind Kind
      = CI->isCopyCapture() ? CilkForStmt::VCK_ByCopy : CilkForStmt::VCK_ByRef;

    Captures.push_back(CilkForStmt::Capture(CI->getLocation(), Kind,
                                            CI->getVariable()));
    CaptureInits.push_back(CI->getCopyExpr());
  }
}

StmtResult Sema::BuildCilkForStmt(SourceLocation CilkForLoc,
                                  SourceLocation LParenLoc,
                                  Stmt *Init, Expr *Cond, Expr *Inc,
                                  SourceLocation RParenLoc, Stmt *Body,
                                  Expr *LoopCount, Expr *Stride) {
  CilkForScopeInfo *FSI = getCurCilkFor();
  assert(FSI && "CilkForScopeInfo is out of sync");

  SmallVector<CilkForStmt::Capture, 4> Captures;
  SmallVector<Expr *, 4> CaptureInits;
  buildCilkForCaptureLists(Captures, CaptureInits, FSI->Captures);

  // Set the variable capturing record declaration.
  RecordDecl *RD = FSI->TheRecordDecl;
  RD->completeDefinition();

  CilkForDecl *CFD = FSI->TheCilkForDecl;
  CFD->setContextRecordDecl(RD);
  CFD->setLoopControlVar(FSI->LoopControlVar);
  CFD->setInnerLoopControlVar(FSI->InnerLoopControlVar);

  // Set parameters for the outlined function.
  // Build the initial value for the inner loop control variable.
  QualType Ty = LoopCount->getType().getNonReferenceType();
  if (!Ty->isDependentType()) {
    // Context for variable capturing.
    CFD->setContextParam(FSI->ContextParam);
    DeclContext *DC = CilkForDecl::castToDeclContext(CFD);

    // In the following, the source location of the loop control variable
    // will be used for diagnostics.
    SourceLocation VarLoc = FSI->LoopControlVar->getLocation();
    assert(VarLoc.isValid() && "invalid source location");

    ImplicitParamDecl *Low
      = ImplicitParamDecl::Create(Context, DC, VarLoc,
                                  &Context.Idents.get("__low"), Ty);
    DC->addDecl(Low);

    ImplicitParamDecl *High
      = ImplicitParamDecl::Create(Context, DC, VarLoc,
                                  &Context.Idents.get("__high"), Ty);
    DC->addDecl(High);

    CFD->setLowHighParams(Low, High);

    // Build a full expression "inner_loop_var += stride * low"
    {
      EnterExpressionEvaluationContext Scope(*this, PotentiallyEvaluated);

      // Both low and stride experssions are of type integral.
      ExprResult LowExpr = BuildDeclRefExpr(Low, Ty, VK_LValue, VarLoc);
      assert(!LowExpr.isInvalid() && "invalid expr");

      assert(Stride && "invalid null stride expression");
      ExprResult StepExpr
        = BuildBinOp(CurScope, VarLoc, BO_Mul, LowExpr.get(), Stride);
      assert(!StepExpr.isInvalid() && "invalid expression");

      VarDecl *InnerVar = CFD->getInnerLoopControlVar();
      ExprResult InnerVarExpr
        = BuildDeclRefExpr(InnerVar, InnerVar->getType(), VK_LValue, VarLoc);
      assert(!InnerVarExpr.isInvalid() && "invalid expression");

      // The '+=' operation could fail if the loop control variable is of
      // class type and this may introduce cleanups.
      ExprResult AdjustExpr = BuildBinOp(CurScope, VarLoc, BO_AddAssign,
                                         InnerVarExpr.get(), StepExpr.get());
      if (!AdjustExpr.isInvalid()) {
        AdjustExpr = MaybeCreateExprWithCleanups(AdjustExpr);
        CFD->setInnerLoopVarAdjust(AdjustExpr.get());
      }
      // FIXME: Should mark the CilkForDecl as invalid?
      // FIXME: Should install the adjustment expression into the CilkForStmt?
    }
  }

  PopExpressionEvaluationContext();
  PopDeclContext();
  PopFunctionScopeInfo();

  // FIXME: Handle ExprNeedsCleanups flag.
  // ExprNeedsCleanups = FSI->ExprNeedsCleanups;

  CilkForStmt *Result = CilkForStmt::Create(Context, Init, Cond, Inc, Body,
                                            LoopCount, CilkForLoc, LParenLoc,
                                            RParenLoc, CFD, Captures,
                                            CaptureInits);

  return Owned(Result);
}

// Find the loop control variable. Returns null if not found.
static const VarDecl *getLoopControlVariable(Sema &S, StmtResult InitStmt) {
  if (InitStmt.isInvalid())
    return 0;

  Stmt *Init = InitStmt.get();

  // No initialization.
  if (!Init)
    return 0;

  const VarDecl *Candidate = 0;

  // Initialization is a declaration statement.
  if (DeclStmt *DS = dyn_cast<DeclStmt>(Init)) {
    if (!DS->isSingleDecl())
      return 0;

    if (VarDecl *Var = dyn_cast<VarDecl>(DS->getSingleDecl()))
      Candidate = Var;
  } else {
    // Initialization is an expression.
    BinaryOperator *Op = 0;
    if (Expr *E = dyn_cast<Expr>(Init)) {
      E = E->IgnoreParenNoopCasts(S.Context);
      Op = dyn_cast<BinaryOperator>(E);
    }

    if (!Op || !Op->isAssignmentOp())
      return 0;

    Expr *E = Op->getLHS();
    if (!E)
      return 0;

    E = E->IgnoreParenNoopCasts(S.Context);
    DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(E);
    if (!LHS)
      return 0;

    if (VarDecl *Var = dyn_cast<VarDecl>(LHS->getDecl()))
      Candidate = Var;
  }

  // Only local variables can be a loop control variable.
  if (Candidate && Candidate->isLocalVarDecl())
    return Candidate;

  // Cannot find the loop control variable.
  return 0;
}

void Sema::ActOnStartOfCilkForStmt(SourceLocation CilkForLoc, Scope *CurScope,
                                   StmtResult FirstPart) {
  DeclContext *DC = CurContext;
  while (!(DC->isFunctionOrMethod() || DC->isRecord() || DC->isFileContext()))
    DC = DC->getParent();

  // Create a C/C++ record decl for variable capturing.
  RecordDecl *RD = 0;
  {
    IdentifierInfo *Id = &PP.getIdentifierTable().get("cilk.for.capture");
    if (getLangOpts().CPlusPlus)
      RD = CXXRecordDecl::Create(Context, TTK_Struct, DC, CilkForLoc,
                                 CilkForLoc, Id);
    else
      RD = RecordDecl::Create(Context, TTK_Struct, DC, CilkForLoc,
                              CilkForLoc, Id);

    DC->addDecl(RD);
    RD->setImplicit();
    RD->startDefinition();
  }

  // Start a CilkForDecl.
  CilkForDecl *CFD = CilkForDecl::Create(Context, CurContext);
  DC->addDecl(CFD);

  const VarDecl *VD = getLoopControlVariable(*this, FirstPart);
  PushCilkForScope(CurScope, CFD, RD, VD, CilkForLoc);

  if (CurScope)
    PushDeclContext(CurScope, CFD);
  else
    CurContext = CFD;

  PushExpressionEvaluationContext(PotentiallyEvaluated);
}

void Sema::ActOnCilkForStmtError(bool IsInstantiation) {
  DiscardCleanupsInEvaluationContext();
  PopExpressionEvaluationContext();
  if (!IsInstantiation)
    PopDeclContext();

  CilkForScopeInfo *FSI = getCurCilkFor();
  RecordDecl *Record = FSI->TheRecordDecl;
  Record->setInvalidDecl();

  SmallVector<Decl*, 4> Fields;
  for (RecordDecl::field_iterator I = Record->field_begin(),
                                  E = Record->field_end(); I != E; ++I)
    Fields.push_back(*I);

  ActOnFields(/*Scope=*/0, Record->getLocation(), Record, Fields,
              SourceLocation(), SourceLocation(), /*AttributeList=*/0);

  PopFunctionScopeInfo();
}
