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
  this->flowChecker = &all.flowChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->unionChecker = &all.unionChecker;
  this->binaryChecker = &all.binaryChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
}

bool SwitchChecker::isConstantTest(const Expr *test) const {
  auto *unary = dyn_cast<UnaryExpr>(test);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : test;

  return isa<IntLitExpr>(value) || isa<FloatLitExpr>(value) || isa<StringLitExpr>(value) ||
         isa<BoolLitExpr>(value) || isa<UndefinedExpr>(value);
}

SwitchChecker::ResultValueComparable SwitchChecker::checkValueType(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextValueType::isVoid();

    return err(SwitchCheckerValueTypeError{
        .code = SwitchCheckerValueTypeErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

SwitchChecker::ResultTestsComparable SwitchChecker::checkCaseType(const Type *type,
                                                                  const Type *testType) const {
  const auto *common = this->typeRelations->comparisonType(type, testType);
  bool hasResults = type->holdsResults() || testType->holdsResults();
  if (common == nullptr && hasResults) {
    auto message = ErrorTextCaseType::results(testType->name(), type->name());

    return err(SwitchCheckerCaseTypeError{
        .code = SwitchCheckerCaseTypeErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }
  if (common == nullptr) {
    auto message = ErrorTextCaseType::mismatch(testType->name(), type->name());

    return err(SwitchCheckerCaseTypeError{
        .code = SwitchCheckerCaseTypeErrorCode::NOT_COMPARABLE,
        .message = message,
    });
  }

  return ok(common);
}

SwitchChecker::ResultCasesAllHandled
SwitchChecker::checkCasesHandled(const SwitchStmt *stmt, llvm::ArrayRef<const Type *> left) const {
  if (left.empty()) {
    return ok(stmt);
  }

  auto names = this->caseNames(left);
  auto message = ErrorTextCasesLeft::unhandled(names, left.front()->name());

  return err(SwitchCheckerCasesHandledError{
      .code = SwitchCheckerCasesHandledErrorCode::CASES_LEFT,
      .message = message,
  });
}

std::string SwitchChecker::caseNames(llvm::ArrayRef<const Type *> left) const {
  auto names = std::string();
  for (size_t index = 0; index < left.size(); ++index) {
    names += this->nameSeparator(index, left.size());
    names += left[index]->name().str();
  }

  return names;
}

const char *SwitchChecker::nameSeparator(size_t index, size_t count) const {
  if (index == 0) {
    return "";
  }
  if (index + 1 == count) {
    return " and ";
  }

  return ", ";
}

void SwitchChecker::walkSwitch(SwitchStmt *stmt) {
  stmt->discriminant = this->exprChecker->checkExpr(stmt->discriminant);
  auto comparable = this->checkValueType(stmt->discriminant->type);
  if (comparable.isErr()) {
    this->state.getDiagnostics().error(stmt->discriminant->loc(), comparable.error().message);
  }
  const Type *type = comparable.isOk() ? comparable.value() : Type::error();
  this->state.getScopeStack().openScope();
  auto all = this->caseStatements(stmt);
  this->typeDeclarer->declareTypes(all);
  auto flows = SwitchFlows{
      .type = type,
      .hint = this->switchHint(type),
      .tests = this->state.getFlowState().getFlow(),
      .matched = std::vector<Flow>(stmt->cases.size()),
  };
  for (size_t index = 0; index < stmt->cases.size(); ++index) {
    this->walkCaseTest(stmt, index, flows);
  }
  this->walkExhaustive(stmt, flows);
  auto exit = this->caseBodiesExit(stmt, flows, all);
  this->forgetCaseVars(stmt);
  this->scopes->closeScope();
  this->state.getFlowState().replaceFlow(exit);
}

std::vector<Stmt *> SwitchChecker::caseStatements(const SwitchStmt *stmt) const {
  auto all = std::vector<Stmt *>();
  for (const SwitchCase &switchCase : stmt->cases) {
    all.insert(all.end(), switchCase.body.begin(), switchCase.body.end());
  }

  return all;
}

const Type *SwitchChecker::switchHint(const Type *type) const {
  const Type *base = type->nonOptional();
  if (base->isNumeric() || base->isDecimal() || base->isJson()) {
    return base;
  }
  if (base->isLiteral() || base->isUnion()) {
    return type;
  }

  return nullptr;
}

void SwitchChecker::walkCaseTest(SwitchStmt *stmt, size_t index, SwitchFlows &flows) {
  SwitchCase &switchCase = stmt->cases[index];
  if (switchCase.test == nullptr) {
    return;
  }
  this->state.getFlowState().replaceFlow(flows.tests);
  switchCase.test = this->exprChecker->checkExpr(switchCase.test, flows.hint);
  this->walkTypeOfCase(stmt, switchCase);
  auto whenTrue = this->state.getFlowState().getFlow();
  auto whenFalse = this->state.getFlowState().getFlow();
  this->addCaseFacts(stmt, switchCase, flows.type, whenTrue, whenFalse);
  flows.matched[index] = whenTrue;
  flows.tests = whenFalse;
}

void SwitchChecker::walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &switchCase) {
  auto *typeOf = dyn_cast<UnaryExpr>(stmt->discriminant);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf) {
    return;
  }
  auto *name = dyn_cast<StringLitExpr>(switchCase.test);
  if (name == nullptr) {
    return;
  }
  auto known = this->binaryChecker->checkTypeOfName(typeOf->operand, name);
  if (known.isErr() &&
      known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(switchCase.test->loc(), known.error().message);
  }
}

void SwitchChecker::addCaseFacts(SwitchStmt *stmt, SwitchCase &switchCase, const Type *type,
                                 Flow &whenTrue, Flow &whenFalse) {
  const Type *testType = switchCase.test->type;
  bool isGeneric = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
  bool isComparable =
      !isGeneric && !type->isError() && !testType->isError() && !testType->isUndefined();
  if (isComparable) {
    return this->addComparedFacts(stmt, switchCase, type, whenTrue, whenFalse);
  }
  if (!testType->isUndefined() || !isa<UndefinedExpr>(switchCase.test)) {
    return;
  }
  BinaryExpr equal(switchCase.loc, BinaryOp::Eq, stmt->discriminant, switchCase.test);
  equal.type = Type::boolTy();
  this->conditionFacts->addConditionFacts(&equal, whenTrue, whenFalse);
}

void SwitchChecker::addComparedFacts(SwitchStmt *stmt, SwitchCase &switchCase, const Type *type,
                                     Flow &whenTrue, Flow &whenFalse) {
  auto compared = this->checkCaseType(type, switchCase.test->type);
  if (compared.isErr()) {
    this->state.getDiagnostics().error(switchCase.test->loc(), compared.error().message);
    return;
  }
  const Type *common = compared.value();
  if (common->isError()) {
    return;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  bool isConstant =
      this->isConstantTest(switchCase.test) && !this->isConstantTest(stmt->discriminant);
  switchCase.test = this->typeRelations->convert(switchCase.test, common);
  switchCase.compareType = common;
  if (!isConstant) {
    return;
  }
  auto *discriminant = this->typeRelations->convert(stmt->discriminant, common);
  BinaryExpr equal(switchCase.loc, BinaryOp::Eq, discriminant, switchCase.test);
  equal.type = Type::boolTy();
  this->conditionFacts->addConditionFacts(&equal, whenTrue, whenFalse);
}

void SwitchChecker::walkExhaustive(SwitchStmt *stmt, SwitchFlows &flows) {
  auto left = this->unionChecker->unhandledCases(stmt);
  if (!left.has_value()) {
    return;
  }
  auto handled = this->checkCasesHandled(stmt, *left);
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
  auto fallthrough = Flow();
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t index = 0; index < stmt->cases.size(); ++index) {
    SwitchCase &switchCase = stmt->cases[index];
    bool isDefault = switchCase.test == nullptr;
    const Flow &entry = isDefault ? flows.tests : flows.matched[index];
    hasDefault = hasDefault || isDefault;
    auto start = this->flowChecker->join(fallthrough, entry);
    this->state.getFlowState().replaceFlow(start);
    position = this->walkCaseBody(switchCase, all, position);
    fallthrough = this->state.getFlowState().getFlow();
    this->rememberCaseVars(switchCase);
  }
  auto exit = hasDefault ? fallthrough : this->flowChecker->join(fallthrough, flows.tests);
  auto breaks = this->state.getBlockPosition().leaveJumpTarget().breaks;

  return this->flowChecker->join(exit, breaks);
}

size_t SwitchChecker::walkCaseBody(const SwitchCase &switchCase, const std::vector<Stmt *> &all,
                                   size_t position) {
  size_t next = position;
  for (Stmt *statement : switchCase.body) {
    this->state.getBlockPosition().rememberBlockPosition(all, next);
    next += 1;
    this->stmtChecker->walkStmt(statement);
  }

  return next;
}

void SwitchChecker::rememberCaseVars(const SwitchCase &switchCase) {
  for (Stmt *statement : switchCase.body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(statement);
    if (varDecl != nullptr) {
      this->rememberOtherCaseVars(varDecl);
    }
  }
}

void SwitchChecker::rememberOtherCaseVars(const VarDeclStmt *varDecl) {
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.sym != nullptr) {
      this->state.getBlockPosition().addOtherCaseVar(declarator.sym);
    }
  }
}

void SwitchChecker::forgetCaseVars(const SwitchStmt *stmt) {
  for (const SwitchCase &switchCase : stmt->cases) {
    this->forgetCaseBodyVars(switchCase);
  }
}

void SwitchChecker::forgetCaseBodyVars(const SwitchCase &switchCase) {
  for (Stmt *statement : switchCase.body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(statement);
    if (varDecl != nullptr) {
      this->forgetOtherCaseVars(varDecl);
    }
  }
}

void SwitchChecker::forgetOtherCaseVars(const VarDeclStmt *varDecl) {
  for (const VarDeclarator &declarator : varDecl->decls) {
    this->state.getBlockPosition().forgetOtherCaseVar(declarator.sym);
  }
}

}
