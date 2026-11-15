#include "compiler/semantic/library/async/library_async_registry.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/async/promise_method_table/promise_method_table.h"
#include "compiler/program/library/async/promise_resolvers_field_table/promise_resolvers_field_table.h"
#include "compiler/program/library/async/promise_static_table/promise_static_table.h"
#include "compiler/program/library/async/scheduler_function_table/scheduler_function_table.h"
#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr uint64_t kTimerReceiver = 16;
constexpr llvm::StringLiteral kNewPromiseLabel = "new Promise";
constexpr llvm::StringLiteral kPromisePlaceholder = "p";

}

LibraryAsyncRegistry::LibraryAsyncRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryAsyncRegistry::resolvePromiseType(const ResolvePromiseType &task) {
  const TypeRef &ref = *task.ref;
  auto &tasks = this->state.walk.tasks;
  if (ref.args.size() != 1) {
    auto message = ErrorTextPromiseType::typeArgs(ref.name);
    this->state.diagnostics.error(ref.loc, message);

    return tasks.pushAnswer(Type::error());
  }
  tasks.pushTask(FinishPromiseType{});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE_OR_VOID});
}

void LibraryAsyncRegistry::finishPromiseType() {
  auto &tasks = this->state.walk.tasks;
  const Type *written = tasks.takeAnswer();
  const auto *promise = Type::promise(this->withoutPromise(written));

  return tasks.pushAnswer(promise);
}

void LibraryAsyncRegistry::walkAsyncReturn(const WalkAsyncReturn &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  if (!functionDecl->returnType.has_value()) {
    return;
  }
  const Type *declared = task.function->getKnownReturnType();
  auto promise = this->checkPromiseDeclared(functionDecl, declared);
  if (promise.isOk()) {
    return;
  }
  this->state.diagnostics.error(functionDecl->returnType->loc, promise.error().message);
  task.function->setReturnType(Type::promise(declared));
}

void LibraryAsyncRegistry::finishAsyncReturns(const FinishAsyncReturns &task) {
  const FuncSymbol *function = task.function;
  const Type *returned = function->getKnownReturnType();
  if (!returned->isPromise()) {
    return;
  }
  const Type *value = returned->promised();
  if (value == nullptr || value->isError()) {
    return;
  }
  for (ReturnStmt *returnStmt : llvm::reverse(task.returns)) {
    bool isTyped = returnStmt->value != nullptr && !returnStmt->value->type->isError();
    if (isTyped) {
      this->finishAsyncReturn(function, returnStmt, value);
    }
  }
}

void LibraryAsyncRegistry::finishAsyncReturn(const FuncSymbol *function, ReturnStmt *returnStmt,
                                             const Type *value) {
  const Type *type = returnStmt->value->type;
  if (!type->isPromise() && !value->isVoid()) {
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &returnStmt->value, .to = value});
  }
  if (!type->isPromise()) {
    return;
  }
  auto matches = this->checkPromiseMatches(type->promised(), value, function);
  if (matches.isErr()) {
    return this->state.diagnostics.error(returnStmt->value->loc(), matches.error().message);
  }
}

void LibraryAsyncRegistry::checkAwait(const CheckAwait &task) {
  AwaitExpr *await = task.await;
  auto &tasks = this->state.walk.tasks;
  await->type = Type::error();
  this->state.inventory.usesAsync = true;
  auto context = this->checkAwaitContext();
  this->walkContext(context, await->loc());
  const Type *hint = this->promiseHint(await->value, task.expected);
  tasks.pushChecked(await);
  tasks.pushTask(AfterAwaitValue{.await = await});
  tasks.pushTask(CalledFunction{});

  return tasks.pushTask(CheckExprSlot{.slot = &await->value, .expected = hint});
}

void LibraryAsyncRegistry::afterAwaitValue(const AfterAwaitValue &task) {
  AwaitExpr *await = task.await;
  const Type *type = await->value->type;
  if (type->isError()) {
    return;
  }
  auto result = this->checkAwaitable(type);
  if (result.isErr()) {
    return this->state.diagnostics.error(await->value->loc(), result.error().message);
  }
  await->type = result.value();
}

void LibraryAsyncRegistry::walkContext(ResultInAsyncContext context, llvm::SMLoc loc) {
  if (context.isErr()) {
    return this->state.diagnostics.error(loc, context.error().message);
  }
  if (context.value() == nullptr) {
    this->state.inventory.topLevelAwait = true;
  }
}

void LibraryAsyncRegistry::walkForAwait(const WalkForAwait &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  auto context = this->checkForAwaitContext();
  this->walkContext(context, stmt->loc());
  tasks.pushTask(AfterAwaitIterable{.forOf = stmt});
  tasks.pushTask(ApparentSlot{.slot = &stmt->iterable});
  tasks.pushTask(CheckExprSlot{.slot = &stmt->iterable, .expected = nullptr});

  return tasks.pushTask(MarkFused{.expr = stmt->iterable});
}

void LibraryAsyncRegistry::afterAwaitIterable(const AfterAwaitIterable &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterJoinedIterable{.forOf = stmt});
  tasks.pushTask(WriteCheckedSlot{.slot = &stmt->iterable});
  tasks.pushTask(JoinInstances{});

  return tasks.pushChecked(stmt->iterable);
}

void LibraryAsyncRegistry::afterJoinedIterable(const AfterJoinedIterable &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterAsyncUserIterator{.forOf = task.forOf});

  return tasks.pushTask(AsyncUserIterator{.expr = task.forOf->iterable});
}

void LibraryAsyncRegistry::afterAsyncUserIterator(const AfterAsyncUserIterator &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  Expr *iterator = tasks.takeChecked();
  if (iterator != nullptr) {
    stmt->iterable = iterator;

    return this->afterAsyncSource(AfterAsyncSource{.forOf = stmt});
  }
  tasks.pushTask(AfterAsyncSource{.forOf = stmt});

  return tasks.pushTask(IterableOfSlot{.slot = &stmt->iterable});
}

void LibraryAsyncRegistry::afterAsyncSource(const AfterAsyncSource &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  if (!stmt->iterable->type->isMatch()) {
    return this->bindAwaitElement(BindAwaitElement{.forOf = stmt});
  }
  tasks.pushTask(BindAwaitElement{.forOf = stmt});
  tasks.pushTask(WriteCheckedSlot{.slot = &stmt->iterable});
  tasks.pushChecked(stmt->iterable);

  return tasks.pushTask(MatchItems{});
}

void LibraryAsyncRegistry::bindAwaitElement(const BindAwaitElement &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  const Type *element = this->loopElement(stmt->iterable);
  const Type *bound = this->boundElement(stmt->binding, element);
  tasks.pushTask(StartAwaitBody{.forOf = stmt, .element = bound});

  return tasks.pushTask(CalledFunction{});
}

void LibraryAsyncRegistry::startAwaitBody(const StartAwaitBody &task) {
  ForOfStmt *stmt = task.forOf;
  auto &tasks = this->state.walk.tasks;
  auto scan = this->state.flow.loops.scanForOfLoop(stmt);
  scan.isCallingFunctions = true;
  stmt->borrowElements = false;
  auto head = this->state.flow.loops.loopHead(scan);
  this->state.walk.flow.replaceFlow(head);
  this->state.walk.scopes.openScope();
  VarDeclarator &binding = stmt->binding;
  tasks.pushTask(CalledFunction{});
  tasks.pushTask(RestoreLoopFlow{.flow = std::move(head)});
  tasks.pushTask(CloseScope{});
  tasks.pushTask(EnterAwaitLoopBody{.body = stmt->body});

  return tasks.pushTask(DeclareVariableSlot{
      .slot = &binding.sym,
      .name = binding.name,
      .loc = binding.loc,
      .type = task.element,
      .isConst = stmt->isConst,
  });
}

void LibraryAsyncRegistry::enterAwaitLoopBody(const EnterAwaitLoopBody &task) {
  auto &tasks = this->state.walk.tasks;
  auto labels = this->state.walk.block.takePendingLabels();
  auto breaks = Flow();
  breaks.unreachable = true;
  auto target = JumpTarget{
      .kind = JumpTarget::Kind::Loop,
      .labels = std::move(labels),
      .breaks = std::move(breaks),
  };
  this->state.walk.block.enterJumpTarget(std::move(target));
  this->state.walk.functions.enterAwaitLoop();
  tasks.pushTask(LeaveLoopBody{.isAwait = true});

  return tasks.pushTask(WalkBody{.body = task.body});
}

const Type *LibraryAsyncRegistry::loopElement(const Expr *iterable) {
  auto awaited = this->checkAwaitedElement(iterable->type);
  if (awaited.isOk()) {
    this->walkSendsNothing(iterable);

    return awaited.value();
  }
  if (awaited.error().code != LibraryAsyncRegistryAwaitedElementErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(iterable->loc(), awaited.error().message);
  }

  return Type::error();
}

void LibraryAsyncRegistry::walkSendsNothing(const Expr *iterable) {
  const Type *type = iterable->type;
  if (!type->isIterator()) {
    return;
  }
  auto label = ErrorTextAsyncContext::awaitLoop();
  auto nothing = this->state.code.iteration.checkSendsNothing(type, label);
  if (nothing.isErr()) {
    return this->state.diagnostics.error(iterable->loc(), nothing.error().message);
  }
}

void LibraryAsyncRegistry::checkPromiseNew(const CheckPromiseNew &task) {
  NewExpr *newExpr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  auto *call = this->state.ast.make<CallExpr>(newExpr->loc(), newExpr, newExpr->args);
  call->target = BuiltinCall{.builtin = Builtin::PromiseNew};
  call->type = Type::error();
  tasks.pushChecked(call);
  if (newExpr->typeArgs.size() > 1) {
    auto message = ErrorTextPromiseValue::typeArgs();
    this->state.diagnostics.error(newExpr->loc(), message);

    return this->continuePromiseNew(call, Type::error());
  }
  if (newExpr->typeArgs.size() == 1) {
    tasks.pushTask(AfterPromiseTypeArg{.call = call});

    return tasks.pushTask(
        ResolveTypeAnswer{.ref = &newExpr->typeArgs[0], .use = TypeUse::STANDALONE_OR_VOID});
  }
  const Type *wanted = this->expectedPromised(task.expected);
  if (wanted != nullptr && !wanted->isNever()) {
    return this->continuePromiseNew(call, wanted);
  }
  if (call->args.size() != 1) {
    return this->continuePromiseNew(call, nullptr);
  }

  return this->inferExecutorValue(call);
}

void LibraryAsyncRegistry::afterPromiseTypeArg(const AfterPromiseTypeArg &task) {
  const Type *written = this->state.walk.tasks.takeAnswer();

  return this->continuePromiseNew(task.call, this->withoutPromise(written));
}

void LibraryAsyncRegistry::inferExecutorValue(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(call->args[0]);
  if (functionExpr == nullptr) {
    return this->reportExecutorValue(call);
  }
  const FunctionDecl *executorDecl = functionExpr->fn;
  if (executorDecl->params.empty()) {
    return this->continuePromiseNew(call, Type::voidTy());
  }
  llvm::StringRef resolve = executorDecl->params[0].name;
  if (!this->isResolvingWithValue(executorDecl->body, resolve)) {
    return this->continuePromiseNew(call, Type::voidTy());
  }
  if (executorDecl->pristine == nullptr) {
    return this->reportExecutorValue(call);
  }
  auto *copy = this->state.library.common.freshCopy(functionExpr->fn);
  copy->sym = nullptr;
  auto *trial = this->state.ast.make<FunctionExpr>(functionExpr->loc(), copy);
  auto saved = this->state.walk.flow.getFlow();
  auto signature = Signature{
      .params = {Type::error(), Type::error()},
      .ret = Type::voidTy(),
      .voidResult = true,
      .pad = false,
  };
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  tasks.pushTask(AfterExecutorTrial{.call = call, .copy = copy, .resolve = resolve});
  tasks.pushTask(RestoreLoopFlow{.flow = std::move(saved)});
  tasks.pushTask(DropDiagnosticList{});
  tasks.pushTask(DiscardChecked{});

  return tasks.pushTask(CheckFunctionExpr{.functionExpr = trial, .expected = signature});
}

void LibraryAsyncRegistry::afterExecutorTrial(const AfterExecutorTrial &task) {
  const Type *value = this->resolvedValueType(task.copy->body, task.resolve);
  if (value == nullptr) {
    return this->reportExecutorValue(task.call);
  }

  return this->continuePromiseNew(task.call, value);
}

void LibraryAsyncRegistry::reportExecutorValue(CallExpr *call) {
  auto message = ErrorTextPromiseValue::notInferred();
  this->state.diagnostics.error(call->loc(), message);

  return this->continuePromiseNew(call, Type::error());
}

void LibraryAsyncRegistry::continuePromiseNew(CallExpr *call, const Type *value) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(call->loc(), arity.error().message);
  }
  if (value->isError()) {
    return this->pushLooseExecutor(&call->args[0]);
  }
  tasks.pushTask(FinishPromiseNew{.call = call, .value = value});
  tasks.pushTask(CalledFunction{});

  return this->pushTypedExecutor(&call->args[0], value);
}

void LibraryAsyncRegistry::finishPromiseNew(const FinishPromiseNew &task) {
  task.call->type = Type::promise(task.value);
}

void LibraryAsyncRegistry::pushLooseExecutor(Expr **slot) {
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(*slot);
  if (functionExpr == nullptr) {
    return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
  }
  auto loose = Signature{
      .params = {Type::error(), Type::error()},
      .pad = false,
  };
  tasks.pushTask(WriteCheckedSlot{.slot = slot});

  return tasks.pushTask(CheckFunctionExpr{.functionExpr = functionExpr, .expected = loose});
}

void LibraryAsyncRegistry::pushTypedExecutor(Expr **slot, const Type *value) {
  const auto *resolver = this->resolverType(value);
  auto params = std::vector<const Type *>{resolver};
  auto shaped = this->checkExecutorShape(*slot);
  if (shaped.isErr()) {
    llvm::SMLoc rejectLoc = cast<FunctionExpr>(*slot)->fn->params[1].loc;
    this->state.diagnostics.error(rejectLoc, shaped.error().message);
    params.push_back(Type::error());
  }
  auto primary = unsigned(params.size());
  auto signature = Signature{
      .params = params,
      .minArgs = params.size(),
      .ret = Type::voidTy(),
      .voidResult = true,
      .pad = false,
  };

  return this->state.walk.tasks.pushTask(CheckFunctionArgSlot{
      .slot = slot,
      .expected = std::move(signature),
      .primary = primary,
      .method = kNewPromiseLabel,
  });
}

void LibraryAsyncRegistry::checkPromiseMethod(const CheckPromiseMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  auto plain = this->checkNotResultMethod(member);
  if (plain.isErr()) {
    call->type = Type::error();

    return this->reportAfterArgs(call, member->loc(), plain.error().message);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  this->state.inventory.usesAsync = true;
  auto chaining = this->checkIsThen(call, member);
  if (chaining.isErr()) {
    bool isNoMethod =
        chaining.error().code == LibraryAsyncRegistryCheckPromiseMethodErrorCode::NO_SUCH_METHOD;
    bool isTwo = chaining.error().code ==
                 LibraryAsyncRegistryCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS;
    llvm::SMLoc at = isTwo ? call->args[1]->loc() : member->loc();
    if (!isNoMethod) {
      call->type = Type::error();
    }

    return this->reportAfterArgs(call, at, chaining.error().message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseThen};
  if (isa<UndefinedExpr>(call->args[0])) {
    tasks.pushTask(FinishPromiseThen{.call = call, .member = member, .isCallback = false});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }
  auto signature = this->thenSignature(call->args[0], member, task.expected);
  tasks.pushTask(FinishPromiseThen{.call = call, .member = member, .isCallback = true});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[0],
      .expected = std::move(signature),
      .primary = 1,
      .method = member->name,
  });
}

void LibraryAsyncRegistry::reportAfterArgs(CallExpr *call, llvm::SMLoc at,
                                           const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(ReportError{.loc = at, .message = message});

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryAsyncRegistry::finishPromiseThen(const FinishPromiseThen &task) {
  CallExpr *call = task.call;
  const Type *value = task.member->object->type->promised();
  if (!task.isCallback) {
    return this->settlePromiseThen(call, value, value);
  }
  if (!call->args[0]->type->isFunction()) {
    return;
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(call->args[0]);
  if (functionExpr != nullptr) {
    functionExpr->fn->nonEscaping = false;
  }
  const Type *returned = this->withoutPromise(call->args[0]->type->ret());

  return this->settlePromiseThen(call, value, returned);
}

void LibraryAsyncRegistry::settlePromiseThen(CallExpr *call, const Type *value,
                                             const Type *result) {
  if (result->isError()) {
    return;
  }
  const Type *settled = result->isNever() ? value : result;
  call->type = Type::promise(settled);
}

void LibraryAsyncRegistry::checkPromiseStatic(const CheckPromiseStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto plain = this->checkStaticTypeArgs(member);
  if (plain.isErr()) {
    this->state.diagnostics.error(member->loc(), plain.error().message);
    member->typeArgs.clear();
  }
  const auto *wanted = this->wantedValue(task.expected);
  auto promiseName = GlobalNameTable::getName(GlobalName::PROMISE);
  std::string functionName = promiseName.str() + "." + member->name.str();
  auto method = PromiseStaticTable::find(member->name);
  if (!method.has_value()) {
    auto message = ErrorTextPromiseStatic::unknown(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  switch (*method) {
    case PromiseStatic::RESOLVE:
      return this->checkResolve(call, member, wanted, functionName);
    case PromiseStatic::REJECT:
      return this->reportNoReject(call, member, ErrorTextNoReject::rejected());
    case PromiseStatic::ALL_SETTLED:
      return this->reportNoReject(call, member, ErrorTextNoReject::settled());
    case PromiseStatic::ANY:
      return this->reportNoReject(call, member, ErrorTextNoReject::any());
    case PromiseStatic::WITH_RESOLVERS:
      return this->checkWithResolvers(call, member, task.expected);
    case PromiseStatic::ALL:
      return this->checkCombinator(call, member, wanted, functionName, Builtin::PromiseAll);
    case PromiseStatic::RACE:
      return this->checkCombinator(call, member, wanted, functionName, Builtin::PromiseRace);
  }
}

void LibraryAsyncRegistry::reportNoReject(CallExpr *call, const MemberExpr *member,
                                          const std::string &message) {
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushTask(MarkCallFailed{.call = call});
}

void LibraryAsyncRegistry::checkResolve(CallExpr *call, const MemberExpr *member,
                                        const Type *wanted, const std::string &functionName) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseResolve};
  if (call->args.empty()) {
    call->type = Type::promise(Type::voidTy());

    return;
  }
  const Type *hint = wanted != nullptr && !isa<UndefinedExpr>(call->args[0]) ? wanted : nullptr;
  tasks.pushTask(AfterResolveValue{.call = call, .functionName = functionName});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = hint});
}

void LibraryAsyncRegistry::afterResolveValue(const AfterResolveValue &task) {
  CallExpr *call = task.call;
  const Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  if (type->isPromise()) {
    call->type = type;

    return;
  }
  if (type->isUndefined()) {
    call->type = Type::promise(Type::voidTy());

    return;
  }
  auto value = this->checkNotVoid(type, task.functionName);
  if (value.isErr()) {
    return this->state.diagnostics.error(arg->loc(), value.error().message);
  }
  call->type = Type::promise(type);
}

void LibraryAsyncRegistry::checkWithResolvers(CallExpr *call, MemberExpr *member,
                                              const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::PromiseWithResolvers};
  auto single = this->checkResolverTypeArgs(member);
  if (single.isErr()) {
    return this->state.diagnostics.error(member->loc(), single.error().message);
  }
  if (member->typeArgs.size() != 1) {
    const auto *value = this->expectedResolverValue(expected);

    return this->finishWithResolvers(
        FinishWithResolvers{.call = call, .member = member, .value = value});
  }
  tasks.pushTask(AfterResolverTypeArg{.call = call, .member = member});

  return tasks.pushTask(
      ResolveTypeAnswer{.ref = &member->typeArgs[0], .use = TypeUse::STANDALONE_OR_VOID});
}

void LibraryAsyncRegistry::afterResolverTypeArg(const AfterResolverTypeArg &task) {
  const Type *written = this->state.walk.tasks.takeAnswer();
  const auto *value = this->withoutPromise(written);

  return this->finishWithResolvers(
      FinishWithResolvers{.call = task.call, .member = task.member, .value = value});
}

void LibraryAsyncRegistry::finishWithResolvers(const FinishWithResolvers &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  member->typeArgs.clear();
  auto known = this->checkResolverValue(task.value);
  bool isShown = known.isErr() &&
                 known.error().code != LibraryAsyncRegistryResolverValueErrorCode::ALREADY_REPORTED;
  if (isShown) {
    this->state.diagnostics.error(member->loc(), known.error().message);
  }
  if (known.isErr()) {
    return;
  }
  const Type *value = known.value();
  auto promiseName = PromiseResolversFieldTable::getName(PromiseResolversField::PROMISE);
  auto resolveName = PromiseResolversFieldTable::getName(PromiseResolversField::RESOLVE);
  const auto *resolver = this->resolverType(value);
  call->type = Type::object(
      {{promiseName.str(), Type::promise(value), false}, {resolveName.str(), resolver, false}});
}

const Type *LibraryAsyncRegistry::expectedResolverValue(const Type *expected) const {
  if (expected == nullptr || !expected->isObject()) {
    return nullptr;
  }
  auto promiseName = PromiseResolversFieldTable::getName(PromiseResolversField::PROMISE);
  const auto *promise = expected->field(promiseName);
  if (promise == nullptr || !promise->type->isPromise()) {
    return nullptr;
  }

  return promise->type->promised();
}

void LibraryAsyncRegistry::checkCombinator(CallExpr *call, const MemberExpr *member,
                                           const Type *wanted, const std::string &functionName,
                                           Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = builtin};
  bool isRace = builtin == Builtin::PromiseRace;
  const auto *wantedElement = this->combinatorElement(wanted, isRace);
  auto draft = CombinatorDraft{
      .call = call,
      .wantedElement = wantedElement,
      .functionName = functionName,
      .isRace = isRace,
  };
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  auto isSpreadOrHole = std::bind_front(&LibraryAsyncRegistry::isSpreadOrHole, this);
  bool isPlainLiteral = literal != nullptr && llvm::none_of(literal->elements, isSpreadOrHole);
  if (isPlainLiteral && literal->elements.empty()) {
    return this->emptyPromiseList(draft, literal);
  }
  if (isPlainLiteral) {
    return tasks.pushTask(
        NextPromiseElement{.draft = std::move(draft), .literal = literal, .index = 0});
  }
  tasks.pushTask(AfterPromiseValues{.draft = std::move(draft)});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryAsyncRegistry::emptyPromiseList(const CombinatorDraft &draft, ArrayLitExpr *literal) {
  auto typed = this->checkEmptyList(draft.wantedElement, draft.functionName);
  if (typed.isErr()) {
    return this->state.diagnostics.error(literal->loc(), typed.error().message);
  }
  literal->type = Type::array(Type::promise(typed.value()));
  auto list = PromiseList{.element = typed.value(), .tuple = {}};

  return this->typeCombinator(draft, list);
}

void LibraryAsyncRegistry::nextPromiseElement(const NextPromiseElement &task) {
  ArrayLitExpr *literal = task.literal;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (index == literal->elements.size()) {
    return this->finishPromiseLiteral(task.draft, literal);
  }
  ArrayLitExpr::Element &item = literal->elements[index];
  const Type *wantedElement = task.draft.wantedElement;
  bool isShaped =
      wantedElement != nullptr && this->state.inference.destinations.isLiteralShaped(item.expr);
  tasks.pushTask(AfterPromiseElement{.draft = task.draft, .literal = literal, .index = index});

  return tasks.pushTask(
      CheckExprSlot{.slot = &item.expr, .expected = isShaped ? wantedElement : nullptr});
}

void LibraryAsyncRegistry::afterPromiseElement(const AfterPromiseElement &task) {
  const Expr *item = task.literal->elements[task.index].expr;
  llvm::StringRef functionName = task.draft.functionName;
  if (item->type->isError()) {
    return;
  }
  auto value = this->checkNotVoid(item->type, functionName);
  if (value.isErr()) {
    return this->state.diagnostics.error(item->loc(), value.error().message);
  }
  auto plain = this->checkPlainValue(item->type, functionName);
  if (plain.isErr()) {
    return this->state.diagnostics.error(item->loc(), plain.error().message);
  }

  return this->state.walk.tasks.pushTask(
      NextPromiseElement{.draft = task.draft, .literal = task.literal, .index = task.index + 1});
}

void LibraryAsyncRegistry::finishPromiseLiteral(const CombinatorDraft &draft,
                                                ArrayLitExpr *literal) {
  auto types = std::vector<const Type *>();
  for (const ArrayLitExpr::Element &item : literal->elements) {
    types.push_back(item.expr->type);
  }
  bool isSame = llvm::all_equal(types);
  const auto *common = this->unifiedType(types);
  bool isJoined = isSame || (common != nullptr && !common->isError());
  if (isJoined) {
    const Type *same = isSame ? types.front() : common;
    literal->type = Type::array(same);
    auto list = PromiseList{.element = this->awaited(same), .tuple = {}};
    this->typeCombinator(draft, list);

    return this->convertPromiseElements(literal, isSame ? nullptr : common);
  }
  literal->type = Type::tuple(types);
  auto tuple = std::vector<const Type *>();
  for (const Type *type : types) {
    tuple.push_back(this->awaited(type));
  }
  auto list = PromiseList{.element = nullptr, .tuple = tuple};

  return this->typeCombinator(draft, list);
}

void LibraryAsyncRegistry::convertPromiseElements(ArrayLitExpr *literal, const Type *common) {
  if (common == nullptr) {
    return;
  }
  for (ArrayLitExpr::Element &item : llvm::reverse(literal->elements)) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &item.expr, .to = common});
  }
}

void LibraryAsyncRegistry::afterPromiseValues(const AfterPromiseValues &task) {
  const CombinatorDraft &draft = task.draft;
  const Expr *arg = draft.call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  auto list = this->checkPromiseArray(arg, draft.functionName);
  if (list.isErr()) {
    return this->state.diagnostics.error(arg->loc(), list.error().message);
  }
  if (!type->isArray()) {
    return this->tuplePromiseList(draft, arg);
  }
  auto plain = this->checkPlainValue(type->inner(), draft.functionName);
  if (plain.isErr()) {
    return this->state.diagnostics.error(arg->loc(), plain.error().message);
  }
  auto values = PromiseList{.element = this->awaited(type->inner()), .tuple = {}};

  return this->typeCombinator(draft, values);
}

void LibraryAsyncRegistry::tuplePromiseList(const CombinatorDraft &draft, const Expr *arg) {
  auto tuple = std::vector<const Type *>();
  for (const Field &field : arg->type->fields()) {
    auto plain = this->checkPlainValue(field.type, draft.functionName);
    if (plain.isErr()) {
      return this->state.diagnostics.error(arg->loc(), plain.error().message);
    }
    tuple.push_back(this->awaited(field.type));
  }
  auto list = PromiseList{.element = nullptr, .tuple = tuple};

  return this->typeCombinator(draft, list);
}

void LibraryAsyncRegistry::typeCombinator(const CombinatorDraft &draft, const PromiseList &list) {
  CallExpr *call = draft.call;
  if (draft.isRace) {
    return this->typeRaceCall(call, list, draft.wantedElement, draft.functionName);
  }
  const auto *element = this->settledElement(list.element, draft.wantedElement);

  return this->typeAllCall(call, element, list.tuple);
}

void LibraryAsyncRegistry::typeRaceCall(CallExpr *call, const PromiseList &list,
                                        const Type *wantedElement, llvm::StringRef functionName) {
  auto joined = this->checkRaceElement(list, functionName);
  if (joined.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), joined.error().message);
  }
  const auto *element = this->settledElement(joined.value(), wantedElement);
  call->type = Type::promise(element);
}

void LibraryAsyncRegistry::typeAllCall(CallExpr *call, const Type *element,
                                       const std::vector<const Type *> &tuple) {
  if (!tuple.empty()) {
    auto values = this->checkAllValues(tuple);
    if (values.isErr()) {
      return this->state.diagnostics.error(call->args[0]->loc(), values.error().message);
    }
    call->type = Type::promise(values.value());

    return;
  }
  if (element->isVoid()) {
    call->type = Type::promise(Type::voidTy());

    return;
  }
  call->type = Type::promise(Type::array(element));
}

void LibraryAsyncRegistry::pushTaskCallback(Expr **slot, llvm::StringRef functionName) {
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(*slot);
  if (functionExpr != nullptr) {
    auto signature = Signature{
        .ret = Type::voidTy(),
        .voidResult = true,
        .asyncVoid = true,
    };
    tasks.pushTask(WriteCheckedSlot{.slot = slot});

    return tasks.pushTask(CheckFunctionExpr{.functionExpr = functionExpr, .expected = signature});
  }
  tasks.pushTask(AfterTaskCallback{.slot = slot, .functionName = functionName});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

void LibraryAsyncRegistry::afterTaskCallback(const AfterTaskCallback &task) {
  Expr *arg = *task.slot;
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  const auto *callback = Type::function({}, 0, false, Type::voidTy());
  bool isAdaptable =
      type->isFunction() && type != callback && type->minArgs() == 0 && !type->hasRest();
  Expr *adapted = isAdaptable ? this->adaptedTask(arg, callback) : nullptr;
  auto fits = this->checkTaskCallbackType(type, callback, adapted, task.functionName);
  if (fits.isErr()) {
    return this->state.diagnostics.error(arg->loc(), fits.error().message);
  }
  if (adapted != nullptr) {
    *task.slot = adapted;
  }
}

Expr *LibraryAsyncRegistry::adaptedTask(Expr *arg, const Type *callback) {
  if (this->namedFunction(arg) == nullptr) {
    return nullptr;
  }
  auto *castExpr = this->state.ast.make<CastExpr>(arg->loc(), arg, std::nullopt);
  castExpr->type = callback;

  return castExpr;
}

FunctionDecl *LibraryAsyncRegistry::namedFunction(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  if (name->func() != nullptr) {
    return name->func()->decl;
  }
  const VarSymbol *variable = name->var();
  if (variable == nullptr) {
    return nullptr;
  }
  if (variable->function() != nullptr) {
    return variable->function();
  }

  return variable->isConst ? variable->lambda() : nullptr;
}

void LibraryAsyncRegistry::checkAsyncGlobal(const CheckAsyncGlobal &task) {
  CallExpr *call = task.call;
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  if (!this->isTimerName(name)) {
    return tasks.pushChecked(nullptr);
  }
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto global = GlobalNameTable::find(name->name);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (global == GlobalName::SET_TIMEOUT || global == GlobalName::SET_INTERVAL) {
    return this->checkTimeoutCall(call, name, global == GlobalName::SET_INTERVAL);
  }
  if (global == GlobalName::SET_IMMEDIATE || global == GlobalName::QUEUE_MICROTASK) {
    return this->checkTaskQueueCall(call, name, global == GlobalName::SET_IMMEDIATE);
  }

  return this->checkClearCall(call, name, global == GlobalName::CLEAR_IMMEDIATE);
}

void LibraryAsyncRegistry::checkTimeoutCall(CallExpr *call, const NameExpr *name, bool isInterval) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() > 2) {
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return this->reportAfterArgs(call, call->args[2]->loc(), message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(name->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = isInterval ? Builtin::SetInterval : Builtin::SetTimeout};
  call->type = Type::timeout();
  tasks.pushTask(AfterTimerCallback{.call = call, .isInterval = isInterval});
  if (call->args.size() > 1) {
    this->pushAnyNumber(&call->args[1]);
  }

  return this->pushTaskCallback(&call->args[0], name->name);
}

void LibraryAsyncRegistry::pushAnyNumber(Expr **slot) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterAnyNumber{.slot = slot});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

void LibraryAsyncRegistry::afterAnyNumber(const AfterAnyNumber &task) {
  auto &tasks = this->state.walk.tasks;
  if ((*task.slot)->type->isInteger()) {
    return tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::doubleTy()});
  }

  return tasks.pushTask(CoerceSlot{.slot = task.slot, .to = Type::doubleTy(), .what = ""});
}

void LibraryAsyncRegistry::afterTimerCallback(const AfterTimerCallback &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  Expr *callback = call->args[0];
  if (callback->type->isError()) {
    return;
  }
  bool isClassifying = task.isInterval || this->isRearming(callback);
  if (!isClassifying) {
    return;
  }
  auto *bits = this->state.ast.make<IntLitExpr>(call->loc(), uint64_t(0));
  tasks.pushTask(AfterTimerReceiver{.call = call, .isInterval = task.isInterval, .bits = bits});

  return tasks.pushTask(ClassifyListener{
      .arg = callback,
      .emitter = nullptr,
      .use = ListenerUse::TIMER,
      .bits = bits,
  });
}

void LibraryAsyncRegistry::afterTimerReceiver(const AfterTimerReceiver &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  bool isClassified = tasks.takeVerdict();
  uint64_t bits = task.bits->value;
  uint64_t kept = task.isInterval ? (isClassified ? bits : 0) : (bits & kTimerReceiver);
  if (kept == 0) {
    return;
  }
  task.bits->value = kept;
  if (call->args.size() < 2) {
    auto *delay = this->state.ast.make<IntLitExpr>(call->loc(), uint64_t(1));
    call->args.push_back(delay);
  }
  call->args.push_back(task.bits);
  size_t delayAt = call->args.size() - 2;
  tasks.pushTask(CheckExprSlot{.slot = &call->args.back(), .expected = Type::intTy()});
  if (call->args[delayAt]->type != nullptr) {
    return;
  }

  return this->pushAnyNumber(&call->args[delayAt]);
}

void LibraryAsyncRegistry::checkTaskQueueCall(CallExpr *call, const NameExpr *name,
                                              bool isImmediate) {
  if (call->args.size() > 1 && isImmediate) {
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return this->reportAfterArgs(call, call->args[1]->loc(), message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(name->loc(), arity.error().message);
  }
  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::SetImmediate : Builtin::QueueMicrotask};
  call->type = isImmediate ? Type::immediate() : Type::voidTy();

  return this->pushTaskCallback(&call->args[0], name->name);
}

void LibraryAsyncRegistry::checkClearCall(CallExpr *call, const NameExpr *name, bool isImmediate) {
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(name->loc(), arity.error().message);
  }
  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::ClearImmediate : Builtin::ClearTimeout};
  if (call->args.empty()) {
    auto *none = this->state.ast.make<UndefinedExpr>(call->loc());
    none->type = Type::undefinedTy();
    call->args.push_back(none);
  }
  const auto *handle = Type::optional(isImmediate ? Type::immediate() : Type::timeout());
  call->type = Type::voidTy();

  return this->state.walk.tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = handle});
}

void LibraryAsyncRegistry::checkNextTick(const CheckNextTick &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (call->args.size() > 1) {
    auto message = ErrorTextAsyncGlobal::callbackArg();

    return this->reportAfterArgs(call, call->args[1]->loc(), message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::NextTick};
  call->type = Type::voidTy();
  auto nextTick = ProcessQualifiedNameTable::getQualifiedName(ProcessMember::NEXT_TICK);

  return this->pushTaskCallback(&call->args[0], nextTick);
}

void LibraryAsyncRegistry::checkTimerMethod(const CheckTimerMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto method = TimerMethodTable::find(name);
  bool isAvailable = method.has_value() && (method != TimerMethod::REFRESH || !type->isImmediate());
  if (!isAvailable) {
    auto message = ErrorTextTimerMethod::noMethod(name, type->name(), type->isImmediate());

    return this->reportAfterArgs(call, member->loc(), message);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  Builtin builtin = this->timerMethodBuiltin(method.value());
  call->target = BuiltinCall{.builtin = builtin};
  call->type = method == TimerMethod::HAS_REF ? Type::boolTy() : type;
}

void LibraryAsyncRegistry::checkTimersPromisesCall(const CheckTimersPromisesCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);

  return this->startTimersPromises(call, task.functionName, task.loc, task.expected);
}

void LibraryAsyncRegistry::startTimersPromises(CallExpr *call, llvm::StringRef functionName,
                                               llvm::SMLoc loc, const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  auto function = TimersPromisesFunctionTable::find(functionName);
  if (function == TimersPromisesFunction::SCHEDULER) {
    auto message = ErrorTextTimersPromises::scheduler();

    return this->reportAfterArgs(call, loc, message);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  bool isImmediate = function == TimersPromisesFunction::SET_IMMEDIATE;
  size_t valueAt = isImmediate ? 0 : 1;
  auto arity = this->state.library.common.checkArity(call, 0, unsigned(valueAt + 2));
  if (arity.isErr()) {
    return this->state.diagnostics.error(loc, arity.error().message);
  }
  auto draft = TimerPromiseDraft{
      .call = call,
      .functionName = functionName,
      .loc = loc,
      .expected = expected,
      .isImmediate = isImmediate,
      .isInterval = function == TimersPromisesFunction::SET_INTERVAL,
  };
  tasks.pushTask(TimerPromiseValue{.draft = draft});
  bool isDelayMissing = call->args.empty() || isa<UndefinedExpr>(call->args[0]);
  if (isImmediate || isDelayMissing) {
    return;
  }

  return this->pushAnyNumber(&call->args[0]);
}

void LibraryAsyncRegistry::timerPromiseValue(const TimerPromiseValue &task) {
  const TimerPromiseDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  size_t valueAt = draft.isImmediate ? 0 : 1;
  Expr **given = valueAt < call->args.size() ? &call->args[valueAt] : nullptr;
  if (given == nullptr) {
    return this->readTimerOptions(draft, Type::voidTy(), nullptr);
  }
  if (isa<UndefinedExpr>(*given)) {
    tasks.pushTask(
        TimerPromiseOptions{.draft = draft, .value = Type::voidTy(), .argument = nullptr});
    tasks.pushTask(DiscardChecked{});

    return tasks.pushTask(CheckExpr{.expr = *given, .expected = nullptr});
  }
  const auto *wanted = this->promisedHint(draft.expected);
  tasks.pushTask(AfterTimerPromiseValue{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = given, .expected = wanted});
}

void LibraryAsyncRegistry::afterTimerPromiseValue(const AfterTimerPromiseValue &task) {
  const TimerPromiseDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  size_t valueAt = draft.isImmediate ? 0 : 1;
  Expr *given = call->args[valueAt];
  const Type *value = given->type;
  if (value->isError()) {
    return;
  }
  if (value->isVoid()) {
    auto message = ErrorTextTimersPromises::voidValue(draft.functionName);

    return this->state.diagnostics.error(given->loc(), message);
  }
  if (value->isUndefined()) {
    return this->readTimerOptions(draft, Type::voidTy(), nullptr);
  }
  const Type *kept = value->isLiteral() ? value->literalBase() : value;
  tasks.pushTask(
      TimerPromiseOptions{.draft = draft, .value = kept, .argument = &call->args[valueAt]});

  return tasks.pushTask(CoerceSlot{.slot = &call->args[valueAt], .to = kept, .what = ""});
}

void LibraryAsyncRegistry::timerPromiseOptions(const TimerPromiseOptions &task) {
  return this->readTimerOptions(task.draft, task.value, task.argument);
}

void LibraryAsyncRegistry::readTimerOptions(const TimerPromiseDraft &draft, const Type *value,
                                            Expr **argument) {
  auto &tasks = this->state.walk.tasks;
  size_t valueAt = draft.isImmediate ? 0 : 1;
  auto ref = std::vector<NodeOption>{NodeOption::REF};
  tasks.pushTask(FinishTimerPromise{.draft = draft, .value = value, .argument = argument});

  return tasks.pushTask(ReadNodeOptions{
      .call = draft.call,
      .index = valueAt + 1,
      .functionName = draft.functionName,
      .allowed = std::move(ref),
  });
}

void LibraryAsyncRegistry::finishTimerPromise(const FinishTimerPromise &task) {
  const TimerPromiseDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  bool isRead = tasks.takeVerdict();
  if (!isRead) {
    return;
  }
  size_t valueAt = draft.isImmediate ? 0 : 1;
  Expr *refOption = this->state.library.common.findNodeOption(call, valueAt + 1, NodeOption::REF);
  auto args = std::vector<Expr *>();
  if (!draft.isImmediate) {
    args.push_back(this->delayArg(call, draft.loc));
  }
  if (task.argument != nullptr) {
    args.push_back(*task.argument);
  }
  args.push_back(refOption != nullptr ? refOption : this->trueLiteral(draft.loc));
  call->args = std::move(args);
  call->spread.clear();
  this->promisedTimer(call, task.value, draft.isImmediate, draft.isInterval);
  if (refOption == nullptr) {
    return;
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args.back(), .to = Type::boolTy()});
}

Expr *LibraryAsyncRegistry::delayArg(CallExpr *call, llvm::SMLoc loc) {
  bool isMissing = call->args.empty() || isa<UndefinedExpr>(call->args[0]);
  if (!isMissing) {
    return call->args[0];
  }
  auto *one = this->state.ast.make<FloatLitExpr>(loc, 1.0);
  one->type = Type::doubleTy();

  return one;
}

Expr *LibraryAsyncRegistry::trueLiteral(llvm::SMLoc loc) {
  auto *yes = this->state.ast.make<BoolLitExpr>(loc, true);
  yes->type = Type::boolTy();

  return yes;
}

void LibraryAsyncRegistry::promisedTimer(CallExpr *call, const Type *value, bool isImmediate,
                                         bool isInterval) const {
  if (isInterval) {
    call->target = BuiltinCall{.builtin = Builtin::TimersSetInterval};
    call->type = Type::asyncIterator(value->isVoid() ? Type::undefinedTy() : value);

    return;
  }
  call->target =
      BuiltinCall{.builtin = isImmediate ? Builtin::TimersSetImmediate : Builtin::TimersSetTimeout};
  call->type = Type::promise(value);
}

void LibraryAsyncRegistry::checkSchedulerCall(const CheckSchedulerCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  this->state.inventory.usesAsync = true;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto function = SchedulerFunctionTable::find(member->name);
  if (!function.has_value()) {
    auto message = ErrorTextScheduler::noFunction(member->name);

    return this->reportAfterArgs(call, member->loc(), message);
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  bool isWait = function == SchedulerFunction::WAIT;
  auto arity = this->state.library.common.checkArity(call, isWait ? 1 : 0, isWait ? 2 : 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  if (isWait) {
    Expr *options = call->args.size() > 1 ? call->args[1] : nullptr;
    auto *none = this->state.ast.make<UndefinedExpr>(member->loc());
    call->args = {call->args[0], none};
    if (options != nullptr) {
      call->args.push_back(options);
    }
  }
  auto timer = isWait ? TimersPromisesFunction::SET_TIMEOUT : TimersPromisesFunction::SET_IMMEDIATE;
  auto timerName = TimersPromisesFunctionTable::getName(timer);

  return this->startTimersPromises(call, timerName, member->loc(), nullptr);
}

const Type *LibraryAsyncRegistry::withoutPromise(const Type *type) const {
  return type->isPromise() ? type->promised() : type;
}

LibraryAsyncRegistry::ResultReturnsPromise
LibraryAsyncRegistry::checkPromiseDeclared(const FunctionDecl *functionDecl,
                                           const Type *declared) const {
  bool isPlainAsync = functionDecl->isAsync && !functionDecl->isGenerator;
  if (isPlainAsync && !declared->isError() && !declared->isPromise()) {
    auto message = ErrorTextPromiseType::notPromise(declared->name());

    return err(LibraryAsyncRegistryPromiseDeclaredError{
        .code = LibraryAsyncRegistryPromiseDeclaredErrorCode::NOT_A_PROMISE,
        .message = message,
    });
  }

  return ok(declared);
}

LibraryAsyncRegistry::ResultPromiseMatches
LibraryAsyncRegistry::checkPromiseMatches(const Type *given, const Type *value,
                                          const FuncSymbol *function) const {
  if (given != value && !given->isNever()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextAsyncReturns::promise(given->name(), name, value->name());

    return err(LibraryAsyncRegistryPromiseMatchesError{
        .code = LibraryAsyncRegistryPromiseMatchesErrorCode::OTHER_PROMISE_TYPE,
        .message = message,
    });
  }

  return ok(given);
}

LibraryAsyncRegistry::ResultInAsyncContext LibraryAsyncRegistry::checkAwaitContext() const {
  auto *function = this->state.walk.functions.getCurrentFunction();
  if (this->isPlainFunction(function)) {
    auto hint = this->asyncHint(function->decl);
    auto message = ErrorTextAsyncContext::await(hint);

    return err(LibraryAsyncRegistryAsyncContextError{
        .code = LibraryAsyncRegistryAsyncContextErrorCode::AWAIT_NOT_ASYNC,
        .message = message,
    });
  }

  return ok(function);
}

LibraryAsyncRegistry::ResultInAsyncContext LibraryAsyncRegistry::checkForAwaitContext() const {
  auto *function = this->state.walk.functions.getCurrentFunction();
  if (this->isPlainFunction(function)) {
    auto message = ErrorTextAsyncContext::forAwait();

    return err(LibraryAsyncRegistryAsyncContextError{
        .code = LibraryAsyncRegistryAsyncContextErrorCode::FOR_AWAIT_NOT_ASYNC,
        .message = message,
    });
  }

  return ok(function);
}

bool LibraryAsyncRegistry::isPlainFunction(const FuncSymbol *function) const {
  if (function == nullptr) {
    return false;
  }
  bool isAsync = function->decl != nullptr && function->decl->isAsync;

  return !isAsync;
}

std::string LibraryAsyncRegistry::asyncHint(const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr) {
    return ErrorTextAsyncContext::plainHint();
  }
  if (functionDecl->isArrow()) {
    return ErrorTextAsyncContext::arrowHint();
  }
  if (functionDecl->cls() != nullptr) {
    return ErrorTextAsyncContext::methodHint(functionDecl->name);
  }
  if (!functionDecl->name.empty()) {
    return ErrorTextAsyncContext::namedHint(functionDecl->name);
  }

  return ErrorTextAsyncContext::plainHint();
}

LibraryAsyncRegistry::ResultAwaitedOneType
LibraryAsyncRegistry::checkAwaitable(const Type *type) const {
  const auto *result = this->state.library.async.awaitedType(type);
  if (result == nullptr) {
    auto message = ErrorTextAwaited::mixed(type->name());

    return err(LibraryAsyncRegistryAwaitableError{
        .code = LibraryAsyncRegistryAwaitableErrorCode::MIXED_PROMISE,
        .message = message,
    });
  }

  return ok(result);
}

const Type *LibraryAsyncRegistry::promiseHint(const Expr *value, const Type *expected) const {
  bool isExpected = expected != nullptr && !expected->isError() && !expected->isVoid();
  bool isMaking = isa<NewExpr>(value) || isa<CallExpr>(value);
  if (isExpected && isMaking) {
    return Type::promise(expected);
  }

  return nullptr;
}

LibraryAsyncRegistry::ResultAwaitedElement
LibraryAsyncRegistry::checkAwaitedElement(const Type *type) const {
  const auto *element = this->state.code.iteration.asyncIteratedType(type);
  if (element != nullptr) {
    return ok(element);
  }
  if (this->isOptionalAsyncIterable(type)) {
    auto message = ErrorTextAwaitedElement::undefined(type->name());

    return err(LibraryAsyncRegistryAwaitedElementError{
        .code = LibraryAsyncRegistryAwaitedElementErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type->isRecord()) {
    auto message = ErrorTextAwaitedElement::record();

    return err(LibraryAsyncRegistryAwaitedElementError{
        .code = LibraryAsyncRegistryAwaitedElementErrorCode::RECORD_NOT_ITERABLE,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextAwaitedElement::notAsync(type->name());

    return err(LibraryAsyncRegistryAwaitedElementError{
        .code = LibraryAsyncRegistryAwaitedElementErrorCode::NOT_ASYNC_ITERABLE,
        .message = message,
    });
  }

  return err(LibraryAsyncRegistryAwaitedElementError{
      .code = LibraryAsyncRegistryAwaitedElementErrorCode::ALREADY_REPORTED,
      .message = "",
  });
}

bool LibraryAsyncRegistry::isOptionalAsyncIterable(const Type *type) const {
  if (!type->isOptional()) {
    return false;
  }
  const auto *element = this->state.code.iteration.asyncIteratedType(type->inner());

  return element != nullptr;
}

LibraryAsyncRegistry::ResultBindingUntyped
LibraryAsyncRegistry::checkBindingType(const VarDeclarator &binding) const {
  if (binding.typeAnn.has_value()) {
    auto message = ErrorTextBindingType::annotated();

    return err(LibraryAsyncRegistryBindingTypeError{
        .code = LibraryAsyncRegistryBindingTypeErrorCode::HAS_TYPE_ANNOTATION,
        .message = message,
    });
  }

  return ok(&binding);
}

LibraryAsyncRegistry::ResultBindingPattern
LibraryAsyncRegistry::checkBindingShape(const VarDeclarator &binding, const Type *element) const {
  bool isListLike =
      element->isArray() || element->isTuple() || element->isMatch() || element->isError();
  bool isFitting = !binding.destructured() || binding.objectPattern() || isListLike;
  if (!isFitting) {
    auto message = ErrorTextBindingShape::notArray(element->name());

    return err(LibraryAsyncRegistryBindingShapeError{
        .code = LibraryAsyncRegistryBindingShapeErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(element);
}

const Type *LibraryAsyncRegistry::boundElement(const VarDeclarator &binding, const Type *element) {
  auto typed = this->checkBindingType(binding);
  if (typed.isErr()) {
    this->state.diagnostics.error(binding.typeAnn->loc, typed.error().message);
  }
  auto shape = this->checkBindingShape(binding, element);
  if (shape.isErr()) {
    this->state.diagnostics.error(binding.loc, shape.error().message);

    return Type::error();
  }

  return element;
}

const Type *LibraryAsyncRegistry::expectedPromised(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }

  return expected->nonOptional()->promised();
}

const Type *LibraryAsyncRegistry::resolverType(const Type *value) const {
  auto params = std::vector<const Type *>();
  if (!value->isVoid()) {
    params.push_back(value);
  }

  return Type::function(params, params.size(), false, Type::voidTy());
}

const CallExpr *LibraryAsyncRegistry::resolveCall(const Expr *expr, llvm::StringRef resolve) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || call->args.empty()) {
    return nullptr;
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr || name->name != resolve) {
    return nullptr;
  }

  return call;
}

bool LibraryAsyncRegistry::isResolvingWithValue(const Stmt *body, llvm::StringRef resolve) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->resolveCall(placed.expr, resolve) != nullptr) {
      return true;
    }
  }

  return false;
}

const Type *LibraryAsyncRegistry::resolvedValueType(const Stmt *body,
                                                    llvm::StringRef resolve) const {
  const Type *found = nullptr;
  bool isUnknown = false;
  for (const CallExpr *call : this->resolveCalls(body, resolve)) {
    const auto *joined = this->joinedValue(found, call->args[0]->type);
    if (joined == nullptr) {
      isUnknown = true;
    } else {
      found = joined;
    }
  }

  return isUnknown ? nullptr : found;
}

std::vector<const CallExpr *> LibraryAsyncRegistry::resolveCalls(const Stmt *body,
                                                                 llvm::StringRef resolve) const {
  auto calls = std::vector<const CallExpr *>();
  for (const Expr *expr : AstWalker::allExprsIn(body)) {
    const auto *call = this->resolveCall(expr, resolve);
    if (call != nullptr) {
      calls.push_back(call);
    }
  }

  return calls;
}

const Type *LibraryAsyncRegistry::joinedValue(const Type *found, const Type *type) const {
  if (type == nullptr || type->isError() || type->isVoid() || type->isUndefined()) {
    return nullptr;
  }
  if (found == nullptr) {
    return type;
  }
  const auto *common = this->state.types.literals.unionUnify(found, type);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

LibraryAsyncRegistry::ResultExecutorShaped
LibraryAsyncRegistry::checkExecutorShape(const Expr *executor) const {
  auto *function = dyn_cast<FunctionExpr>(executor);
  if (function != nullptr && function->fn->params.size() > 1) {
    auto message = ErrorTextExecutor::rejects();

    return err(LibraryAsyncRegistryExecutorShapeError{
        .code = LibraryAsyncRegistryExecutorShapeErrorCode::REJECT_PARAMETER,
        .message = message,
    });
  }

  return ok(executor);
}

Signature LibraryAsyncRegistry::thenSignature(const Expr *callback, const MemberExpr *member,
                                              const Type *expected) const {
  auto signature = Signature{
      .params = {},
      .minArgs = 0,
      .ret = nullptr,
      .voidResult = false,
      .pad = false,
  };
  const Type *value = member->object->type->promised();
  if (value != nullptr && !value->isVoid()) {
    signature.params = {value};
  }
  const Type *wanted = this->expectedPromised(expected);
  if (wanted != nullptr && isa<FunctionExpr>(callback) &&
      this->state.inference.destinations.isLiteralShaped(callback)) {
    signature.ret = wanted;
  }

  return signature;
}

const Type *LibraryAsyncRegistry::awaited(const Type *type) const {
  return type->isPromise() ? type->promised() : type;
}

LibraryAsyncRegistry::ResultEmptyListTyped
LibraryAsyncRegistry::checkEmptyList(const Type *wantedElement,
                                     llvm::StringRef functionName) const {
  if (wantedElement == nullptr) {
    auto message = ErrorTextPromiseList::noType(functionName);

    return err(LibraryAsyncRegistryEmptyListError{
        .code = LibraryAsyncRegistryEmptyListErrorCode::ELEMENT_TYPE_UNKNOWN,
        .message = message,
    });
  }

  return ok(wantedElement);
}

LibraryAsyncRegistry::ResultValueIsNotVoid
LibraryAsyncRegistry::checkNotVoid(const Type *type, llvm::StringRef functionName) const {
  if (type->isVoid()) {
    auto message = ErrorTextPromiseVoid::given(functionName);

    return err(LibraryAsyncRegistryNotVoidError{
        .code = LibraryAsyncRegistryNotVoidErrorCode::VOID_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

LibraryAsyncRegistry::ResultPlainValueType
LibraryAsyncRegistry::checkPlainValue(const Type *type, llvm::StringRef functionName) const {
  bool isMayBePromise = !type->isPromise() && this->state.library.async.awaitedType(type) != type;
  if (isMayBePromise) {
    auto message = ErrorTextPromiseList::mayBe(functionName, type->name());

    return err(LibraryAsyncRegistryPlainValueError{
        .code = LibraryAsyncRegistryPlainValueErrorCode::MAY_BE_PROMISE,
        .message = message,
    });
  }

  return ok(type);
}

LibraryAsyncRegistry::ResultListOfPromises
LibraryAsyncRegistry::checkPromiseArray(const Expr *arg, llvm::StringRef functionName) const {
  const Type *type = arg->type;
  if (!type->isArray() && !type->isTuple()) {
    const auto *name = dyn_cast<NameExpr>(arg);
    bool isIterable = type->isIterator() || type->isDict();
    std::string values = name != nullptr ? name->name.str() : ErrorTextPromiseList::valuesLabel();
    auto message = ErrorTextPromiseList::notArray(functionName, type->name(), isIterable, values);

    return err(LibraryAsyncRegistryPromiseArrayError{
        .code = LibraryAsyncRegistryPromiseArrayErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

bool LibraryAsyncRegistry::isSpreadOrHole(const ArrayLitExpr::Element &element) const {
  return element.spread || element.hole;
}

const Type *LibraryAsyncRegistry::unifiedType(llvm::ArrayRef<const Type *> types) const {
  const Type *common = types.front();
  for (const Type *type : types) {
    if (common != nullptr) {
      common = this->state.types.relations.unify(common, type);
    }
  }

  return common;
}

LibraryAsyncRegistry::ResultTypeArgsAbsent
LibraryAsyncRegistry::checkStaticTypeArgs(const MemberExpr *member) const {
  auto method = PromiseStaticTable::find(member->name);
  if (!member->typeArgs.empty() && method != PromiseStatic::WITH_RESOLVERS) {
    auto message = ErrorTextStaticTypeArgs::resolvers();

    return err(LibraryAsyncRegistryStaticTypeArgsError{
        .code = LibraryAsyncRegistryStaticTypeArgsErrorCode::TYPE_ARGUMENTS_UNUSED,
        .message = message,
    });
  }

  return ok(member);
}

LibraryAsyncRegistry::ResultOneTypeArgOnly
LibraryAsyncRegistry::checkResolverTypeArgs(const MemberExpr *member) const {
  if (member->typeArgs.size() > 1) {
    auto message = ErrorTextResolverValue::typeArgs();

    return err(LibraryAsyncRegistryResolverTypeArgsError{
        .code = LibraryAsyncRegistryResolverTypeArgsErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(member);
}

LibraryAsyncRegistry::ResultValueTypeKnown
LibraryAsyncRegistry::checkResolverValue(const Type *value) const {
  if (value == nullptr) {
    auto message = ErrorTextResolverValue::unknown();

    return err(LibraryAsyncRegistryResolverValueError{
        .code = LibraryAsyncRegistryResolverValueErrorCode::VALUE_TYPE_UNKNOWN,
        .message = message,
    });
  }
  if (value->isError()) {
    return err(LibraryAsyncRegistryResolverValueError{
        .code = LibraryAsyncRegistryResolverValueErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  return ok(value);
}

LibraryAsyncRegistry::ResultRaceTypeJoined
LibraryAsyncRegistry::checkRaceJoin(const Type *element, const Type *type,
                                    llvm::StringRef functionName) const {
  const auto *common = this->state.types.literals.unionUnify(element, type);
  if (common == nullptr || common->isError()) {
    auto message = ErrorTextRaceJoin::differ(functionName, element->name(), type->name());

    return err(LibraryAsyncRegistryRaceJoinError{
        .code = LibraryAsyncRegistryRaceJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

LibraryAsyncRegistry::ResultTupleHasValues
LibraryAsyncRegistry::checkAllValues(llvm::ArrayRef<const Type *> tuple) const {
  auto isVoid = std::bind_front(&LibraryAsyncRegistry::isVoidValue, this);
  if (llvm::any_of(tuple, isVoid)) {
    auto message = ErrorTextAllValues::voidValue();

    return err(LibraryAsyncRegistryAllValuesError{
        .code = LibraryAsyncRegistryAllValuesErrorCode::VOID_PROMISE,
        .message = message,
    });
  }
  const auto *values = Type::tuple(tuple);

  return ok(values);
}

bool LibraryAsyncRegistry::isVoidValue(const Type *type) const {
  return type->isVoid();
}

const Type *LibraryAsyncRegistry::wantedValue(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }
  const Type *wanted = expected->nonOptional()->promised();

  return wanted->isNever() ? nullptr : wanted;
}

const Type *LibraryAsyncRegistry::combinatorElement(const Type *wanted, bool isRace) const {
  if (wanted != nullptr && !isRace && wanted->isArray()) {
    return wanted->inner();
  }
  if (wanted != nullptr && isRace) {
    return wanted;
  }

  return nullptr;
}

const Type *LibraryAsyncRegistry::settledElement(const Type *element,
                                                 const Type *wantedElement) const {
  if (element == nullptr || !element->isNever()) {
    return element;
  }

  return wantedElement != nullptr ? wantedElement : Type::voidTy();
}

LibraryAsyncRegistry::ResultRaceTypeJoined
LibraryAsyncRegistry::checkRaceElement(const PromiseList &list,
                                       llvm::StringRef functionName) const {
  if (list.tuple.empty()) {
    return ok(list.element);
  }
  const Type *element = list.tuple.front();
  for (const Type *part : list.tuple) {
    auto joined = this->checkRaceJoin(element, part, functionName);
    if (joined.isErr()) {
      return err(joined.error());
    }
    element = joined.value();
  }

  return ok(element);
}

LibraryAsyncRegistry::ResultTaskCallbackOk
LibraryAsyncRegistry::checkTaskCallbackType(const Type *type, const Type *task, const Expr *adapted,
                                            llvm::StringRef functionName) const {
  if (!type->isFunction()) {
    auto message = ErrorTextTaskCallback::notFunction(functionName, type->name());

    return err(LibraryAsyncRegistryTaskCallbackError{
        .code = LibraryAsyncRegistryTaskCallbackErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (type != task && adapted == nullptr) {
    auto message = ErrorTextTaskCallback::nothing(functionName, type->name());

    return err(LibraryAsyncRegistryTaskCallbackError{
        .code = LibraryAsyncRegistryTaskCallbackErrorCode::CALLED_WITH_NOTHING,
        .message = message,
    });
  }

  return ok(type);
}

bool LibraryAsyncRegistry::isRearming(const Expr *callback) const {
  const auto *current = this->state.walk.functions.getCurrentDecl();
  if (current == nullptr || current->sym == nullptr) {
    return false;
  }
  const auto *bare = ExprParts::withoutImplicitCasts(callback);
  const auto *member = dyn_cast<MemberExpr>(bare);
  if (member != nullptr) {
    return member->method() == current->sym;
  }
  const auto *functionExpr = dyn_cast<FunctionExpr>(bare);
  if (functionExpr == nullptr || functionExpr->fn->body == nullptr ||
      functionExpr->fn->body->body.size() != 1) {
    return false;
  }
  const Stmt *only = functionExpr->fn->body->body.front();
  const auto *value = this->soleStatementValue(only);
  const auto *call = dyn_cast_or_null<CallExpr>(value);

  return call != nullptr && call->method() == current->sym;
}

const Expr *LibraryAsyncRegistry::soleStatementValue(const Stmt *statement) const {
  const auto *exprStmt = dyn_cast<ExprStmt>(statement);
  if (exprStmt != nullptr) {
    return exprStmt->expr;
  }
  const auto *returnStmt = dyn_cast<ReturnStmt>(statement);
  if (returnStmt != nullptr) {
    return returnStmt->value;
  }

  return nullptr;
}

bool LibraryAsyncRegistry::isTimerName(const NameExpr *name) const {
  static const GlobalName names[] = {GlobalName::SET_TIMEOUT,    GlobalName::SET_INTERVAL,
                                     GlobalName::SET_IMMEDIATE,  GlobalName::CLEAR_TIMEOUT,
                                     GlobalName::CLEAR_INTERVAL, GlobalName::CLEAR_IMMEDIATE,
                                     GlobalName::QUEUE_MICROTASK};
  for (GlobalName timer : names) {
    if (this->state.isGlobalName(name, timer)) {
      return true;
    }
  }

  return false;
}

Builtin LibraryAsyncRegistry::timerMethodBuiltin(TimerMethod method) const {
  switch (method) {
    case TimerMethod::REF:
      return Builtin::TimerRef;
    case TimerMethod::UNREF:
      return Builtin::TimerUnref;
    case TimerMethod::HAS_REF:
      return Builtin::TimerHasRef;
    case TimerMethod::REFRESH:
      return Builtin::TimerRefresh;
  }

  return Builtin::TimerRef;
}

const Type *LibraryAsyncRegistry::promisedHint(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isPromise()) {
    return nullptr;
  }
  const Type *wanted = expected->nonOptional()->promised();
  bool isEmpty = wanted->isVoid() || wanted->isNever();

  return isEmpty ? nullptr : wanted;
}

LibraryAsyncRegistry::ResultMethodAccepted
LibraryAsyncRegistry::checkNotResultMethod(const MemberExpr *member) const {
  const Type *value = member->object->type->promised();
  const auto *resultMethod = ResultMethodTable::find(member->name);
  if (value->isResult() && resultMethod != nullptr) {
    const auto *object = dyn_cast<NameExpr>(member->object);
    std::string promise = object != nullptr ? object->name.str() : kPromisePlaceholder.str();
    auto message = ErrorTextPromiseMethod::onResult(member->name, promise);

    return err(LibraryAsyncRegistryCheckPromiseMethodError{
        .code = LibraryAsyncRegistryCheckPromiseMethodErrorCode::RESULT_METHOD,
        .message = message,
    });
  }

  return ok();
}

LibraryAsyncRegistry::ResultMethodAccepted
LibraryAsyncRegistry::checkIsThen(const CallExpr *call, const MemberExpr *member) const {
  auto method = PromiseMethodTable::find(member->name);
  if (method == PromiseMethod::CATCH) {
    auto message = ErrorTextPromiseMethod::noCatch();

    return err(LibraryAsyncRegistryCheckPromiseMethodError{
        .code = LibraryAsyncRegistryCheckPromiseMethodErrorCode::NO_CATCH,
        .message = message,
    });
  }
  if (method == PromiseMethod::FINALLY) {
    auto message = ErrorTextPromiseMethod::noFinally();

    return err(LibraryAsyncRegistryCheckPromiseMethodError{
        .code = LibraryAsyncRegistryCheckPromiseMethodErrorCode::NO_FINALLY,
        .message = message,
    });
  }
  if (method == PromiseMethod::THEN && call->args.size() == 2) {
    auto message = ErrorTextPromiseMethod::thenTwo();

    return err(LibraryAsyncRegistryCheckPromiseMethodError{
        .code = LibraryAsyncRegistryCheckPromiseMethodErrorCode::THEN_TWO_FUNCTIONS,
        .message = message,
    });
  }
  if (method != PromiseMethod::THEN) {
    auto message = ErrorTextPromiseMethod::noMethod(member->name, member->object->type->name());

    return err(LibraryAsyncRegistryCheckPromiseMethodError{
        .code = LibraryAsyncRegistryCheckPromiseMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok();
}

}
