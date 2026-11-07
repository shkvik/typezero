#include "compiler/check/stmt/switch_checker/switch_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/declare/type_declarer/type_declarer.h"
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

SwitchChecker::SwitchChecker(Checker &owner)
    : checker(owner) {}

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
  bool results = Checker::holdsResults(type) || Checker::holdsResults(testType);
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
  stmt->discriminant = this->checker.exprChecker().checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  ResultValueComparable comparable = SwitchChecker::valueType(type);
  if (comparable.isErr()) {
    this->checker.diags_.error(stmt->discriminant->loc(), comparable.error().message);
    type = Type::error();
  }
  this->checker.pushScope();
  std::vector<Stmt *> all;
  for (SwitchCase &c : stmt->cases) {
    all.insert(all.end(), c.body.begin(), c.body.end());
  }
  this->checker.typeDeclarer().declareTypes(all);

  const Type *base = type->nonOptional();
  const Type *hint = base->isNumeric() || base->isDecimal() || base->isJson() ? base
                     : base->isLiteral() || base->isUnion()                   ? type
                                                                              : nullptr;
  Checker::Flow tests = this->checker.flow_;
  std::vector<Checker::Flow> matched(stmt->cases.size());
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    this->checker.flow_ = tests;
    c.test = this->checker.exprChecker().checkExpr(c.test, hint);
    if (auto *typeOf = dyn_cast<UnaryExpr>(stmt->discriminant);
        typeOf && typeOf->op == UnaryOp::TypeOf && isa<StringLitExpr>(c.test)) {
      this->checker.binaryChecker().checkTypeOfName(typeOf->operand, cast<StringLitExpr>(c.test));
    }
    const Type *testType = c.test->type;
    Checker::Flow whenTrue = this->checker.flow_;
    Checker::Flow whenFalse = this->checker.flow_;
    bool generic = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
    if (!generic && !type->isError() && !testType->isError() && !testType->isUndefined()) {
      ResultTestsComparable compared = SwitchChecker::caseType(type, testType);
      const Type *common = compared.isOk() ? compared.value() : nullptr;
      if (compared.isErr()) {
        this->checker.diags_.error(c.test->loc(), compared.error().message);
      } else if (!common->isError()) {
        if (common->nonOptional()->isFunction()) {
          this->checker.file_->comparesFunctions = true;
        }
        bool constant = SwitchChecker::isConstantTest(c.test) &&
                        !SwitchChecker::isConstantTest(stmt->discriminant);
        c.test = this->checker.typeRelations().convert(c.test, common);
        c.compareType = common;
        if (constant) {
          BinaryExpr equal(c.loc, BinaryOp::Eq,
                           this->checker.typeRelations().convert(stmt->discriminant, common),
                           c.test);
          equal.type = Type::boolTy();
          this->checker.conditionFacts().addConditionFacts(&equal, whenTrue, whenFalse);
        }
      }
    } else if (testType->isUndefined() && isa<UndefinedExpr>(c.test)) {
      BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
      equal.type = Type::boolTy();
      this->checker.conditionFacts().addConditionFacts(&equal, whenTrue, whenFalse);
    }
    matched[i] = whenTrue;
    tests = whenFalse;
  }
  if (std::optional<std::vector<const Type *>> left =
          this->checker.unionChecker().unhandledCases(stmt)) {
    ResultCasesAllHandled handled = SwitchChecker::casesHandled(stmt, *left);
    if (handled.isOk()) {
      stmt->exhaustive = true;
      tests.unreachable = true;
    } else {
      this->checker.diags_.error(stmt->discriminant->loc(), handled.error().message);
    }
  }

  this->checker.jumpChecker().pushTarget(Checker::JumpTarget::Kind::Switch);
  size_t position = 0;
  Checker::Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    this->checker.flow_ = FlowChecker::join(fallthrough, c.test ? matched[i] : tests);
    for (Stmt *s : c.body) {
      this->checker.block_ = all;
      this->checker.blockIndex_ = position++;
      this->checker.stmtChecker().checkStmt(s);
    }
    fallthrough = this->checker.flow_;
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          if (d.sym) {
            this->checker.otherCaseVars_.insert(d.sym);
          }
        }
      }
    }
  }
  Checker::Flow exit = fallthrough;
  if (!hasDefault) {
    exit = FlowChecker::join(exit, tests);
  }
  exit = FlowChecker::join(exit, this->checker.targets_.back().breaks);
  this->checker.targets_.pop_back();
  for (SwitchCase &c : stmt->cases) {
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          this->checker.otherCaseVars_.erase(d.sym);
        }
      }
    }
  }
  this->checker.popScope();
  this->checker.flow_ = exit;
}

}
