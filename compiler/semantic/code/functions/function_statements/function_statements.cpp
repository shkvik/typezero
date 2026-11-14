#include "compiler/semantic/code/functions/function_statements/function_statements.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FunctionStatements::FunctionStatements(SemanticState &owner)
    : state(owner) {}

void FunctionStatements::walkStatements(const WalkStatements &task) {
  BlockStmt *block = task.block;
  this->state.walk.tasks.pushTask(AfterBlockTypes{.block = block});

  return this->state.walk.tasks.pushTask(DeclareBlockTypes{.stmts = block->body});
}

void FunctionStatements::afterBlockTypes(const AfterBlockTypes &task) {
  BlockStmt *block = task.block;
  auto members = this->nestedMembers(block);
  auto &tasks = this->state.walk.tasks;
  if (members.empty()) {
    return this->enterStatements(EnterStatements{.block = block, .group = nullptr});
  }
  auto *group = this->state.ast.newGroup();
  block->group = group;
  group->owner = this->state.walk.functions.getCurrentDecl();
  group->scopeDepth = this->state.walk.scopes.getScopeDepth();
  group->functionDepth = this->state.walk.functions.getFunctionDepth();
  auto names = llvm::StringSet<>();
  for (FunctionDecl *functionDecl : members) {
    this->prepareMember(group, functionDecl);
    names.insert(functionDecl->name);
  }
  this->collectDeclaredNames(block, group);
  auto firstUse = this->firstUseOf(block, names);
  if (firstUse.has_value()) {
    group->firstUse = *firstUse;
  }
  tasks.pushTask(EnterStatements{.block = block, .group = group});
  tasks.pushTask(FindCallCycles{.functions = group->members});
  for (FunctionDecl *functionDecl : llvm::reverse(members)) {
    tasks.pushTask(StartFunctionSymbol{
        .functionDecl = functionDecl,
        .variable = functionDecl->var,
        .isNamed = true,
    });
  }
}

void FunctionStatements::prepareMember(NestedGroup *group, FunctionDecl *functionDecl) {
  auto *currentDecl = this->state.walk.functions.getCurrentDecl();
  functionDecl->group = group;
  functionDecl->parent = currentDecl;
  group->members.push_back(functionDecl);
  auto *variable = this->state.ast.newVar();
  variable->name = functionDecl->name;
  variable->loc = functionDecl->loc();
  variable->isConst = true;
  variable->place = VarSymbol::localPlace(currentDecl, 0);
  variable->holds = DeclaredFunction{.function = functionDecl};
  functionDecl->var = variable;

  return this->collectAssigned(functionDecl->body, group->assignedNames);
}

void FunctionStatements::enterStatements(const EnterStatements &task) {
  BlockStmt *block = task.block;
  if (task.group != nullptr) {
    this->state.walk.functions.enterGroup(task.group);
  }
  auto names = this->blockNames(block);
  this->state.walk.scopes.enterBlockNames(std::move(names));
  auto draft = StatementsDraft{
      .block = block,
      .group = task.group,
      .index = 0,
  };

  return this->nextStatement(NextStatement{.draft = draft});
}

void FunctionStatements::nextStatement(const NextStatement &task) {
  const StatementsDraft &draft = task.draft;
  BlockStmt *block = draft.block;
  if (draft.index == block->body.size()) {
    return this->finishStatements(draft);
  }
  if (draft.group != nullptr && draft.index == draft.group->firstUse) {
    this->markGroupCreated(draft.group);
  }
  this->state.walk.block.rememberBlockPosition(block->body, draft.index);
  this->state.walk.tasks.pushTask(AfterStatement{.draft = draft});

  return this->state.walk.tasks.pushTask(WalkStmt{.stmt = block->body[draft.index]});
}

void FunctionStatements::afterStatement(const AfterStatement &task) {
  auto draft = task.draft;
  BlockStmt *block = draft.block;
  if (this->isDecidedEnd(block, draft.index, draft.group)) {
    block->body.resize(draft.index + 1);
    return this->finishStatements(draft);
  }
  draft.index += 1;

  return this->nextStatement(NextStatement{.draft = draft});
}

void FunctionStatements::finishStatements(const StatementsDraft &draft) {
  this->state.walk.scopes.leaveBlockNames();
  NestedGroup *group = draft.group;
  if (group == nullptr) {
    return;
  }
  group->finished = true;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(LeaveGroup{});
  tasks.pushTask(GroupUseOrder{.group = group});
  for (FunctionDecl *functionDecl : llvm::reverse(group->members)) {
    tasks.pushTask(GroupMember{.functionDecl = functionDecl});
  }
}

void FunctionStatements::groupMember(const GroupMember &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->state.walk.tasks.pushTask(AfterGroupMemberReturn{.functionDecl = functionDecl});
  if (!functionDecl->sym->hasReturnType()) {
    return this->state.walk.tasks.pushTask(InferReturnType{.function = functionDecl->sym});
  }
}

void FunctionStatements::afterGroupMemberReturn(const AfterGroupMemberReturn &task) {
  if (!task.functionDecl->checked) {
    return this->state.walk.tasks.pushTask(WalkInferredBody{.functionDecl = task.functionDecl});
  }
}

void FunctionStatements::groupUseOrder(const GroupUseOrder &task) {
  for (const NestedGroup::Use &use : task.group->uses) {
    this->walkUseOrder(task.group, use);
  }
}

void FunctionStatements::leaveGroup() {
  return this->state.walk.functions.leaveGroup();
}

void FunctionStatements::noteUse(const NoteUse &task) {
  VarSymbol *variable = task.variable;
  if (variable->local() == nullptr) {
    return;
  }
  this->rememberGroupUse(variable, task.loc);
  for (FunctionDecl *functionDecl : this->functionsInside(variable)) {
    if (this->isGroupMate(functionDecl, variable)) {
      this->linkSibling(functionDecl, variable->function());
      return;
    }
    this->captureVariable(functionDecl, variable);
  }
}

BlockNames FunctionStatements::blockNames(const BlockStmt *block) const {
  auto names = BlockNames{this->state.walk.functions.getFunctionDepth(), {}};
  for (Stmt *stmt : block->body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr) {
      this->addDeclaratorNames(varDecl, names);
    }
  }

  return names;
}

void FunctionStatements::addDeclaratorNames(const VarDeclStmt *varDecl, BlockNames &names) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    names.names.push_back(declarator.name);
  }
}

void FunctionStatements::markGroupCreated(NestedGroup *group) {
  group->creationSeq = this->state.declaration.order.takeSequenceNumber();
  for (FunctionDecl *functionDecl : group->members) {
    LocalVarState *localState = functionDecl->var->local();
    if (localState != nullptr) {
      localState->declSeq = group->creationSeq;
    }
  }
}

bool FunctionStatements::isDecidedEnd(const BlockStmt *block, size_t index,
                                      const NestedGroup *group) const {
  auto *decided = dyn_cast<IfStmt>(block->body[index]);
  bool isConstantIf = decided != nullptr && isa<BoolLitExpr>(decided->cond);
  bool isGroupCreated = group == nullptr || group->firstUse <= index;
  bool isEndReached = this->state.generics.copies.isCheckingInstance() && isConstantIf &&
                      this->state.walk.flow.isUnreachable() && isGroupCreated;
  if (!isEndReached) {
    return false;
  }
  auto rest = llvm::ArrayRef<Stmt *>(block->body).drop_front(index + 1);

  return llvm::none_of(rest, std::bind_front(&FunctionStatements::isDeclarationStmt, this));
}

bool FunctionStatements::isDeclarationStmt(const Stmt *stmt) const {
  return isa<FunctionDecl>(stmt) || isa<ClassDecl>(stmt);
}

void FunctionStatements::collectAssigned(const Stmt *stmt,
                                         std::vector<llvm::StringRef> &names) const {
  for (const Expr *expr : AstWalker::allExprsIn(stmt)) {
    const NameExpr *name = this->assignedName(expr);
    if (name != nullptr && !llvm::is_contained(names, name->name)) {
      names.push_back(name->name);
    }
  }
}

const NameExpr *FunctionStatements::assignedName(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  auto *update = dyn_cast<UpdateExpr>(expr);

  if (assign != nullptr) {
    return llvm::dyn_cast_or_null<NameExpr>(assign->target);
  }
  if (update != nullptr) {
    return llvm::dyn_cast_or_null<NameExpr>(update->target);
  }

  return nullptr;
}

std::vector<FunctionDecl *> FunctionStatements::nestedMembers(const BlockStmt *block) const {
  auto members = std::vector<FunctionDecl *>();
  for (Stmt *stmt : block->body) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr) {
      members.push_back(functionDecl);
    }
  }

  return members;
}

void FunctionStatements::collectDeclaredNames(const BlockStmt *block, NestedGroup *group) const {
  for (Stmt *stmt : block->body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr) {
      this->addDeclaredNames(varDecl, group);
    }
  }
}

void FunctionStatements::addDeclaredNames(const VarDeclStmt *varDecl, NestedGroup *group) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    group->declaredNames.push_back(declarator.name);
  }
}

std::optional<size_t> FunctionStatements::firstUseOf(const BlockStmt *block,
                                                     const llvm::StringSet<> &names) const {
  for (size_t index = 0; index < block->body.size(); ++index) {
    const Stmt *stmt = block->body[index];
    if (!isa<FunctionDecl>(stmt) && this->isUsingAny(stmt, names)) {
      return index;
    }
  }

  return std::nullopt;
}

bool FunctionStatements::isUsingAny(const Stmt *stmt, const llvm::StringSet<> &names) const {
  auto used = llvm::StringSet<>();
  AstWalker::collectNames(stmt, used);
  for (const auto &entry : used) {
    if (names.contains(entry.getKey())) {
      return true;
    }
  }

  return false;
}

void FunctionStatements::walkUseOrder(const NestedGroup *group, const NestedGroup::Use &use) {
  auto ordered = this->checkUseOrder(group, use);
  if (ordered.isErr()) {
    auto *later = this->capturedLater(group, use);
    auto declared = ErrorTextUseOrder::declared(later->name);
    this->state.diagnostics.error(use.loc, ordered.error().message);
    this->state.diagnostics.note(later->loc, declared);
  }
}

VarSymbol *FunctionStatements::capturedLater(const NestedGroup *group,
                                             const NestedGroup::Use &use) const {
  for (FunctionDecl *functionDecl : this->reachedFrom(use.fn)) {
    VarSymbol *later = this->laterCapture(functionDecl, group, use);
    if (later != nullptr) {
      return later;
    }
  }

  return nullptr;
}

std::vector<FunctionDecl *> FunctionStatements::reachedFrom(FunctionDecl *start) const {
  auto reached = std::vector<FunctionDecl *>{start};
  for (size_t index = 0; index < reached.size(); ++index) {
    for (FunctionDecl *sibling : reached[index]->siblings) {
      if (!llvm::is_contained(reached, sibling)) {
        reached.push_back(sibling);
      }
    }
  }

  return reached;
}

VarSymbol *FunctionStatements::laterCapture(const FunctionDecl *functionDecl,
                                            const NestedGroup *group,
                                            const NestedGroup::Use &use) const {
  for (VarSymbol *variable : functionDecl->captures) {
    if (variable->owner() == group->owner && variable->declSeq() > use.seq) {
      return variable;
    }
  }

  return nullptr;
}

FunctionStatements::ResultUseInOrder
FunctionStatements::checkUseOrder(const NestedGroup *group, const NestedGroup::Use &use) const {
  auto *later = this->capturedLater(group, use);
  if (later != nullptr) {
    auto message = ErrorTextUseOrder::early(use.fn->name, later->name);

    return err(CodeFunctionsRegistryUseOrderError{
        .code = CodeFunctionsRegistryUseOrderErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }

  return ok(&use);
}

void FunctionStatements::rememberGroupUse(VarSymbol *variable, llvm::SMLoc loc) {
  FunctionDecl *member = variable->function();
  if (member == nullptr) {
    return;
  }
  auto *currentDecl = this->state.walk.functions.getCurrentDecl();
  if (currentDecl == member->group->owner) {
    auto sequence = this->state.declaration.order.getSequenceNumber();
    member->group->uses.push_back({member, sequence, loc});
  }
}

std::vector<FunctionDecl *> FunctionStatements::functionsInside(const VarSymbol *variable) const {
  auto functions = this->state.walk.functions.getFunctions();
  auto inside = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (functionDecl == variable->owner() || functionDecl == variable->lambda()) {
      break;
    }
    inside.push_back(functionDecl);
  }

  return inside;
}

bool FunctionStatements::isGroupMate(const FunctionDecl *functionDecl,
                                     const VarSymbol *variable) const {
  FunctionDecl *held = variable->function();

  return held != nullptr && functionDecl->group != nullptr && functionDecl->group == held->group;
}

void FunctionStatements::linkSibling(FunctionDecl *functionDecl, FunctionDecl *sibling) const {
  bool isLinked = llvm::is_contained(functionDecl->siblings, sibling);
  if (functionDecl != sibling && !isLinked) {
    functionDecl->siblings.push_back(sibling);
  }
}

void FunctionStatements::captureVariable(FunctionDecl *functionDecl, VarSymbol *variable) const {
  NestedGroup *group = functionDecl->group;
  if (!llvm::is_contained(functionDecl->captures, variable)) {
    functionDecl->captures.push_back(variable);
  }
  if (group != nullptr && !llvm::is_contained(group->captures, variable)) {
    group->captures.push_back(variable);
  }
  LocalVarState *localState = variable->local();
  unsigned created = group != nullptr ? group->creationSeq : functionDecl->creationSeq;
  localState->captured = true;
  if (created <= localState->declSeq) {
    localState->capturedEarly = true;
  }
}

}
