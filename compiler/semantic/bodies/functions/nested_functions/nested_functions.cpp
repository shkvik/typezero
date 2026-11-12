#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

NestedFunctions::NestedFunctions(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callCycles = &all.callCycles;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
}

void NestedFunctions::collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) const {
  for (const Expr *expr : AstWalker::exprsIn(stmt, true)) {
    const NameExpr *name = this->assignedName(expr);
    if (name != nullptr && !llvm::is_contained(names, name->name)) {
      names.push_back(name->name);
    }
  }
}

const NameExpr *NestedFunctions::assignedName(const Expr *expr) const {
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

NestedGroup *NestedFunctions::declareNested(BlockStmt *block) {
  auto members = this->nestedMembers(block);
  if (members.empty()) {
    return nullptr;
  }
  auto *group = this->state.getAst().newGroup();
  block->group = group;
  group->owner = this->state.getFunctionStack().getCurrentDecl();
  group->scopeDepth = this->state.getScopeStack().getScopeDepth();
  group->functionDepth = this->state.getFunctionStack().getFunctionDepth();
  auto names = llvm::StringSet<>();
  for (FunctionDecl *functionDecl : members) {
    this->declareMember(group, functionDecl);
    names.insert(functionDecl->name);
  }
  this->collectDeclaredNames(block, group);
  auto firstUse = this->firstUseOf(block, names);
  if (firstUse.has_value()) {
    group->firstUse = *firstUse;
  }
  this->callCycles->findCycles(group->members);

  return group;
}

std::vector<FunctionDecl *> NestedFunctions::nestedMembers(const BlockStmt *block) const {
  auto members = std::vector<FunctionDecl *>();
  for (Stmt *stmt : block->body) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr) {
      members.push_back(functionDecl);
    }
  }

  return members;
}

void NestedFunctions::declareMember(NestedGroup *group, FunctionDecl *functionDecl) {
  auto *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  functionDecl->group = group;
  functionDecl->parent = currentDecl;
  group->members.push_back(functionDecl);
  auto *variable = this->state.getAst().newVar();
  variable->name = functionDecl->name;
  variable->loc = functionDecl->loc();
  variable->isConst = true;
  variable->place = VarSymbol::localPlace(currentDecl, 0);
  variable->holds = DeclaredFunction{.function = functionDecl};
  functionDecl->var = variable;
  this->functionChecker->declareFunction(functionDecl, variable);
  this->collectAssigned(functionDecl->body, group->assignedNames);
}

void NestedFunctions::collectDeclaredNames(const BlockStmt *block, NestedGroup *group) const {
  for (Stmt *stmt : block->body) {
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr) {
      this->addDeclaredNames(varDecl, group);
    }
  }
}

void NestedFunctions::addDeclaredNames(const VarDeclStmt *varDecl, NestedGroup *group) const {
  for (const VarDeclarator &declarator : varDecl->decls) {
    group->declaredNames.push_back(declarator.name);
  }
}

std::optional<size_t> NestedFunctions::firstUseOf(const BlockStmt *block,
                                                  const llvm::StringSet<> &names) const {
  for (size_t index = 0; index < block->body.size(); ++index) {
    const Stmt *stmt = block->body[index];
    if (!isa<FunctionDecl>(stmt) && this->isUsingAny(stmt, names)) {
      return index;
    }
  }

  return std::nullopt;
}

bool NestedFunctions::isUsingAny(const Stmt *stmt, const llvm::StringSet<> &names) const {
  auto used = llvm::StringSet<>();
  AstWalker::collectNames(stmt, used);
  for (const auto &entry : used) {
    if (names.contains(entry.getKey())) {
      return true;
    }
  }

  return false;
}

void NestedFunctions::finishGroup(NestedGroup *group) {
  group->finished = true;
  for (FunctionDecl *functionDecl : group->members) {
    if (!functionDecl->sym->hasReturnType()) {
      this->returnInferrer->inferReturnType(functionDecl->sym);
    }
    if (!functionDecl->checked) {
      this->walkMemberBody(functionDecl);
    }
  }
  for (const NestedGroup::Use &use : group->uses) {
    this->walkUseOrder(group, use);
  }
}

void NestedFunctions::walkMemberBody(FunctionDecl *functionDecl) {
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->functionChecker->walkFunctionBody(functionDecl);
  this->returnInferrer->leaveBody(saved);
}

void NestedFunctions::walkUseOrder(const NestedGroup *group, const NestedGroup::Use &use) {
  auto ordered = this->useOrder(group, use);
  if (ordered.isErr()) {
    auto *later = this->capturedLater(group, use);
    auto declared = ErrorTextUseOrder::declared(later->name);
    this->state.getDiagnostics().error(use.loc, ordered.error().message);
    this->state.getDiagnostics().note(later->loc, declared);
  }
}

VarSymbol *NestedFunctions::capturedLater(const NestedGroup *group,
                                          const NestedGroup::Use &use) const {
  for (FunctionDecl *functionDecl : this->reachedFrom(use.fn)) {
    VarSymbol *later = this->laterCapture(functionDecl, group, use);
    if (later != nullptr) {
      return later;
    }
  }

  return nullptr;
}

std::vector<FunctionDecl *> NestedFunctions::reachedFrom(FunctionDecl *start) const {
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

VarSymbol *NestedFunctions::laterCapture(const FunctionDecl *functionDecl, const NestedGroup *group,
                                         const NestedGroup::Use &use) const {
  for (VarSymbol *variable : functionDecl->captures) {
    if (variable->owner() == group->owner && variable->declSeq() > use.seq) {
      return variable;
    }
  }

  return nullptr;
}

NestedFunctions::ResultUseInOrder NestedFunctions::useOrder(const NestedGroup *group,
                                                            const NestedGroup::Use &use) const {
  auto *later = this->capturedLater(group, use);
  if (later != nullptr) {
    auto message = ErrorTextUseOrder::early(use.fn->name, later->name);

    return err(NestedFunctionsUseOrderError{
        .code = NestedFunctionsUseOrderErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }

  return ok(&use);
}

void NestedFunctions::noteUse(VarSymbol *variable, llvm::SMLoc loc) {
  if (variable->local() == nullptr) {
    return;
  }
  this->rememberGroupUse(variable, loc);
  for (FunctionDecl *functionDecl : this->functionsInside(variable)) {
    if (this->isGroupMate(functionDecl, variable)) {
      this->linkSibling(functionDecl, variable->function());
      return;
    }
    this->captureVariable(functionDecl, variable);
  }
}

void NestedFunctions::rememberGroupUse(VarSymbol *variable, llvm::SMLoc loc) {
  FunctionDecl *member = variable->function();
  if (member == nullptr) {
    return;
  }
  auto *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  if (currentDecl == member->group->owner) {
    auto sequence = this->state.getReturnInference().getSequenceNumber();
    member->group->uses.push_back({member, sequence, loc});
  }
}

std::vector<FunctionDecl *> NestedFunctions::functionsInside(const VarSymbol *variable) const {
  auto functions = this->state.getFunctionStack().getFunctions();
  auto inside = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (functionDecl == variable->owner() || functionDecl == variable->lambda()) {
      break;
    }
    inside.push_back(functionDecl);
  }

  return inside;
}

bool NestedFunctions::isGroupMate(const FunctionDecl *functionDecl,
                                  const VarSymbol *variable) const {
  FunctionDecl *held = variable->function();

  return held != nullptr && functionDecl->group != nullptr && functionDecl->group == held->group;
}

void NestedFunctions::linkSibling(FunctionDecl *functionDecl, FunctionDecl *sibling) const {
  bool isLinked = llvm::is_contained(functionDecl->siblings, sibling);
  if (functionDecl != sibling && !isLinked) {
    functionDecl->siblings.push_back(sibling);
  }
}

void NestedFunctions::captureVariable(FunctionDecl *functionDecl, VarSymbol *variable) const {
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

FunctionDecl *NestedFunctions::namedFunction(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  if (name->func() != nullptr) {
    return name->func()->decl;
  }
  VarSymbol *variable = name->var();
  if (variable == nullptr) {
    return nullptr;
  }
  if (variable->function() != nullptr) {
    return variable->function();
  }
  if (variable->isConst) {
    return variable->lambda();
  }

  return nullptr;
}

}
