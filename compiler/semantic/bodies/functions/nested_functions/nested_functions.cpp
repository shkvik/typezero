#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

NestedFunctions::NestedFunctions(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callCycles = &all.callCycles;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
}

void NestedFunctions::collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) {
  for (const Expr *e : AstWalker::exprsIn(stmt, true)) {
    const Expr *target = nullptr;
    if (auto *assign = dyn_cast<AssignExpr>(e)) {
      target = assign->target;
    } else if (auto *update = dyn_cast<UpdateExpr>(e)) {
      target = update->target;
    }
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(target)) {
      if (!llvm::is_contained(names, name->name)) {
        names.push_back(name->name);
      }
    }
  }
}

NestedGroup *NestedFunctions::declareNested(BlockStmt *block) {
  std::vector<FunctionDecl *> members;
  for (Stmt *stmt : block->body) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
      members.push_back(fn);
    }
  }
  if (members.empty()) {
    return nullptr;
  }
  NestedGroup *group = this->state.getAst().newGroup();
  block->group = group;
  group->owner = this->state.getFunctionStack().getCurrentDecl();
  group->scopeDepth = this->state.getScopeStack().getScopeDepth();
  group->functionDepth = this->state.getFunctionStack().getFunctionDepth();
  llvm::StringSet<> names;
  for (FunctionDecl *fn : members) {
    fn->group = group;
    fn->parent = this->state.getFunctionStack().getCurrentDecl();
    group->members.push_back(fn);
    names.insert(fn->name);
    VarSymbol *var = this->state.getAst().newVar();
    var->name = fn->name;
    var->loc = fn->loc();
    var->isConst = true;
    var->place = VarSymbol::localPlace(this->state.getFunctionStack().getCurrentDecl(), 0);
    var->holds = DeclaredFunction{.function = fn};
    fn->var = var;
    this->functionChecker->declareFunction(fn, var);
    NestedFunctions::collectAssigned(fn->body, group->assignedNames);
  }
  for (Stmt *stmt : block->body) {
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt)) {
      for (const VarDeclarator &d : decl->decls) {
        group->declaredNames.push_back(d.name);
      }
    }
  }
  for (size_t i = 0; i < block->body.size() && group->firstUse == ~size_t(0); ++i) {
    if (isa<FunctionDecl>(block->body[i])) {
      continue;
    }
    llvm::StringSet<> used;
    AstWalker::collectNames(block->body[i], used);
    if (llvm::any_of(used, [&](const auto &entry) { return names.contains(entry.getKey()); })) {
      group->firstUse = i;
    }
  }
  this->callCycles->findCycles(group->members);

  return group;
}

void NestedFunctions::finishGroup(NestedGroup *group) {
  group->finished = true;
  for (FunctionDecl *fn : group->members) {
    if (!fn->sym->hasReturnType()) {
      this->returnInferrer->inferReturnType(fn->sym);
    }
    if (fn->checked) {
      continue;
    }
    BodyState saved = this->returnInferrer->enterBody(fn);
    this->functionChecker->walkFunctionBody(fn);
    this->returnInferrer->leaveBody(saved);
  }
  for (const NestedGroup::Use &use : group->uses) {
    ResultUseInOrder ordered = NestedFunctions::useOrder(group, use);
    if (ordered.isErr()) {
      VarSymbol *later = NestedFunctions::capturedLater(group, use);
      std::string declared = ErrorTextUseOrder::declared(later->name);
      this->state.getDiagnostics().error(use.loc, ordered.error().message);
      this->state.getDiagnostics().note(later->loc, declared);
    }
  }
}

VarSymbol *NestedFunctions::capturedLater(const NestedGroup *group, const NestedGroup::Use &use) {
  std::vector<FunctionDecl *> reached{use.fn};
  for (size_t i = 0; i < reached.size(); ++i) {
    for (FunctionDecl *sibling : reached[i]->siblings) {
      if (!llvm::is_contained(reached, sibling)) {
        reached.push_back(sibling);
      }
    }
  }
  for (FunctionDecl *fn : reached) {
    for (VarSymbol *var : fn->captures) {
      if (var->owner() == group->owner && var->declSeq() > use.seq) {
        return var;
      }
    }
  }

  return nullptr;
}

NestedFunctions::ResultUseInOrder NestedFunctions::useOrder(const NestedGroup *group,
                                                            const NestedGroup::Use &use) {
  VarSymbol *later = NestedFunctions::capturedLater(group, use);
  if (later) {
    std::string message = ErrorTextUseOrder::early(use.fn->name, later->name);

    return err(NestedFunctionsUseOrderError{
        .code = NestedFunctionsUseOrderErrorCode::USED_BEFORE_DECLARED,
        .message = message,
    });
  }

  return ok(&use);
}

// TODO: говнокод
void NestedFunctions::noteUse(VarSymbol *var, llvm::SMLoc loc) {
  LocalVarState *localState = var->local();
  if (!localState) {
    return;
  }
  if (FunctionDecl *member = var->function();
      member && this->state.getFunctionStack().getCurrentDecl() == member->group->owner) {
    member->group->uses.push_back(
        {member, this->state.getReturnInference().getSequenceNumber(), loc});
  }
  std::vector<FunctionDecl *> functions = this->state.getFunctionStack().getFunctions();
  for (size_t i = functions.size(); i-- > 0;) {
    FunctionDecl *fn = functions[i];
    if (fn == var->owner() || fn == var->lambda()) {
      break;
    }
    if (var->function() && fn->group && fn->group == var->function()->group) {
      if (fn != var->function() && !llvm::is_contained(fn->siblings, var->function())) {
        fn->siblings.push_back(var->function());
      }
      break;
    }
    if (!llvm::is_contained(fn->captures, var)) {
      fn->captures.push_back(var);
    }
    if (fn->group && !llvm::is_contained(fn->group->captures, var)) {
      fn->group->captures.push_back(var);
    }
    localState->captured = true;
    unsigned created = fn->group ? fn->group->creationSeq : fn->creationSeq;
    if (created <= localState->declSeq) {
      localState->capturedEarly = true;
    }
  }
}

FunctionDecl *NestedFunctions::namedFunction(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name) {
    return nullptr;
  }
  if (name->func()) {
    return name->func()->decl;
  }
  if (VarSymbol *var = name->var()) {
    return var->function() ? var->function() : var->isConst ? var->lambda() : nullptr;
  }

  return nullptr;
}

}
