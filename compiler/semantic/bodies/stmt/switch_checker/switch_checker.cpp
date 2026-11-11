#include "compiler/semantic/bodies/stmt/switch_checker/switch_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/stmt/jump_checker/jump_checker.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_checker/union_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

SwitchChecker::SwitchChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->exprChecker = &all.exprChecker;
  this->jumpChecker = &all.jumpChecker;
  this->stmtChecker = &all.stmtChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->unionChecker = &all.unionChecker;
  this->binaryChecker = &all.binaryChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
}

bool SwitchChecker::isConstantTest(const Expr *test) {
  if (auto *unary = dyn_cast<UnaryExpr>(test); unary && unary->op == UnaryOp::Neg) {
    test = unary->operand;
  }

  return isa<IntLitExpr>(test) || isa<FloatLitExpr>(test) || isa<StringLitExpr>(test) ||
         isa<BoolLitExpr>(test) || isa<UndefinedExpr>(test);
}

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
  stmt->discriminant = this->exprChecker->checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  ResultValueComparable comparable = SwitchChecker::valueType(type);
  if (comparable.isErr()) {
    this->state.getDiagnostics().error(stmt->discriminant->loc(), comparable.error().message);
    type = Type::error();
  }
  this->state.getScopeStack().openScope();
  std::vector<Stmt *> all = SwitchChecker::caseStatements(stmt);
  this->typeDeclarer->declareTypes(all);
  SwitchFlows flows{
      .type = type,
      .hint = SwitchChecker::switchHint(type),
      .tests = this->state.getFlowState().getFlow(),
      .matched = std::vector<Flow>(stmt->cases.size()),
  };
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    this->walkCaseTest(stmt, i, flows);
  }
  this->walkExhaustive(stmt, flows);
  Flow exit = this->caseBodiesExit(stmt, flows, all);
  this->forgetCaseVars(stmt);
  this->scopes->closeScope();
  this->state.getFlowState().replaceFlow(exit);
}

std::vector<Stmt *> SwitchChecker::caseStatements(const SwitchStmt *stmt) {
  std::vector<Stmt *> all;
  for (const SwitchCase &c : stmt->cases) {
    all.insert(all.end(), c.body.begin(), c.body.end());
  }

  return all;
}

const Type *SwitchChecker::switchHint(const Type *type) {
  const Type *base = type->nonOptional();
  if (base->isNumeric() || base->isDecimal() || base->isJson()) {
    return base;
  }
  if (base->isLiteral() || base->isUnion()) {
    return type;
  }

  return nullptr;
}

void SwitchChecker::walkCaseTest(SwitchStmt *stmt, size_t i, SwitchFlows &flows) {
  SwitchCase &c = stmt->cases[i];
  if (!c.test) {
    return;
  }
  this->state.getFlowState().replaceFlow(flows.tests);
  c.test = this->exprChecker->checkExpr(c.test, flows.hint);
  this->walkTypeOfCase(stmt, c);
  Flow whenTrue = this->state.getFlowState().getFlow();
  Flow whenFalse = this->state.getFlowState().getFlow();
  this->addCaseFacts(stmt, c, flows.type, whenTrue, whenFalse);
  flows.matched[i] = whenTrue;
  flows.tests = whenFalse;
}

void SwitchChecker::walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &c) {
  auto *typeOf = dyn_cast<UnaryExpr>(stmt->discriminant);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf) {
    return;
  }
  auto *name = dyn_cast<StringLitExpr>(c.test);
  if (name == nullptr) {
    return;
  }
  BinaryChecker::ResultTypeOfNameKnown known =
      this->binaryChecker->checkTypeOfName(typeOf->operand, name);
  if (known.isErr() &&
      known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(c.test->loc(), known.error().message);
  }
}

void SwitchChecker::addCaseFacts(SwitchStmt *stmt, SwitchCase &c, const Type *type, Flow &whenTrue,
                                 Flow &whenFalse) {
  const Type *testType = c.test->type;
  bool generic = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
  bool comparable =
      !generic && !type->isError() && !testType->isError() && !testType->isUndefined();
  if (comparable) {
    return this->addComparedFacts(stmt, c, type, whenTrue, whenFalse);
  }
  if (!testType->isUndefined() || !isa<UndefinedExpr>(c.test)) {
    return;
  }
  BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
  equal.type = Type::boolTy();
  this->conditionFacts->addConditionFacts(&equal, whenTrue, whenFalse);
}

void SwitchChecker::addComparedFacts(SwitchStmt *stmt, SwitchCase &c, const Type *type,
                                     Flow &whenTrue, Flow &whenFalse) {
  ResultTestsComparable compared = SwitchChecker::caseType(type, c.test->type);
  if (compared.isErr()) {
    this->state.getDiagnostics().error(c.test->loc(), compared.error().message);
    return;
  }
  const Type *common = compared.value();
  if (common->isError()) {
    return;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  bool constant =
      SwitchChecker::isConstantTest(c.test) && !SwitchChecker::isConstantTest(stmt->discriminant);
  c.test = this->typeRelations->convert(c.test, common);
  c.compareType = common;
  if (!constant) {
    return;
  }
  Expr *discriminant = this->typeRelations->convert(stmt->discriminant, common);
  BinaryExpr equal(c.loc, BinaryOp::Eq, discriminant, c.test);
  equal.type = Type::boolTy();
  this->conditionFacts->addConditionFacts(&equal, whenTrue, whenFalse);
}

void SwitchChecker::walkExhaustive(SwitchStmt *stmt, SwitchFlows &flows) {
  std::optional<std::vector<const Type *>> left = this->unionChecker->unhandledCases(stmt);
  if (!left.has_value()) {
    return;
  }
  ResultCasesAllHandled handled = SwitchChecker::casesHandled(stmt, *left);
  if (handled.isErr()) {
    this->state.getDiagnostics().error(stmt->discriminant->loc(), handled.error().message);
    return;
  }
  stmt->exhaustive = true;
  flows.tests.unreachable = true;
}

Flow SwitchChecker::caseBodiesExit(SwitchStmt *stmt, const SwitchFlows &flows,
                                   const std::vector<Stmt *> &all) {
  this->jumpChecker->pushTarget(JumpTarget::Kind::Switch);
  size_t position = 0;
  Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    const Flow &entry = c.test ? flows.matched[i] : flows.tests;
    this->state.getFlowState().replaceFlow(FlowChecker::join(fallthrough, entry));
    for (Stmt *s : c.body) {
      this->state.getBlockPosition().rememberBlockPosition(all, position++);
      this->stmtChecker->checkStmt(s);
    }
    fallthrough = this->state.getFlowState().getFlow();
    this->rememberCaseVars(c);
  }
  Flow exit = fallthrough;
  if (!hasDefault) {
    exit = FlowChecker::join(exit, flows.tests);
  }

  return FlowChecker::join(exit, this->state.getBlockPosition().leaveJumpTarget().breaks);
}

void SwitchChecker::rememberCaseVars(const SwitchCase &c) {
  for (Stmt *s : c.body) {
    auto *decl = dyn_cast<VarDeclStmt>(s);
    if (decl == nullptr) {
      continue;
    }
    for (const VarDeclarator &d : decl->decls) {
      if (d.sym != nullptr) {
        this->state.getBlockPosition().addOtherCaseVar(d.sym);
      }
    }
  }
}

void SwitchChecker::forgetCaseVars(const SwitchStmt *stmt) {
  for (const SwitchCase &c : stmt->cases) {
    for (Stmt *s : c.body) {
      auto *decl = dyn_cast<VarDeclStmt>(s);
      if (decl == nullptr) {
        continue;
      }
      for (const VarDeclarator &d : decl->decls) {
        this->state.getBlockPosition().forgetOtherCaseVar(d.sym);
      }
    }
  }
}

}
