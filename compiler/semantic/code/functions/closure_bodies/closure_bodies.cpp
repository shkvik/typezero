#include "compiler/semantic/code/functions/closure_bodies/closure_bodies.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context_errors.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

ClosureBodies::ClosureBodies(SemanticState &owner, FunctionRules &sharedFunctionRules)
    : state(owner),
      functionRules(sharedFunctionRules) {}

void ClosureBodies::afterClosureReturn(const AfterClosureReturn &task) {
  const ClosureDraft &draft = task.draft;
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  FuncSymbol *function = draft.function;
  auto &tasks = this->state.walk.tasks;
  if (draft.isGeneric) {
    this->state.walk.scopes.closeTypeScope();
  }
  tasks.pushTask(AfterClosureType{.functionExpr = draft.functionExpr});
  tasks.pushTask(FunctionTypeOf{.function = function});
  tasks.pushTask(StartClosureBody{.functionDecl = functionDecl});
  if (function->hasReturnType() || !this->isRecursiveClosure(functionDecl)) {
    return;
  }

  return this->startRecursiveReturn(functionDecl, function);
}

void ClosureBodies::startRecursiveReturn(FunctionDecl *functionDecl, FuncSymbol *function) {
  auto &tasks = this->state.walk.tasks;
  this->state.inference.cycles.addCycle({function});
  auto cycle = this->state.inference.cycles.getCycleOf(function);
  bool isOwnScope = !functionDecl->ownName().empty();
  tasks.pushTask(LeaveReturnBody{});
  if (isOwnScope) {
    tasks.pushTask(CloseScope{});
  }
  tasks.pushTask(InferCycle{.cycle = std::move(cycle)});
  if (isOwnScope) {
    tasks.pushTask(DeclareOwnName{.functionDecl = functionDecl});
  }

  return tasks.pushTask(EnterReturnBody{.functionDecl = functionDecl});
}

void ClosureBodies::startClosureBody(const StartClosureBody &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  auto &classContext = this->state.walk.self;
  auto *initializer = classContext.getInitializer();
  bool isInInitializer =
      initializer != nullptr && this->state.walk.functions.getCurrentDecl() == initializer;
  if (!isInInitializer) {
    return this->pushClosureBody(functionDecl);
  }
  bool isReady = this->isThisReady(this->state.walk.flow.getFlow());
  classContext.enterInitializerClosure(isReady);
  this->state.walk.tasks.pushTask(LeaveInitializerClosure{});

  return this->pushClosureBody(functionDecl);
}

bool ClosureBodies::isThisReady(const Flow &flow) const {
  if (flow.beforeSuper) {
    return false;
  }
  const ClassDecl *initialized = this->state.walk.self.getInitializer()->cls();
  for (const ClassField &field : initialized->fields) {
    if (this->isUnsetField(flow, field)) {
      return false;
    }
  }

  return true;
}

bool ClosureBodies::isUnsetField(const Flow &flow, const ClassField &field) const {
  return !field.isStatic() && field.method() == nullptr && !field.optional() &&
         !llvm::is_contained(flow.setFields, field.name);
}

void ClosureBodies::leaveInitializerClosure() {
  return this->state.walk.self.leaveInitializerClosure();
}

void ClosureBodies::pushClosureBody(FunctionDecl *functionDecl) {
  auto &tasks = this->state.walk.tasks;
  Flow outer = this->state.walk.flow.getFlow();
  bool isOwnScope = functionDecl->isExpression() && !functionDecl->ownName().empty();
  bool isTyped = functionDecl->sym->hasReturnType();
  tasks.pushTask(LeaveReturnBody{});
  if (isOwnScope) {
    tasks.pushTask(CloseScope{});
  }
  if (isTyped) {
    tasks.pushTask(WalkFunctionBody{.functionDecl = functionDecl});
  }
  if (!isTyped) {
    tasks.pushTask(InferFromBody{.functionDecl = functionDecl});
  }
  if (isOwnScope) {
    tasks.pushTask(DeclareOwnName{.functionDecl = functionDecl});
  }
  tasks.pushTask(KeepConstantFlow{.outer = std::move(outer)});

  return tasks.pushTask(EnterReturnBody{.functionDecl = functionDecl});
}

void ClosureBodies::declareOwnName(const DeclareOwnName &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->state.walk.scopes.openScope();
  auto *self = this->state.ast.newVar();
  self->name = functionDecl->ownName();
  self->loc = functionDecl->loc();
  self->isConst = true;
  self->holds = Lambda{.lambda = functionDecl};
  unsigned declSeq = this->state.declaration.order.takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = functionDecl, .state = {.declSeq = declSeq}};
  auto added = this->state.walk.scopes.declare(functionDecl->ownName(), {VarEntry{.var = self}});
  if (added.isOk()) {
    return;
  }
  this->state.diagnostics.error(functionDecl->loc(), added.error().message);
  if (added.error().code == ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  auto previousEntry = this->state.walk.scopes.findName(functionDecl->ownName());
  auto declaredHere = ErrorTextDeclareName::previous();
  this->state.diagnostics.note(previousEntry.loc(), declaredHere);
}

void ClosureBodies::keepConstantFlow(const KeepConstantFlow &task) {
  const Flow &outer = task.outer;
  auto &flowState = this->state.walk.flow;
  for (const Facts::Fact &fact : outer.facts.all()) {
    if (this->isFixedTerm(fact.lhs) && this->isFixedTerm(fact.rhs)) {
      flowState.getFacts().add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const VarSymbol *variable : outer.defined) {
    if (variable->isConst) {
      flowState.markDefined(variable);
    }
  }
  for (const auto &[variable, type] : outer.narrowed) {
    if (variable->isConst) {
      flowState.addNarrowed(variable, type);
    }
  }
  for (const VarSymbol *variable : outer.okResults) {
    if (variable->isConst) {
      flowState.markOkResult(variable);
    }
  }
  for (const VarSymbol *variable : outer.errResults) {
    if (variable->isConst) {
      flowState.markErrResult(variable);
    }
  }
}

void ClosureBodies::afterClosureType(const AfterClosureType &task) {
  task.functionExpr->type = this->state.walk.tasks.takeAnswer();

  return this->state.walk.tasks.pushChecked(task.functionExpr);
}

void ClosureBodies::checkFunctionValue(const CheckFunctionValue &task) {
  const Signature &signature = task.signature;
  auto count = unsigned(signature.params.size());
  auto *wrapper = this->functionRules.builtinFunctionValue(task.expr, signature, count, {});
  if (wrapper == nullptr) {
    return this->state.walk.tasks.pushTask(
        CheckExprKind{.expr = task.expr, .expected = task.expected});
  }

  return this->state.walk.tasks.pushTask(
      CheckFunctionExpr{.functionExpr = wrapper, .expected = signature});
}

bool ClosureBodies::isRecursiveClosure(FunctionDecl *functionDecl) {
  auto names = llvm::StringSet<>();
  AstWalker::collectNames(functionDecl->body, names);
  if (!functionDecl->ownName().empty() && names.contains(functionDecl->ownName())) {
    return true;
  }
  for (const auto &entry : names) {
    auto found = this->state.walk.scopes.findName(entry.getKey());
    VarSymbol *variable = found.var();
    if (variable != nullptr && variable->lambda() == functionDecl) {
      return true;
    }
  }

  return false;
}

bool ClosureBodies::isFixedTerm(const Term &term) const {
  bool isFirstFixed = term.var == nullptr || (term.var->isConst && !term.length);
  bool isSecondFixed = term.var2 == nullptr || (term.var2->isConst && !term.length2);

  return isFirstFixed && isSecondFixed;
}

}
