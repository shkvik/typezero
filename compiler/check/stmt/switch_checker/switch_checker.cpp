#include "compiler/check/stmt/switch_checker/switch_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/stmt/jump_checker/jump_checker.h"
#include "compiler/check/stmt/stmt_checker/stmt_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

// A literal case test: comparing with it cannot change any variable, so what
// the comparison says about the discriminant holds in the case's body.
static bool isConstantTest(const Expr *test) {
  if (auto *unary = dyn_cast<UnaryExpr>(test); unary && unary->op == UnaryOp::Neg) {
    test = unary->operand;
  }

  return isa<IntLitExpr>(test) || isa<FloatLitExpr>(test) || isa<StringLitExpr>(test) ||
         isa<BoolLitExpr>(test) || isa<UndefinedExpr>(test);
}

SwitchChecker::SwitchChecker(Checker &checker) : checker_(checker) {}

// The tests run in order until one matches; then control enters that case
// (or `default` if none matches) and falls through the cases after it until
// a `break`.
void SwitchChecker::checkSwitch(SwitchStmt *stmt) {
  stmt->discriminant = checker_.exprChecker().checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  if (type->isVoid()) {
    checker_.diags_.error(stmt->discriminant->loc(),
                          "an expression of type 'void' cannot be compared");
    type = Type::error();
  }
  // As in JavaScript, the cases share one scope.
  checker_.pushScope();
  std::vector<Stmt *> all;
  for (SwitchCase &c : stmt->cases) {
    all.insert(all.end(), c.body.begin(), c.body.end());
  }
  checker_.typeDeclarer().declareTypes(all);

  const Type *base = type->nonOptional();
  // `case null:` of a Json, which matches only null, as `===` does. A case of
  // a literal type or a union takes its literal type: `case "circle":`.
  const Type *hint = base->isNumeric() || base->isDecimal() || base->isJson() ? base
                     : base->isLiteral() || base->isUnion()                   ? type
                                                                              : nullptr;
  Checker::Flow tests = checker_.flow_; // what holds while the tests run
  std::vector<Checker::Flow> matched(stmt->cases.size());
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    if (!c.test) {
      continue;
    }
    checker_.flow_ = tests;
    c.test = checker_.exprChecker().checkExpr(c.test, hint);
    const Type *testType = c.test->type;
    Checker::Flow whenTrue = checker_.flow_, whenFalse = checker_.flow_;
    // As with `==`: a value of a type parameter compares with anything here,
    // and each instance checks the types it has.
    bool generic = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
    if (!generic && !type->isError() && !testType->isError() && !testType->isUndefined()) {
      const Type *common = TypeRelations::comparisonType(type, testType);
      if (!common) {
        checker_.diags_.error(c.test->loc(),
                              "a case of type '" + testType->name() +
                                  "' cannot be compared with a 'switch' value of type '" +
                                  type->name() + "'" +
                                  (Checker::holdsResults(type) || Checker::holdsResults(testType)
                                       ? "; two Results are never equal, as they are two "
                                         "objects in JavaScript: switch on what they hold"
                                       : ""));
      } else if (!common->isError()) {
        if (common->nonOptional()->isFunction()) {
          checker_.file_->comparesFunctions = true;
        }
        // A literal discriminant has nothing to learn about, and converting
        // it here would change its type.
        bool constant = isConstantTest(c.test) && !isConstantTest(stmt->discriminant);
        c.test = checker_.typeRelations().convert(c.test, common);
        c.compareType = common;
        if (constant) {
          // `discriminant == test`, to learn from it as from a condition.
          BinaryExpr equal(c.loc, BinaryOp::Eq,
                           checker_.typeRelations().convert(stmt->discriminant, common), c.test);
          equal.type = Type::boolTy();
          checker_.conditionFacts().addConditionFacts(&equal, whenTrue, whenFalse);
        }
      }
    } else if (testType->isUndefined() && isa<UndefinedExpr>(c.test)) {
      BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
      equal.type = Type::boolTy();
      checker_.conditionFacts().addConditionFacts(&equal, whenTrue, whenFalse);
    }
    matched[i] = whenTrue;
    tests = whenFalse;
  }
  // A switch on a literal type handles each of its values (docs/objects.md,
  // section 7), or a default says that leaving some out is meant.
  if (std::optional<std::vector<const Type *>> left =
          checker_.unionChecker().unhandledCases(stmt)) {
    if (left->empty()) {
      stmt->exhaustive = true;
      tests.unreachable = true;
    } else {
      std::string names;
      for (size_t i = 0; i < left->size(); ++i) {
        names += (i == 0 ? "" : i + 1 == left->size() ? " and " : ", ") + (*left)[i]->name().str();
      }
      checker_.diags_.error(stmt->discriminant->loc(),
                            "the switch does not handle " + names + "; add 'case " +
                                left->front()->name().str() +
                                ":', or 'default: break' if leaving it out is meant");
    }
  }

  checker_.jumpChecker().pushTarget(Checker::JumpTarget::Kind::Switch);
  // The statements of the cases after one go on after it (see block_).
  size_t position = 0;
  Checker::Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    checker_.flow_ = FlowChecker::join(fallthrough, c.test ? matched[i] : tests);
    for (Stmt *s : c.body) {
      checker_.block_ = all;
      checker_.blockIndex_ = position++;
      checker_.stmtChecker().checkStmt(s);
    }
    fallthrough = checker_.flow_;
    // A variable declared in this case is in scope in the cases after it,
    // but JavaScript reports its use there as an error unless control fell
    // through its declaration. Checker rejects such uses outright.
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          if (d.sym) {
            checker_.otherCaseVars_.insert(d.sym);
          }
        }
      }
    }
  }
  Checker::Flow exit = fallthrough;
  if (!hasDefault) {
    exit = FlowChecker::join(exit, tests);
  }
  exit = FlowChecker::join(exit, checker_.targets_.back().breaks);
  checker_.targets_.pop_back();
  for (SwitchCase &c : stmt->cases) {
    for (Stmt *s : c.body) {
      if (auto *decl = dyn_cast<VarDeclStmt>(s)) {
        for (const VarDeclarator &d : decl->decls) {
          checker_.otherCaseVars_.erase(d.sym);
        }
      }
    }
  }
  checker_.popScope();
  checker_.flow_ = exit;
}

} // namespace tz
