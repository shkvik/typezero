#include "compiler/pipeline/check_program/topics/stmt/switch_checker/switch_checker.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/flow/condition_facts/condition_facts.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/expr/binary_checker/binary_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/jump_checker/jump_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/stmt_checker/stmt_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/union_checker/union_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool SwitchChecker::isConstantTest(const Expr *test) {
  if (auto *unary = dyn_cast<UnaryExpr>(test); unary && unary->op == UnaryOp::Neg) {
    test = unary->operand;
  }

  return isa<IntLitExpr>(test) || isa<FloatLitExpr>(test) || isa<StringLitExpr>(test) ||
         isa<BoolLitExpr>(test) || isa<UndefinedExpr>(test);
}

SwitchChecker::SwitchChecker(CheckState &owner)
    : state(owner) {}

SwitchChecker::ResultValueComparable SwitchChecker::valueType(const Type *type) {
  if (type->isVoid()) {
    std::string message = ErrorTextValueType::isVoid();

    return err(SwitchCheckerValueTypeError{
        .code = SwitchCheckerValueTypeErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

SwitchChecker::ResultTestsComparable SwitchChecker::caseType(const Type *type,
                                                             const Type *testType) {
  const Type *common = TypeRelations::comparisonType(type, testType);
  bool results = type->holdsResults() || testType->holdsResults();
  if (!common && results) {
    std::string message = ErrorTextCaseType::results(testType->name(), type->name());

    return err(SwitchCheckerCaseTypeError{
        .code = SwitchCheckerCaseTypeErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }
  if (!common) {
    std::string message = ErrorTextCaseType::mismatch(testType->name(), type->name());

    return err(SwitchCheckerCaseTypeError{
        .code = SwitchCheckerCaseTypeErrorCode::NOT_COMPARABLE,
        .message = message,
    });
  }

  return ok(common);
}

SwitchChecker::ResultCasesAllHandled
SwitchChecker::casesHandled(const SwitchStmt *stmt, llvm::ArrayRef<const Type *> left) {
  if (!left.empty()) {
    std::string names;
    for (size_t i = 0; i < left.size(); ++i) {
      names += (i == 0 ? "" : i + 1 == left.size() ? " and " : ", ") + left[i]->name().str();
    }
    std::string message = ErrorTextCasesLeft::unhandled(names, left.front()->name());

    return err(SwitchCheckerCasesHandledError{
        .code = SwitchCheckerCasesHandledErrorCode::CASES_LEFT,
        .message = message,
    });
  }

  return ok(stmt);
}

void SwitchChecker::checkSwitch(SwitchStmt *stmt) {
  stmt->discriminant = ExprChecker(this->state).checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  ResultValueComparable comparable = SwitchChecker::valueType(type);
  if (comparable.isErr()) {
    this->state.getDiagnostics().error(stmt->discriminant->loc(), comparable.error().message);
    type = Type::error();
  }
  this->state.getScopeStack().openScope();
  std::vector<Stmt *> all;
  for (SwitchCase &c : stmt->cases) {
    all.insert(all.end(), c.body.begin(), c.body.end());
  }
  TypeDeclarer(this->state).declareTypes(all);

  const Type *base = type->nonOptional();
  const Type *hint = base->isNumeric() || base->isDecimal() || base->isJson() ? base
                     : base->isLiteral() || base->isUnion()                   ? type
                                                                              : nullptr;
  Flow tests = this->state.getFlowState().getFlow();
  std::vector<Flow> matched(stmt->cases.size());
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    this->state.getFlowState().replaceFlow(tests);
    c.test = ExprChecker(this->state).checkExpr(c.test, hint);
    if (auto *typeOf = dyn_cast<UnaryExpr>(stmt->discriminant);
        typeOf && typeOf->op == UnaryOp::TypeOf && isa<StringLitExpr>(c.test)) {
      BinaryChecker::ResultTypeOfNameKnown known =
          BinaryChecker(this->state).checkTypeOfName(typeOf->operand, cast<StringLitExpr>(c.test));
      if (known.isErr() &&
          known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
        this->state.getDiagnostics().error(c.test->loc(), known.error().message);
      }
    }
    const Type *testType = c.test->type;
    Flow whenTrue = this->state.getFlowState().getFlow();
    Flow whenFalse = this->state.getFlowState().getFlow();
    bool generic = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
    if (!generic && !type->isError() && !testType->isError() && !testType->isUndefined()) {
      ResultTestsComparable compared = SwitchChecker::caseType(type, testType);
      const Type *common = compared.isOk() ? compared.value() : nullptr;
      if (compared.isErr()) {
        this->state.getDiagnostics().error(c.test->loc(), compared.error().message);
      } else if (!common->isError()) {
        if (common->nonOptional()->isFunction()) {
          this->state.getProgramUse().markComparesFunctions();
        }
        bool constant = SwitchChecker::isConstantTest(c.test) &&
                        !SwitchChecker::isConstantTest(stmt->discriminant);
        c.test = TypeRelations(this->state).convert(c.test, common);
        c.compareType = common;
        if (constant) {
          BinaryExpr equal(c.loc, BinaryOp::Eq,
                           TypeRelations(this->state).convert(stmt->discriminant, common), c.test);
          equal.type = Type::boolTy();
          ConditionFacts(this->state).addConditionFacts(&equal, whenTrue, whenFalse);
        }
      }
    } else if (testType->isUndefined() && isa<UndefinedExpr>(c.test)) {
      BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
      equal.type = Type::boolTy();
      ConditionFacts(this->state).addConditionFacts(&equal, whenTrue, whenFalse);
    }
    matched[i] = whenTrue;
    tests = whenFalse;
  }
  if (std::optional<std::vector<const Type *>> left =
          UnionChecker(this->state).unhandledCases(stmt)) {
    ResultCasesAllHandled handled = SwitchChecker::casesHandled(stmt, *left);
    if (handled.isOk()) {
      stmt->exhaustive = true;
      tests.unreachable = true;
    } else {
      this->state.getDiagnostics().error(stmt->discriminant->loc(), handled.error().message);
    }
  }

  JumpChecker(this->state).pushTarget(JumpTarget::Kind::Switch);
  size_t position = 0;
  Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    this->state.getFlowState().replaceFlow(
        FlowChecker::join(fallthrough, c.test ? matched[i] : tests));
    for (Stmt *s : c.body) {
      this->state.getBlockPosition().rememberBlockPosition(all, position++);
      StmtChecker(this->state).checkStmt(s);
    }
    fallthrough = this->state.getFlowState().getFlow();
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          if (d.sym) {
            this->state.getBlockPosition().addOtherCaseVar(d.sym);
          }
        }
      }
    }
  }
  Flow exit = fallthrough;
  if (!hasDefault) {
    exit = FlowChecker::join(exit, tests);
  }
  exit = FlowChecker::join(exit, this->state.getBlockPosition().leaveJumpTarget().breaks);
  for (SwitchCase &c : stmt->cases) {
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          this->state.getBlockPosition().forgetOtherCaseVar(d.sym);
        }
      }
    }
  }
  Scopes(this->state).closeScope();
  this->state.getFlowState().replaceFlow(exit);
}

}
