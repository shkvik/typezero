#include "compiler/semantic/code/branches/code_branches_registry.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CodeBranchesRegistry::CodeBranchesRegistry(SemanticState &owner)
    : state(owner) {}

void CodeBranchesRegistry::walkIf(const WalkIf &task) {
  this->state.walk.tasks.pushTask(AfterIfCondition{.ifStmt = task.ifStmt});

  return this->state.walk.tasks.pushTask(CheckCondition{.cond = task.ifStmt->cond});
}

void CodeBranchesRegistry::afterIfCondition(const AfterIfCondition &task) {
  IfStmt *stmt = task.ifStmt;
  stmt->cond = this->state.walk.tasks.takeChecked();
  auto flows = this->state.walk.tasks.takeConditionFlows();
  if (flows.constant.has_value()) {
    return this->startConstantIf(stmt, *flows.constant, flows);
  }
  this->state.walk.flow.replaceFlow(flows.whenTrue);
  this->state.walk.tasks.pushTask(
      AfterThenBranch{.ifStmt = stmt, .whenFalse = std::move(flows.whenFalse)});

  return this->state.walk.tasks.pushTask(WalkBody{.body = stmt->thenStmt});
}

void CodeBranchesRegistry::startConstantIf(IfStmt *stmt, bool constant,
                                           const ConditionFlows &flows) {
  auto *literal = this->state.ast.make<BoolLitExpr>(stmt->cond->loc(), constant);
  literal->type = Type::boolTy();
  stmt->cond = literal;
  if (constant) {
    this->state.walk.flow.replaceFlow(flows.whenTrue);
    this->state.walk.tasks.pushTask(WalkBody{.body = stmt->thenStmt});
    stmt->elseStmt = nullptr;
    return;
  }
  stmt->thenStmt = this->state.ast.make<EmptyStmt>(stmt->thenStmt->loc());
  this->state.walk.flow.replaceFlow(flows.whenFalse);
  if (stmt->elseStmt != nullptr) {
    this->state.walk.tasks.pushTask(WalkBody{.body = stmt->elseStmt});
  }
}

void CodeBranchesRegistry::afterThenBranch(const AfterThenBranch &task) {
  auto afterThen = this->state.walk.flow.getFlow();
  this->state.walk.flow.replaceFlow(task.whenFalse);
  this->state.walk.tasks.pushTask(FinishIf{.afterThen = std::move(afterThen)});
  if (task.ifStmt->elseStmt != nullptr) {
    this->state.walk.tasks.pushTask(WalkBody{.body = task.ifStmt->elseStmt});
  }
}

void CodeBranchesRegistry::finishIf(const FinishIf &task) {
  const Flow &afterElse = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.afterThen, afterElse);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

void CodeBranchesRegistry::walkSwitch(const WalkSwitch &task) {
  this->state.walk.tasks.pushTask(AfterSwitchValue{.switchStmt = task.switchStmt});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &task.switchStmt->discriminant, .expected = nullptr});
}

void CodeBranchesRegistry::afterSwitchValue(const AfterSwitchValue &task) {
  SwitchStmt *stmt = task.switchStmt;
  auto comparable = this->checkValueType(stmt->discriminant->type);
  if (comparable.isErr()) {
    this->state.diagnostics.error(stmt->discriminant->loc(), comparable.error().message);
  }
  const Type *type = comparable.isOk() ? comparable.value() : Type::error();
  this->state.walk.scopes.openScope();
  auto all = this->caseStatements(stmt);
  this->state.walk.block.enterSwitchBlock(all);
  auto unreachable = Flow();
  unreachable.unreachable = true;
  auto draft = SwitchDraft{
      .switchStmt = stmt,
      .type = type,
      .hint = this->switchHint(type),
      .tests = this->state.walk.flow.getFlow(),
      .matched = std::vector<Flow>(stmt->cases.size()),
      .fallthrough = std::move(unreachable),
      .hasDefault = false,
      .index = 0,
      .position = 0,
  };
  this->state.walk.tasks.pushTask(NextCaseTest{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(DeclareBlockTypes{.stmts = std::move(all)});
}

void CodeBranchesRegistry::nextCaseTest(const NextCaseTest &task) {
  auto draft = task.draft;
  const auto &cases = draft.switchStmt->cases;
  while (draft.index < cases.size() && cases[draft.index].test == nullptr) {
    draft.index += 1;
  }
  if (draft.index == cases.size()) {
    return this->finishCaseTests(std::move(draft));
  }
  SwitchCase &switchCase = draft.switchStmt->cases[draft.index];
  const Type *hint = draft.hint;
  this->state.walk.flow.replaceFlow(draft.tests);
  this->state.walk.tasks.pushTask(AfterCaseTest{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &switchCase.test, .expected = hint});
}

void CodeBranchesRegistry::afterCaseTest(const AfterCaseTest &task) {
  SwitchStmt *stmt = task.draft.switchStmt;
  size_t index = task.draft.index;
  const auto &flow = this->state.walk.flow.getFlow();
  this->state.walk.tasks.pushConditionFlows(ConditionFlows{
      .whenTrue = flow,
      .whenFalse = flow,
      .constant = std::nullopt,
  });
  this->state.walk.tasks.pushTask(FinishCaseTest{.draft = task.draft});
  this->state.walk.tasks.pushTask(
      AddCaseFacts{.switchStmt = stmt, .index = index, .type = task.draft.type});

  return this->walkTypeOfCase(stmt, stmt->cases[index]);
}

void CodeBranchesRegistry::walkTypeOfCase(const SwitchStmt *stmt, const SwitchCase &switchCase) {
  auto *typeOf = dyn_cast<UnaryExpr>(stmt->discriminant);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf) {
    return;
  }
  auto *name = dyn_cast<StringLitExpr>(switchCase.test);
  if (name == nullptr) {
    return;
  }

  return this->state.walk.tasks.pushTask(WalkTypeOfName{.operand = typeOf->operand, .text = name});
}

void CodeBranchesRegistry::addCaseFacts(const AddCaseFacts &task) {
  SwitchStmt *stmt = task.switchStmt;
  SwitchCase &switchCase = stmt->cases[task.index];
  const Type *type = task.type;
  const Type *testType = switchCase.test->type;
  bool isGeneric = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
  bool isComparable =
      !isGeneric && !type->isError() && !testType->isError() && !testType->isUndefined();
  if (isComparable) {
    return this->addComparedFacts(stmt, switchCase, type);
  }
  if (!testType->isUndefined() || !isa<UndefinedExpr>(switchCase.test)) {
    return;
  }
  auto *equal = this->equalTest(switchCase.loc, stmt->discriminant, switchCase.test);

  return this->state.walk.tasks.pushTask(AddConditionFacts{.cond = equal});
}

void CodeBranchesRegistry::addComparedFacts(SwitchStmt *stmt, SwitchCase &switchCase,
                                            const Type *type) {
  auto compared = this->checkCaseType(type, switchCase.test->type);
  if (compared.isErr()) {
    return this->state.diagnostics.error(switchCase.test->loc(), compared.error().message);
  }
  const Type *common = compared.value();
  if (common->isError()) {
    return;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.inventory.comparesFunctions = true;
  }
  bool isConstant =
      this->isConstantTest(switchCase.test) && !this->isConstantTest(stmt->discriminant);
  switchCase.compareType = common;
  if (isConstant) {
    this->state.walk.tasks.pushTask(FinishConstantCase{.switchCase = &switchCase});
    this->state.walk.tasks.pushChecked(stmt->discriminant);
    this->state.walk.tasks.pushTask(Convert{.to = common});
  }

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &switchCase.test, .to = common});
}

void CodeBranchesRegistry::finishConstantCase(const FinishConstantCase &task) {
  Expr *discriminant = this->state.walk.tasks.takeChecked();
  const SwitchCase &switchCase = *task.switchCase;
  auto *equal = this->equalTest(switchCase.loc, discriminant, switchCase.test);

  return this->state.walk.tasks.pushTask(AddConditionFacts{.cond = equal});
}

BinaryExpr *CodeBranchesRegistry::equalTest(llvm::SMLoc loc, Expr *value, Expr *test) {
  auto *equal = this->state.ast.make<BinaryExpr>(loc, BinaryOp::Eq, value, test);
  equal->type = Type::boolTy();

  return equal;
}

void CodeBranchesRegistry::finishCaseTest(const FinishCaseTest &task) {
  auto draft = task.draft;
  auto flows = this->state.walk.tasks.takeConditionFlows();
  draft.matched[draft.index] = std::move(flows.whenTrue);
  draft.tests = std::move(flows.whenFalse);
  draft.index += 1;

  return this->nextCaseTest(NextCaseTest{.draft = std::move(draft)});
}

void CodeBranchesRegistry::finishCaseTests(SwitchDraft draft) {
  this->walkExhaustive(draft);
  auto labels = this->state.walk.block.takePendingLabels();
  auto breaks = Flow();
  breaks.unreachable = true;
  auto target = JumpTarget{
      .kind = JumpTarget::Kind::Switch,
      .labels = std::move(labels),
      .breaks = std::move(breaks),
  };
  this->state.walk.block.enterJumpTarget(std::move(target));
  draft.index = 0;

  return this->nextCaseBody(NextCaseBody{.draft = std::move(draft)});
}

void CodeBranchesRegistry::walkExhaustive(SwitchDraft &draft) {
  const SwitchStmt *stmt = draft.switchStmt;
  auto left = this->unhandledCases(stmt);
  if (!left.has_value()) {
    return;
  }
  auto handled = this->checkCasesHandled(stmt, *left);
  if (handled.isErr()) {
    return this->state.diagnostics.error(stmt->discriminant->loc(), handled.error().message);
  }
  draft.switchStmt->exhaustive = true;
  draft.tests.unreachable = true;
}

std::optional<std::vector<const Type *>>
CodeBranchesRegistry::unhandledCases(const SwitchStmt *stmt) const {
  const Type *type = stmt->discriminant->type;
  bool isChecked = type != nullptr && !type->isError() && !type->isOptional();
  if (!isChecked || !this->isLiteralSet(type)) {
    return std::nullopt;
  }
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  auto left = std::vector<const Type *>(members.begin(), members.end());
  for (const SwitchCase &switchCase : stmt->cases) {
    if (switchCase.test == nullptr) {
      return std::nullopt;
    }
    const Type *handled = this->caseLiteral(switchCase.test);
    if (handled != nullptr) {
      llvm::erase(left, handled);
    }
  }

  return left;
}

bool CodeBranchesRegistry::isLiteralSet(const Type *type) const {
  if (type->isLiteral()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral()) {
      return false;
    }
  }

  return true;
}

const Type *CodeBranchesRegistry::caseLiteral(const Expr *test) const {
  const Type *written = UnionCheckHelpers::writtenLiteral(test);
  if (written != nullptr) {
    return written;
  }
  if (test->type != nullptr && test->type->isLiteral()) {
    return test->type;
  }

  return nullptr;
}

void CodeBranchesRegistry::nextCaseBody(const NextCaseBody &task) {
  auto draft = task.draft;
  SwitchStmt *stmt = draft.switchStmt;
  if (draft.index == stmt->cases.size()) {
    return this->finishCaseBodies(draft);
  }
  const SwitchCase &switchCase = stmt->cases[draft.index];
  bool isDefault = switchCase.test == nullptr;
  const Flow &entry = isDefault ? draft.tests : draft.matched[draft.index];
  draft.hasDefault = draft.hasDefault || isDefault;
  auto start = this->state.flow.joins.join(draft.fallthrough, entry);
  this->state.walk.flow.replaceFlow(std::move(start));
  size_t position = draft.position;
  size_t count = switchCase.body.size();
  this->state.walk.tasks.pushTask(AfterCaseBody{.draft = std::move(draft)});
  for (size_t index = count; index > 0; --index) {
    this->state.walk.tasks.pushTask(WalkCaseStatement{.position = position + index - 1});
  }
}

void CodeBranchesRegistry::walkCaseStatement(const WalkCaseStatement &task) {
  auto all = this->state.walk.block.getSwitchBlock();
  this->state.walk.block.rememberBlockPosition(all, task.position);

  return this->state.walk.tasks.pushTask(WalkStmt{.stmt = all[task.position]});
}

void CodeBranchesRegistry::afterCaseBody(const AfterCaseBody &task) {
  auto draft = task.draft;
  const SwitchCase &switchCase = draft.switchStmt->cases[draft.index];
  draft.fallthrough = this->state.walk.flow.getFlow();
  this->rememberCaseVars(switchCase);
  draft.position += switchCase.body.size();
  draft.index += 1;

  return this->nextCaseBody(NextCaseBody{.draft = std::move(draft)});
}

void CodeBranchesRegistry::finishCaseBodies(const SwitchDraft &draft) {
  auto exit = draft.hasDefault ? draft.fallthrough
                               : this->state.flow.joins.join(draft.fallthrough, draft.tests);
  auto breaks = this->state.walk.block.leaveJumpTarget().breaks;
  auto joined = this->state.flow.joins.join(exit, breaks);
  this->forgetCaseVars(draft.switchStmt);
  this->state.walk.block.leaveSwitchBlock();
  this->state.walk.tasks.pushTask(FinishSwitch{.exit = std::move(joined)});

  return this->state.walk.tasks.pushTask(CloseScope{});
}

void CodeBranchesRegistry::finishSwitch(const FinishSwitch &task) {
  return this->state.walk.flow.replaceFlow(task.exit);
}

bool CodeBranchesRegistry::isConstantTest(const Expr *test) const {
  auto *unary = dyn_cast<UnaryExpr>(test);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : test;

  return isa<IntLitExpr>(value) || isa<FloatLitExpr>(value) || isa<StringLitExpr>(value) ||
         isa<BoolLitExpr>(value) || isa<UndefinedExpr>(value);
}

CodeBranchesRegistry::ResultValueComparable
CodeBranchesRegistry::checkValueType(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextValueType::isVoid();

    return err(CodeBranchesRegistryValueTypeError{
        .code = CodeBranchesRegistryValueTypeErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

CodeBranchesRegistry::ResultTestsComparable
CodeBranchesRegistry::checkCaseType(const Type *type, const Type *testType) const {
  const auto *common = this->state.types.relations.comparisonType(type, testType);
  bool hasResults = type->holdsResults() || testType->holdsResults();
  if (common == nullptr && hasResults) {
    auto message = ErrorTextCaseType::results(testType->name(), type->name());

    return err(CodeBranchesRegistryCaseTypeError{
        .code = CodeBranchesRegistryCaseTypeErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }
  if (common == nullptr) {
    auto message = ErrorTextCaseType::mismatch(testType->name(), type->name());

    return err(CodeBranchesRegistryCaseTypeError{
        .code = CodeBranchesRegistryCaseTypeErrorCode::NOT_COMPARABLE,
        .message = message,
    });
  }

  return ok(common);
}

CodeBranchesRegistry::ResultCasesAllHandled
CodeBranchesRegistry::checkCasesHandled(const SwitchStmt *stmt,
                                        llvm::ArrayRef<const Type *> left) const {
  if (left.empty()) {
    return ok(stmt);
  }

  auto names = this->caseNames(left);
  auto message = ErrorTextCasesLeft::unhandled(names, left.front()->name());

  return err(CodeBranchesRegistryCasesHandledError{
      .code = CodeBranchesRegistryCasesHandledErrorCode::CASES_LEFT,
      .message = message,
  });
}

std::string CodeBranchesRegistry::caseNames(llvm::ArrayRef<const Type *> left) const {
  auto names = std::string();
  for (size_t index = 0; index < left.size(); ++index) {
    names += this->nameSeparator(index, left.size());
    names += left[index]->name().str();
  }

  return names;
}

const char *CodeBranchesRegistry::nameSeparator(size_t index, size_t count) const {
  if (index == 0) {
    return "";
  }
  if (index + 1 == count) {
    return " and ";
  }

  return ", ";
}

std::vector<Stmt *> CodeBranchesRegistry::caseStatements(const SwitchStmt *stmt) const {
  auto all = std::vector<Stmt *>();
  for (const SwitchCase &switchCase : stmt->cases) {
    all.insert(all.end(), switchCase.body.begin(), switchCase.body.end());
  }

  return all;
}

const Type *CodeBranchesRegistry::switchHint(const Type *type) const {
  const Type *base = type->nonOptional();
  if (base->isNumeric() || base->isDecimal() || base->isJson()) {
    return base;
  }
  if (base->isLiteral() || base->isUnion()) {
    return type;
  }

  return nullptr;
}

void CodeBranchesRegistry::rememberCaseVars(const SwitchCase &switchCase) {
  for (Stmt *statement : switchCase.body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(statement);
    if (varDecl != nullptr) {
      this->rememberOtherCaseVars(varDecl);
    }
  }
}

void CodeBranchesRegistry::rememberOtherCaseVars(const VarDeclStmt *varDecl) {
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.sym != nullptr) {
      this->state.walk.block.addOtherCaseVar(declarator.sym);
    }
  }
}

void CodeBranchesRegistry::forgetCaseVars(const SwitchStmt *stmt) {
  for (const SwitchCase &switchCase : stmt->cases) {
    this->forgetCaseBodyVars(switchCase);
  }
}

void CodeBranchesRegistry::forgetCaseBodyVars(const SwitchCase &switchCase) {
  for (Stmt *statement : switchCase.body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(statement);
    if (varDecl != nullptr) {
      this->forgetOtherCaseVars(varDecl);
    }
  }
}

void CodeBranchesRegistry::forgetOtherCaseVars(const VarDeclStmt *varDecl) {
  for (const VarDeclarator &declarator : varDecl->decls) {
    this->state.walk.block.forgetOtherCaseVar(declarator.sym);
  }
}

}
