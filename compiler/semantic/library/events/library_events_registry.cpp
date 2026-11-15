#include "compiler/semantic/library/events/library_events_registry.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "compiler/program/library/events/emitter_event_table/emitter_event_table.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/program/library/events/event_target_method_table/event_target_method_table.h"
#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/process/process_qualified_name_table/process_qualified_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_queries/type_queries.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int kReceiverBit = 16;
constexpr int kDedupeBit = 32;

}

LibraryEventsRegistry::LibraryEventsRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryEventsRegistry::classifyListener(const ClassifyListener &task) {
  int bits = int(task.bits->value);
  bool isClassified = this->classifyArg(task.arg, task.emitter, bits, task.use);
  task.bits->value = uint64_t(bits);

  return this->state.walk.tasks.pushVerdict(isClassified);
}

void LibraryEventsRegistry::noteErrorsHandled(const NoteErrorsHandled &task) {
  const Expr *emitter = task.expr;
  if (emitter->type != nullptr) {
    const auto *events = this->state.library.emitters.eventMapOf(emitter->type);
    if (events != nullptr) {
      this->state.library.events.markErrorListened(events);
    }
  }
  auto key = this->keyOf(emitter);
  this->state.walk.flow.listenForErrors(key);
}

void LibraryEventsRegistry::reportUnlistenedErrors() {
  auto destination = this->state.diagnostics.getCurrentDestination();
  bool isDiscarded = destination == DiagnosticRoute::DISCARD;
  auto &store = this->state.library.events;
  for (const auto &[key, loc] : this->state.walk.flow.takePendingErrors()) {
    if (!isDiscarded && !store.isErrorSubscriptionReported(loc)) {
      store.markErrorSubscriptionReported(loc);
      auto path = this->emitterPath(key);
      auto message = ErrorTextUnlistened::mayEmit(path);
      this->state.diagnostics.error(loc, message);
    }
  }
}

void LibraryEventsRegistry::reportEmitterMethodValue(const ReportEmitterMethodValue &task) {
  auto message = ErrorTextEmitted::methodVal(task.member->name);
  this->state.diagnostics.error(task.member->loc(), message);
}

void LibraryEventsRegistry::finishSentEmits() {
  auto &store = this->state.library.events;
  store.markSentEmitsFinished();
  for (EventSite *site : llvm::reverse(store.getSites())) {
    for (EventRecord &record : llvm::reverse(site->events)) {
      record.state = EventState::DONE;
      for (const SentEmit &emit : llvm::reverse(record.sent)) {
        this->state.walk.tasks.pushTask(DiscardChecked{});
        this->pushFinishEmit(emit, record.args);
      }
      record.sent.clear();
    }
  }
}

void LibraryEventsRegistry::pushFinishEmit(const SentEmit &emit, const Type *args) {
  CallExpr *call = emit.call;
  auto &tasks = this->state.walk.tasks;
  auto emitName = EmitterMemberTable::getName(EmitterMember::EMIT);
  tasks.pushTask(CallLibraryMethod{
      .call = call,
      .member = emit.member,
      .classDecl = emit.cls,
      .name = emitName,
      .bound = {args},
      .given = std::vector<bool>(call->args.size(), true),
      .receiver = emit.callReceiver,
  });
  if (call->args.size() < 2) {
    return;
  }

  return tasks.pushTask(
      CoerceSlot{.slot = &call->args[1], .to = args->fields()[0].type, .what = ""});
}

void LibraryEventsRegistry::instantiateEmitterParent(const InstantiateEmitterParent &task) {
  auto &tasks = this->state.walk.tasks;
  auto *events = this->classEventMap(task.classDecl);
  tasks.pushTask(AfterEmitterParent{.ref = task.ref});

  return tasks.pushTask(
      InstantiateClassType{.classDecl = task.emitter, .args = {events}, .loc = task.ref->loc});
}

void LibraryEventsRegistry::afterEmitterParent(const AfterEmitterParent &task) {
  task.ref->resolved = this->state.walk.tasks.takeAnswer();
}

void LibraryEventsRegistry::checkEmitterNew(const CheckEmitterNew &task) {
  NewExpr *newExpr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  if (newExpr->typeArgs.size() > 1) {
    return tasks.pushChecked(nullptr);
  }
  auto draft = AfterEmitterEvents{
      .newExpr = newExpr,
      .classDecl = task.classDecl,
      .expected = task.expected,
  };
  if (newExpr->typeArgs.size() == 1) {
    tasks.pushTask(draft);

    return tasks.pushTask(
        ResolveTypeAnswer{.ref = &newExpr->typeArgs[0], .use = TypeUse::STANDALONE});
  }
  const Type *wanted = task.expected != nullptr ? task.expected->nonOptional() : nullptr;
  const auto *wantedClass = wanted != nullptr ? wanted->classDecl() : nullptr;
  bool isWantedEmitter = wantedClass != nullptr && wantedClass->generic() == task.classDecl;
  const Type *events =
      isWantedEmitter ? wantedClass->typeArgs[0] : this->newEventMap(newExpr->loc());
  tasks.pushAnswer(events);

  return this->afterEmitterEvents(draft);
}

void LibraryEventsRegistry::afterEmitterEvents(const AfterEmitterEvents &task) {
  NewExpr *newExpr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  const Type *events = tasks.takeAnswer();
  if (events->isError()) {
    return this->failEmitterNew(newExpr);
  }
  tasks.pushTask(AfterEmitterInstance{.newExpr = newExpr, .expected = task.expected});

  return tasks.pushTask(
      InstantiateClassType{.classDecl = task.classDecl, .args = {events}, .loc = newExpr->loc()});
}

void LibraryEventsRegistry::afterEmitterInstance(const AfterEmitterInstance &task) {
  NewExpr *newExpr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  const Type *instance = tasks.takeAnswer();
  auto *classDecl = instance->isError() ? nullptr : const_cast<ClassDecl *>(instance->classDecl());
  if (classDecl == nullptr) {
    return this->failEmitterNew(newExpr);
  }
  newExpr->typeArgs.clear();

  return tasks.pushTask(
      CheckClassNew{.newExpr = newExpr, .classDecl = classDecl, .expected = task.expected});
}

void LibraryEventsRegistry::failEmitterNew(NewExpr *newExpr) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(newExpr);
  for (Expr *&arg : llvm::reverse(newExpr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryEventsRegistry::walkLooseArgs(CallExpr *call, size_t from) {
  for (size_t index = call->args.size(); index > from; --index) {
    this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
  }
}

void LibraryEventsRegistry::finishLoosely(CallExpr *call, size_t from) {
  this->state.walk.tasks.pushChecked(call);

  return this->walkLooseArgs(call, from);
}

void LibraryEventsRegistry::createdType(const CreatedType &task) {
  const NewExpr *create = task.create;
  auto &tasks = this->state.walk.tasks;
  auto *classDecl = llvm::dyn_cast_or_null<ClassDecl>(
      create->member.empty() ? this->state.findTypeDeclaration(create->name) : nullptr);
  if (classDecl == nullptr) {
    return tasks.pushAnswer(nullptr);
  }
  if (!classDecl->isGeneric()) {
    return tasks.pushAnswer(classDecl->type);
  }
  tasks.pushTask(AfterCreatedArgs{.create = create, .classDecl = classDecl});
  for (const TypeRef &ref : llvm::reverse(create->typeArgs)) {
    tasks.pushTask(ResolveTypeAnswer{.ref = &ref, .use = TypeUse::STANDALONE});
  }
}

void LibraryEventsRegistry::afterCreatedArgs(const AfterCreatedArgs &task) {
  const NewExpr *create = task.create;
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  auto args = tasks.takeAnswers(create->typeArgs.size());
  auto isError = [](const Type *type) { return type->isError(); };
  if (llvm::any_of(args, isError)) {
    return tasks.pushAnswer(nullptr);
  }
  bool isEmitter = classDecl->library() &&
                   LibraryClassTable::isName(classDecl->name, LibraryClass::EVENT_EMITTER);
  if (args.empty() && isEmitter) {
    args.push_back(this->newEventMap(create->loc()));
  }
  if (args.empty()) {
    return tasks.pushAnswer(nullptr);
  }
  tasks.pushTask(AfterCreatedInstance{});

  return tasks.pushTask(
      InstantiateClassType{.classDecl = classDecl, .args = std::move(args), .loc = create->loc()});
}

void LibraryEventsRegistry::afterCreatedInstance() {
  auto &tasks = this->state.walk.tasks;
  const Type *instance = tasks.takeAnswer();

  return tasks.pushAnswer(instance->isError() ? nullptr : instance);
}

void LibraryEventsRegistry::eventArgsOf(const EventArgsOf &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterEventName{.events = task.events, .name = task.name, .method = task.method});

  return tasks.pushTask(CheckArgSlot{.slot = task.name, .to = Type::string()});
}

void LibraryEventsRegistry::afterEventName(const AfterEventName &task) {
  const Type *events = task.events;
  const Expr *name = *task.name;
  auto &tasks = this->state.walk.tasks;
  if (events->isRecord()) {
    return tasks.pushAnswer(name->type->isError() ? nullptr : events->valueType());
  }
  auto key = this->state.code.constants.constantKey(name);
  if (!key.has_value() && !name->type->isError()) {
    auto message = ErrorTextEventName::unknown(task.method);
    this->state.diagnostics.error(name->loc(), message);
  }
  if (!key.has_value()) {
    return tasks.pushAnswer(nullptr);
  }
  auto &emitters = this->state.library.emitters;
  if (emitters.isServiceEvent(*key)) {
    return tasks.pushAnswer(emitters.serviceEventArgs());
  }
  auto *site = this->state.library.events.findSiteOfMap(events);
  if (site != nullptr) {
    return tasks.pushTask(InferredEventArgs{.site = site, .key = *key, .loc = name->loc()});
  }
  const auto *field = events->field(*key);
  if (field != nullptr) {
    return tasks.pushAnswer(field->type);
  }
  auto nearest = emitters.nearestEvent(events, *key);
  auto message = ErrorTextEventName::noEvent(*key, events, nearest);
  this->state.diagnostics.error(name->loc(), message);

  return tasks.pushAnswer(nullptr);
}

void LibraryEventsRegistry::inferredEventArgs(const InferredEventArgs &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInferredEvent{.site = task.site, .key = task.key, .loc = task.loc});

  return tasks.pushTask(CompleteEvent{.site = task.site, .name = task.key});
}

void LibraryEventsRegistry::afterInferredEvent(const AfterInferredEvent &task) {
  auto &tasks = this->state.walk.tasks;
  const EventRecord *record = this->findEvent(task.site, task.key);
  if (record->args != nullptr) {
    return tasks.pushAnswer(record->args);
  }
  if (record->state == EventState::COMPLETING) {
    auto message = ErrorTextEventName::circular(task.key);
    this->state.diagnostics.error(task.loc, message);

    return tasks.pushAnswer(nullptr);
  }
  if (!record->untyped.empty()) {
    auto message = ErrorTextEventName::untyped(task.key, record->untyped);
    this->state.diagnostics.error(task.loc, message);

    return tasks.pushAnswer(nullptr);
  }
  tasks.pushTask(ReportNotEmitted{.site = task.site, .key = task.key, .loc = task.loc});

  return tasks.pushTask(CompleteSite{.site = task.site});
}

void LibraryEventsRegistry::reportNotEmitted(const ReportNotEmitted &task) {
  auto nearest = this->state.library.emitters.nearestEvent(task.site->map, task.key);
  auto message = ErrorTextEventName::notEmitted(task.key, task.site->map, nearest);
  this->state.diagnostics.error(task.loc, message);

  return this->state.walk.tasks.pushAnswer(nullptr);
}

void LibraryEventsRegistry::findEventArgs(const FindEventArgs &task) {
  auto &tasks = this->state.walk.tasks;
  auto *site = this->state.library.events.findSiteOfMap(task.events);
  if (site == nullptr) {
    const auto *field = task.events->field(task.name);

    return tasks.pushAnswer(field != nullptr ? field->type : nullptr);
  }
  tasks.pushTask(AnswerEventArgs{.site = site, .name = task.name});

  return tasks.pushTask(CompleteEvent{.site = site, .name = task.name});
}

void LibraryEventsRegistry::answerEventArgs(const AnswerEventArgs &task) {
  const EventRecord *record = this->findEvent(task.site, task.name);

  return this->state.walk.tasks.pushAnswer(record->args);
}

void LibraryEventsRegistry::completeEvent(const CompleteEvent &task) {
  EventSite *site = task.site;
  auto &tasks = this->state.walk.tasks;
  auto *record = this->findEvent(site, task.name);
  if (record != nullptr && record->state != EventState::COLLECTING) {
    return;
  }
  if (record == nullptr) {
    record = this->addEvent(site, task.name);
  }
  record->state = EventState::COMPLETING;
  auto functions = llvm::SmallPtrSet<const FunctionDecl *, 4>();
  llvm::SmallSet<size_t, 4> statements;
  auto untyped = std::string();
  auto trials = std::vector<CheckTask>();
  for (const EmitCandidate &candidate : this->getSiteCandidates(site)) {
    bool isUnchecked = candidate.event == record->name && candidate.call->type == nullptr;
    if (isUnchecked && untyped.empty()) {
      untyped = this->shortPlaceOf(candidate.call->loc());
    }
    if (isUnchecked) {
      this->addTrial(candidate, functions, statements, trials);
    }
  }
  tasks.pushTask(FinishCompleteEvent{.record = record, .untyped = untyped});
  for (CheckTask &trial : llvm::reverse(trials)) {
    tasks.pushTask(std::move(trial));
  }
}

void LibraryEventsRegistry::finishCompleteEvent(const FinishCompleteEvent &task) {
  EventRecord *record = task.record;
  if (record->args == nullptr) {
    record->untyped = task.untyped;
  }
  record->state = EventState::DONE;
}

void LibraryEventsRegistry::addTrial(const EmitCandidate &candidate,
                                     llvm::SmallPtrSetImpl<const FunctionDecl *> &functions,
                                     llvm::SmallSet<size_t, 4> &statements,
                                     std::vector<CheckTask> &trials) {
  if (candidate.function != nullptr) {
    bool isNewFunction = functions.insert(candidate.function).second;
    if (isNewFunction && this->canTryFunction(candidate.function)) {
      trials.push_back(TryEventFunction{.functionDecl = candidate.function});
    }
  }
  if (candidate.function == nullptr && this->canTryStatement(candidate)) {
    bool isNewStatement = statements.insert(candidate.index).second;
    Stmt *statement = candidate.module->file.items[candidate.index];
    if (isNewStatement && !this->isHoldingClassExpr(statement)) {
      trials.push_back(TryEventStatement{.statement = statement});
    }
  }
}

void LibraryEventsRegistry::completeSite(const CompleteSite &task) {
  EventSite *site = task.site;
  if (site->complete) {
    return;
  }
  site->complete = true;
  auto names = std::vector<std::string>();
  for (const EmitCandidate &candidate : this->getSiteCandidates(site)) {
    if (!llvm::is_contained(names, candidate.event)) {
      names.push_back(candidate.event);
    }
  }
  for (const std::string &name : llvm::reverse(names)) {
    this->state.walk.tasks.pushTask(CompleteEvent{.site = site, .name = name});
  }
}

void LibraryEventsRegistry::tryEventFunction(const TryEventFunction &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(LeaveReturnBody{});
  tasks.pushTask(BeginEventTrial{.functionDecl = task.functionDecl, .statement = nullptr});

  return tasks.pushTask(EnterReturnBody{.functionDecl = task.functionDecl});
}

void LibraryEventsRegistry::tryEventStatement(const TryEventStatement &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(LeaveLoadBody{});
  tasks.pushTask(BeginEventTrial{.functionDecl = nullptr, .statement = task.statement});

  return tasks.pushTask(EnterLoadBody{});
}

void LibraryEventsRegistry::beginEventTrial(const BeginEventTrial &task) {
  auto &tasks = this->state.walk.tasks;
  auto &functions = this->state.walk.functions;
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  if (task.statement != nullptr) {
    functions.enterTrial();
    AstCloner cloner(this->state.ast);
    Stmt *copy = cloner.copyStatement(task.statement);
    this->state.walk.scopes.openScope();
    tasks.pushTask(
        EndEventTrial{.outerReturns = nullptr, .outerYields = nullptr, .isStatement = true});

    return tasks.pushTask(WalkStmt{.stmt = copy});
  }
  auto end = EndEventTrial{
      .outerReturns = functions.getReturnCollector(),
      .outerYields = functions.getYieldCollector(),
      .isStatement = false,
  };
  CollectedReturns &collected = this->state.inference.returns.openCollection();
  functions.rememberReturnCollector(&collected.returns);
  functions.rememberYieldCollector(&collected.yields);
  functions.enterTrial();
  auto *copy = this->state.library.common.freshCopy(task.functionDecl);
  tasks.pushTask(end);

  return tasks.pushTask(WalkFunctionFromScratch{.functionDecl = copy});
}

void LibraryEventsRegistry::endEventTrial(const EndEventTrial &task) {
  auto &functions = this->state.walk.functions;
  if (task.isStatement) {
    this->state.walk.scopes.closeScope();
  }
  functions.leaveTrial();
  this->state.diagnostics.popList();
  if (task.isStatement) {
    return;
  }
  this->state.inference.returns.takeCollection();
  functions.rememberReturnCollector(task.outerReturns);
  functions.rememberYieldCollector(task.outerYields);
}

void LibraryEventsRegistry::checkEmitterCall(const CheckEmitterCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto kind = this->emitterMethod(member->name);
  bool isLibrary = this->state.library.declarations.isInsideLibrary(call->loc());
  auto methods = task.classDecl->findMethodsNamed(member->name);
  bool isOwnMethod = !methods.empty() && !methods[0]->cls()->library();
  const Type *receiver = member->object->type;
  const auto *events = this->state.library.emitters.eventMapOf(receiver);
  bool isSkipped = kind == EmitterMethodKind::None ||
                   (isLibrary && kind != EmitterMethodKind::Emit) || isOwnMethod ||
                   events == nullptr || (isLibrary && TypeQueries::mentionsParam(events));
  if (isSkipped) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextEmitted::typeArgs(member->name);
    this->state.diagnostics.error(member->loc(), message);
    member->typeArgs.clear();
  }
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto emitter = EmitterDraft{
      .call = call,
      .member = member,
      .classDecl = task.classDecl,
      .receiver = receiver,
      .events = events,
      .callReceiver = task.receiver,
  };

  return this->checkEmitterMethod(emitter, kind, isLibrary);
}

void LibraryEventsRegistry::checkEmitterMethod(const EmitterDraft &emitter, EmitterMethodKind kind,
                                               bool isLibrary) {
  switch (kind) {
    case EmitterMethodKind::Emit:
      return this->checkEmit(emitter, isLibrary);
    case EmitterMethodKind::Listen:
    case EmitterMethodKind::Remove:
      return this->startEmitterQuery(emitter, kind, 2, 2);
    case EmitterMethodKind::Listeners:
    case EmitterMethodKind::RawListeners:
      return this->startEmitterQuery(emitter, kind, 1, 1);
    case EmitterMethodKind::Count:
      return this->startEmitterQuery(emitter, kind, 1, 2);
    case EmitterMethodKind::RemoveAll:
      return this->startEmitterQuery(emitter, kind, 0, 1);
    case EmitterMethodKind::None:
      return this->state.walk.tasks.pushChecked(nullptr);
  }
}

void LibraryEventsRegistry::startEmitterQuery(const EmitterDraft &emitter, EmitterMethodKind kind,
                                              unsigned min, unsigned max) {
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(emitter.member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  if (call->args.empty()) {
    return this->finishRemoveAll(emitter);
  }
  tasks.pushTask(AfterQueryEvent{.emitter = emitter, .methodKind = kind});

  return tasks.pushTask(EventArgsOf{
      .events = emitter.events, .name = &call->args[0], .method = emitter.member->name});
}

void LibraryEventsRegistry::afterQueryEvent(const AfterQueryEvent &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  const Type *args = tasks.takeAnswer();
  bool isWalkingRest = task.methodKind == EmitterMethodKind::Listen ||
                       task.methodKind == EmitterMethodKind::Remove ||
                       task.methodKind == EmitterMethodKind::Count;
  if (args == nullptr && isWalkingRest) {
    return this->finishLoosely(call, 1);
  }
  if (args == nullptr) {
    return tasks.pushChecked(call);
  }
  auto event = this->eventLabel(emitter.events, call->args[0]);
  switch (task.methodKind) {
    case EmitterMethodKind::Listen:
    case EmitterMethodKind::Remove:
    case EmitterMethodKind::Count:
      return this->checkListenerArg(emitter, task.methodKind, args, event);
    case EmitterMethodKind::Listeners:
    case EmitterMethodKind::RawListeners:
      return this->finishListeners(emitter, task.methodKind, args);
    case EmitterMethodKind::RemoveAll:
      return this->finishRemoveAll(emitter);
    case EmitterMethodKind::Emit:
    case EmitterMethodKind::None:
      return tasks.pushChecked(call);
  }
}

void LibraryEventsRegistry::checkListenerArg(const EmitterDraft &emitter, EmitterMethodKind kind,
                                             const Type *args, const std::string &event) {
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() < 2) {
    return this->finishEmitterMethod(emitter, emitter.member->name, {}, false);
  }
  auto want = ListenerWantDraft{
      .args = args,
      .event = event,
      .receiver = emitter.receiver,
      .sync = this->state.library.emitters.listenerType(args, Type::voidTy()),
      .async = this->state.library.emitters.listenerType(args, Type::promise(Type::voidTy())),
  };
  tasks.pushTask(AfterListenerArg{
      .emitter = emitter,
      .methodKind = kind,
      .event = event,
      .original = call->args[1],
  });

  return this->pushListenerCheck(&call->args[1], want);
}

void LibraryEventsRegistry::afterListenerArg(const AfterListenerArg &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args[1]->type->isError()) {
    return tasks.pushChecked(call);
  }
  if (task.methodKind != EmitterMethodKind::Listen) {
    this->state.inventory.comparesFunctions = true;
    bool isTyping = task.methodKind == EmitterMethodKind::Remove;

    return this->finishEmitterMethod(emitter, emitter.member->name, {}, isTyping);
  }
  auto listener = this->listenerKindOf(task.original, call->args[1]);
  auto errorName = EmitterEventTable::getName(EmitterEvent::ERROR_EVENT);
  tasks.pushTask(
      AfterListenerErrors{.emitter = emitter, .event = task.event, .listenerBits = int(listener)});

  return tasks.pushTask(FindEventArgs{.events = emitter.events, .name = errorName.str()});
}

void LibraryEventsRegistry::afterListenerErrors(const AfterListenerErrors &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  const Type *errors = tasks.takeAnswer();
  this->noteListener(emitter.member, emitter.events, task.event, errors);
  int bits = task.listenerBits;
  bool isKept =
      this->classifyArg(call->args[1], emitter.member->object, bits, ListenerUse::EMITTER);
  if (!isKept) {
    return tasks.pushChecked(call);
  }
  auto *flags = this->state.ast.make<IntLitExpr>(call->loc(), uint64_t(bits));
  call->args.push_back(flags);
  tasks.pushTask(FinishEmitterCall{
      .emitter = emitter, .name = emitter.member->name, .bound = {}, .isTyping = true});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args.back(), .expected = Type::intTy()});
}

void LibraryEventsRegistry::finishListeners(const EmitterDraft &emitter, EmitterMethodKind kind,
                                            const Type *args) {
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  auto *raw =
      this->state.ast.make<BoolLitExpr>(call->loc(), kind == EmitterMethodKind::RawListeners);
  call->args.push_back(raw);
  auto listenersOf = LibraryMethodTable::getName(LibraryMethod::LISTENERS_OF);
  const auto *listener = this->state.library.emitters.listenerType(args, Type::voidTy());
  tasks.pushTask(SetCheckedType{.type = Type::array(listener)});
  tasks.pushTask(FinishEmitterCall{
      .emitter = emitter, .name = listenersOf, .bound = {args}, .isTyping = false});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args.back(), .expected = nullptr});
}

void LibraryEventsRegistry::finishRemoveAll(const EmitterDraft &emitter) {
  return this->finishEmitterMethod(emitter, emitter.member->name, {}, true);
}

void LibraryEventsRegistry::finishEmitterMethod(const EmitterDraft &emitter, llvm::StringRef name,
                                                std::vector<const Type *> bound, bool isTyping) {
  auto finish = FinishEmitterCall{
      .emitter = emitter,
      .name = name,
      .bound = std::move(bound),
      .isTyping = isTyping,
  };

  return this->finishEmitterCall(finish);
}

void LibraryEventsRegistry::finishEmitterCall(const FinishEmitterCall &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  if (task.isTyping) {
    tasks.pushTask(SetCheckedType{.type = emitter.receiver});
  }

  return tasks.pushTask(CallLibraryMethod{
      .call = call,
      .member = emitter.member,
      .classDecl = emitter.classDecl,
      .name = task.name,
      .bound = task.bound,
      .given = std::vector<bool>(call->args.size(), true),
      .receiver = emitter.callReceiver,
  });
}

void LibraryEventsRegistry::setCheckedType(const SetCheckedType &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  checked->type = task.type;

  return tasks.pushChecked(checked);
}

void LibraryEventsRegistry::checkEmit(const EmitterDraft &emitter, bool isLibrary) {
  CallExpr *call = emitter.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args.empty()) {
    auto message = ErrorTextEmitted::noName();
    this->state.diagnostics.error(emitter.member->loc(), message);

    return tasks.pushChecked(call);
  }
  auto *site = this->state.library.events.findSiteOfMap(emitter.events);
  if (site == nullptr) {
    tasks.pushTask(AfterEmitArgs{.emitter = emitter, .isLibrary = isLibrary});

    return tasks.pushTask(EventArgsOf{
        .events = emitter.events, .name = &call->args[0], .method = emitter.member->name});
  }
  tasks.pushTask(AfterSiteEmitKey{.emitter = emitter, .site = site, .isLibrary = isLibrary});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryEventsRegistry::afterEmitArgs(const AfterEmitArgs &task) {
  const EmitterDraft &emitter = task.emitter;
  const Type *args = this->state.walk.tasks.takeAnswer();
  auto event = this->eventLabel(emitter.events, emitter.call->args[0]);
  auto shape = EmitShapeDraft{.args = args, .event = event, .first = ""};

  return this->checkEmitShape(emitter, shape, task.isLibrary);
}

void LibraryEventsRegistry::afterSiteEmitKey(const AfterSiteEmitKey &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  EventSite *site = task.site;
  auto &tasks = this->state.walk.tasks;
  const Expr *name = call->args[0];
  auto key = this->state.code.constants.constantKey(name);
  if (!key.has_value() && !name->type->isError()) {
    auto message = ErrorTextEventName::unknown(emitter.member->name);
    this->state.diagnostics.error(name->loc(), message);
  }
  if (!key.has_value()) {
    return this->finishLoosely(call, 1);
  }
  auto emit = SentEmit{
      .call = call,
      .member = emitter.member,
      .cls = emitter.classDecl,
      .callReceiver = emitter.callReceiver,
  };
  const auto *record = this->findEvent(site, *key);
  auto &emitters = this->state.library.emitters;
  bool isService = emitters.isServiceEvent(*key);
  if (!isService && this->isOwnEmit(site, emit) && !this->isFrozen(record)) {
    return this->collectEmitterCall(emit, site, *key, task.isLibrary);
  }
  if (isService) {
    auto shape = EmitShapeDraft{.args = emitters.serviceEventArgs(), .event = *key, .first = ""};

    return this->checkEmitShape(emitter, shape, task.isLibrary);
  }
  tasks.pushTask(AfterEmitEvent{
      .emitter = emitter,
      .site = site,
      .key = *key,
      .loc = name->loc(),
      .isLibrary = task.isLibrary,
  });

  return tasks.pushTask(CompleteEvent{.site = site, .name = *key});
}

void LibraryEventsRegistry::afterEmitEvent(const AfterEmitEvent &task) {
  EventSite *site = task.site;
  const EventRecord *record = this->findEvent(site, task.key);
  if (record->args != nullptr) {
    auto shape = EmitShapeDraft{.args = record->args, .event = task.key, .first = record->where};

    return this->checkEmitShape(task.emitter, shape, task.isLibrary);
  }
  std::string owner =
      site->owner != nullptr ? llvm::sys::path::filename(site->owner->path).str() : "its module";
  auto message = ErrorTextEmitted::foreign(task.key, owner);
  if (record->state == EventState::COMPLETING) {
    message = ErrorTextEventName::circular(task.key);
  } else if (!record->untyped.empty()) {
    message = ErrorTextEventName::untyped(task.key, record->untyped);
  }
  this->state.diagnostics.error(task.loc, message);
  auto shape = EmitShapeDraft{.args = nullptr, .event = task.key, .first = ""};

  return this->checkEmitShape(task.emitter, shape, task.isLibrary);
}

void LibraryEventsRegistry::checkEmitShape(const EmitterDraft &emitter, const EmitShapeDraft &shape,
                                           bool isLibrary) {
  CallExpr *call = emitter.call;
  MemberExpr *member = emitter.member;
  auto &tasks = this->state.walk.tasks;
  if (shape.args == nullptr) {
    return this->finishLoosely(call, 1);
  }
  size_t count = call->args.size() - 1;
  if (count > 1) {
    auto message = ErrorTextEmitted::oneValue(shape.event);
    this->state.diagnostics.error(call->args[2]->loc(), message);

    return this->finishLoosely(call, 1);
  }
  size_t min = shape.args->tupleMin();
  size_t max = shape.args->tupleRest() != nullptr ? ~size_t(0) : shape.args->fields().size();
  if (count < min || count > max) {
    std::string message =
        !shape.first.empty()
            ? ErrorTextEmitted::countAt(shape.event, shape.args->name(), shape.first, count)
            : ErrorTextEmitted::arity(shape.event, shape.args->name(), min, max, count);
    this->state.diagnostics.error(member->loc(), message);

    return this->finishLoosely(call, 1);
  }
  if (EmitterEventTable::find(shape.event) == EmitterEvent::ERROR_EVENT && !isLibrary) {
    this->state.library.events.addErrorEmit(emitter.events, member->loc());
  }
  auto given = std::vector<bool>(call->args.size(), false);
  given[0] = true;
  if (shape.first.empty()) {
    return this->finishEmitShape(emitter, shape, given);
  }

  return tasks.pushTask(
      NextEmitValue{.emitter = emitter, .shape = shape, .given = std::move(given), .index = 1});
}

void LibraryEventsRegistry::nextEmitValue(const NextEmitValue &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  size_t index = task.index;
  bool isDone = index >= call->args.size() || index - 1 >= task.shape.args->fields().size();
  if (isDone) {
    return this->finishEmitShape(emitter, task.shape, task.given);
  }
  const Type *wanted = task.shape.args->fields()[index - 1].type;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterEmitValue{
      .emitter = emitter,
      .shape = task.shape,
      .given = task.given,
      .index = index,
  });

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[index], .expected = wanted});
}

void LibraryEventsRegistry::afterEmitValue(const AfterEmitValue &task) {
  const EmitterDraft &emitter = task.emitter;
  CallExpr *call = emitter.call;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  const Type *wanted = task.shape.args->fields()[index - 1].type;
  const Type *type = call->args[index]->type;
  if (!type->isError() && !this->state.types.relations.isImplicitlyConvertible(type, wanted)) {
    auto message = ErrorTextEmitted::typeAt(task.shape.event, task.shape.args->name(),
                                            task.shape.first, type->name());
    this->state.diagnostics.error(call->args[index]->loc(), message);

    return this->finishLoosely(call, index + 1);
  }
  auto given = task.given;
  given[index] = true;
  tasks.pushTask(NextEmitValue{
      .emitter = emitter,
      .shape = task.shape,
      .given = std::move(given),
      .index = index + 1,
  });

  return tasks.pushTask(CoerceSlot{.slot = &call->args[index], .to = wanted, .what = ""});
}

void LibraryEventsRegistry::finishEmitShape(const EmitterDraft &emitter,
                                            const EmitShapeDraft &shape,
                                            const std::vector<bool> &given) {
  auto emitName = EmitterMemberTable::getName(EmitterMember::EMIT);

  return this->state.walk.tasks.pushTask(CallLibraryMethod{
      .call = emitter.call,
      .member = emitter.member,
      .classDecl = emitter.classDecl,
      .name = emitName,
      .bound = {shape.args},
      .given = given,
      .receiver = emitter.callReceiver,
  });
}

void LibraryEventsRegistry::collectEmitterCall(const SentEmit &emit, EventSite *site,
                                               const std::string &event, bool isLibrary) {
  CallExpr *call = emit.call;
  MemberExpr *member = emit.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::boolTy();
  size_t count = call->args.size() - 1;
  if (count > 1) {
    auto message = ErrorTextEmitted::oneValue(event);
    this->state.diagnostics.error(call->args[2]->loc(), message);

    return this->finishLoosely(call, 1);
  }
  auto *record = this->findEvent(site, event);
  if (record == nullptr) {
    record = this->addEvent(site, event);
  }
  const Type *before = record->args;
  if (before != nullptr && before->fields().size() != count) {
    auto message = ErrorTextEmitted::countAt(event, before->name(), record->where, count);
    this->state.diagnostics.error(member->loc(), message);

    return this->finishLoosely(call, 1);
  }
  auto collected = AfterEmittedValue{
      .emit = emit,
      .site = site,
      .record = record,
      .event = event,
      .isLibrary = isLibrary,
  };
  if (count == 0) {
    return this->finishCollectedEmit(collected, {});
  }
  const Type *wanted = before != nullptr ? before->fields()[0].type : nullptr;
  tasks.pushTask(std::move(collected));

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = wanted});
}

void LibraryEventsRegistry::afterEmittedValue(const AfterEmittedValue &task) {
  CallExpr *call = task.emit.call;
  const EventRecord *record = task.record;
  const Type *before = record->args;
  const Type *wanted = before != nullptr ? before->fields()[0].type : nullptr;
  const Type *type = call->args[1]->type;
  if (type->isVoid()) {
    auto message = ErrorTextEmitted::voidValue(task.event);
    this->state.diagnostics.error(call->args[1]->loc(), message);

    return this->state.walk.tasks.pushChecked(call);
  }
  if (type->isError() || TypeQueries::mentionsParam(type)) {
    return this->state.walk.tasks.pushChecked(call);
  }
  const Type *found = type->isLiteral() ? type->literalBase() : type;
  bool isFitting =
      wanted != nullptr && this->state.types.relations.isImplicitlyConvertible(type, wanted);
  const Type *wider =
      wanted != nullptr && !isFitting ? this->state.types.relations.unify(wanted, found) : nullptr;
  const Type *notFitting = wanted != nullptr ? wider : found;
  const Type *element = isFitting ? wanted : notFitting;
  if (element == nullptr) {
    auto message =
        ErrorTextEmitted::typeAt(task.event, before->name(), record->where, type->name());
    this->state.diagnostics.error(call->args[1]->loc(), message);

    return this->state.walk.tasks.pushChecked(call);
  }

  return this->finishCollectedEmit(task, {element});
}

void LibraryEventsRegistry::finishCollectedEmit(const AfterEmittedValue &collected,
                                                const std::vector<const Type *> &elements) {
  const SentEmit &emit = collected.emit;
  CallExpr *call = emit.call;
  EventRecord *record = collected.record;
  auto &tasks = this->state.walk.tasks;
  auto &store = this->state.library.events;
  const auto *args = Type::tuple(elements);
  if (args != record->args) {
    this->rememberEventArgs(collected.site, record, args, call->args[0]->loc());
  }
  if (this->state.walk.functions.isInsideTrial()) {
    return tasks.pushChecked(call);
  }
  bool isErrorEvent = EmitterEventTable::find(collected.event) == EmitterEvent::ERROR_EVENT;
  if (isErrorEvent && !collected.isLibrary) {
    store.addErrorEmit(collected.site->map, emit.member->loc());
  }
  if (store.hasFinishedSentEmits()) {
    return this->pushFinishEmit(emit, args);
  }
  record->sent.push_back(emit);

  return tasks.pushChecked(call);
}

void LibraryEventsRegistry::pushListenerCheck(Expr **slot, const ListenerWantDraft &want) {
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(*slot);
  if (functionExpr == nullptr) {
    tasks.pushTask(AfterListenerValue{.slot = slot, .want = want});

    return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
  }
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isExplicitThis = !functionDecl->params.empty() &&
                        KeywordTable::find(functionDecl->params[0].name) == Keyword::THIS &&
                        functionDecl->params[0].type.has_value();
  if (isExplicitThis) {
    tasks.pushTask(AfterListenerThis{.slot = slot, .want = want});

    return tasks.pushTask(
        ResolveTypeAnswer{.ref = &*functionDecl->params[0].type, .use = TypeUse::STANDALONE});
  }
  const Type *wanted = functionDecl->isAsync ? want.async : want.sync;
  auto signature = this->signatureOf(wanted);
  signature.voidResult = !functionDecl->isAsync;

  return this->pushListenerLiteral(slot, functionExpr, wanted, std::move(signature));
}

void LibraryEventsRegistry::pushListenerLiteral(Expr **slot, FunctionExpr *functionExpr,
                                                const Type *wanted, Signature signature) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishListenerLiteral{.slot = slot, .wanted = wanted});
  tasks.pushTask(WriteCheckedSlot{.slot = slot});

  return tasks.pushTask(
      CheckFunctionExpr{.functionExpr = functionExpr, .expected = std::move(signature)});
}

void LibraryEventsRegistry::afterListenerThis(const AfterListenerThis &task) {
  auto &tasks = this->state.walk.tasks;
  const Type *self = tasks.takeAnswer();
  auto *functionExpr = cast<FunctionExpr>(*task.slot);
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isTaking = this->isListenerTakingThis(task.want, self, functionDecl->params[0].loc);
  if (isTaking && functionDecl->isAsync) {
    auto message = ErrorTextListener::asyncThis();
    this->state.diagnostics.error(functionExpr->loc(), message);
  }
  if (!isTaking || functionDecl->isAsync) {
    return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = nullptr});
  }
  const auto *wanted = this->withThis(task.want, self);
  auto signature = this->signatureOf(wanted);
  signature.voidResult = true;

  return this->pushListenerLiteral(task.slot, functionExpr, wanted, std::move(signature));
}

void LibraryEventsRegistry::finishListenerLiteral(const FinishListenerLiteral &task) {
  auto &tasks = this->state.walk.tasks;
  if ((*task.slot)->type->isError()) {
    return;
  }
  tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::unknown()});

  return tasks.pushTask(CoerceSlot{.slot = task.slot, .to = task.wanted, .what = ""});
}

void LibraryEventsRegistry::afterListenerValue(const AfterListenerValue &task) {
  Expr *arg = *task.slot;
  const ListenerWantDraft &want = task.want;
  auto &tasks = this->state.walk.tasks;
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  if (!type->isFunction()) {
    auto message = ErrorTextListener::notFunc(type->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  if (type->hasThis()) {
    return this->checkThisValue(task.slot, want);
  }
  bool isAsync = type->ret()->isPromise();
  const Type *wanted = isAsync ? want.async : want.sync;
  if (type == wanted) {
    return tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::unknown()});
  }
  if (isAsync && type->ret() != want.async->ret()) {
    auto message = ErrorTextListener::asyncRet(type->ret()->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  auto *adapted = this->adaptedListener(arg, wanted);
  if (adapted == nullptr) {
    auto message = ErrorTextListener::getsArrow(want.event, want.args->name(), type->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  *task.slot = adapted;

  return tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::unknown()});
}

void LibraryEventsRegistry::checkThisValue(Expr **slot, const ListenerWantDraft &want) {
  const Expr *arg = *slot;
  const Type *type = arg->type;
  const Type *self = type->params()[0];
  if (!this->isListenerTakingThis(want, self, arg->loc())) {
    return;
  }
  if (type == this->withThis(want, self)) {
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = slot, .to = Type::unknown()});
  }
  auto message = ErrorTextListener::gets(want.event, want.args->name(), type->name());
  this->state.diagnostics.error(arg->loc(), message);
}

LibraryEventsRegistry::ListenerKind
LibraryEventsRegistry::listenerKindOf(const Expr *original, const Expr *checked) const {
  const auto *functionExpr = dyn_cast<FunctionExpr>(original);
  if (functionExpr != nullptr) {
    const FunctionDecl *functionDecl = functionExpr->fn;
    bool isExplicitThis = !functionDecl->params.empty() &&
                          KeywordTable::find(functionDecl->params[0].name) == Keyword::THIS &&
                          functionDecl->params[0].type.has_value();
    if (isExplicitThis) {
      return ListenerKind::This;
    }

    return functionDecl->isAsync ? ListenerKind::Async : ListenerKind::Plain;
  }
  const Type *type = ExprParts::withoutImplicitCasts(checked)->type;
  if (type->isFunction() && type->hasThis()) {
    return ListenerKind::This;
  }
  bool isAsync = type->isFunction() && type->ret()->isPromise();

  return isAsync ? ListenerKind::Async : ListenerKind::Plain;
}

Expr *LibraryEventsRegistry::adaptedListener(Expr *arg, const Type *to) {
  const Type *from = arg->type;
  if (this->namedFunction(arg) == nullptr || from->hasRest() || to->hasRest()) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> params = from->params();
  llvm::ArrayRef<const Type *> wanted = to->params();
  size_t shared = std::min(params.size(), wanted.size());
  for (size_t index = 0; index < shared; ++index) {
    if (!this->isParamFitting(params[index], wanted[index])) {
      return nullptr;
    }
  }
  if (wanted.size() < from->minArgs()) {
    return nullptr;
  }
  if (!to->ret()->isVoid() && from->ret() != to->ret()) {
    return nullptr;
  }
  auto *castExpr = this->state.ast.make<CastExpr>(arg->loc(), arg, std::nullopt);
  castExpr->type = to;

  return castExpr;
}

bool LibraryEventsRegistry::isParamFitting(const Type *param, const Type *wanted) const {
  bool isOptionalWanted = param->isOptional() && param->inner() == wanted;
  bool isUnknownTaken = param->nonOptional()->isUnknown() && !wanted->isVoid();

  return param == wanted || isOptionalWanted || isUnknownTaken;
}

FunctionDecl *LibraryEventsRegistry::namedFunction(const Expr *expr) const {
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

Signature LibraryEventsRegistry::signatureOf(const Type *functionType) const {
  llvm::ArrayRef<const Type *> params = functionType->params();

  return Signature{
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = functionType->minArgs(),
      .ret = functionType->ret(),
      .voidResult = functionType->ret()->isVoid(),
      .hasThis = functionType->hasThis(),
  };
}

std::string LibraryEventsRegistry::eventLabel(const Type *events, const Expr *name) const {
  if (events->isRecord()) {
    const auto *literal = dyn_cast<StringLitExpr>(ExprParts::withoutImplicitCasts(name));

    return literal != nullptr ? ToUtf8::toUtf8(literal->value) : std::string();
  }
  auto key = this->state.code.constants.constantKey(name);

  return key.value_or(std::string());
}

void LibraryEventsRegistry::checkEventTargetCall(const CheckEventTargetCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto method = EventTargetMethodTable::find(member->name);
  auto &declarations = this->state.library.declarations;
  auto *eventClass = declarations.findLibraryClass(LibraryClass::EVENT);
  auto *objectListener = declarations.findLibraryType(LibraryType::EVENT_LISTENER_OBJECT);
  if (!method.has_value() || eventClass == nullptr || objectListener == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  member->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 2, 3);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  auto target = TargetCallDraft{
      .call = call,
      .member = member,
      .classDecl = task.classDecl,
      .eventClass = eventClass,
      .isAdd = method == EventTargetMethod::ADD_EVENT_LISTENER,
  };
  tasks.pushTask(AfterTargetObjectType{.target = target});
  tasks.pushTask(AnswerAliasType{.alias = objectListener});
  tasks.pushTask(ResolveAlias{.alias = objectListener});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryEventsRegistry::afterTargetObjectType(const AfterTargetObjectType &task) {
  const TargetCallDraft &target = task.target;
  CallExpr *call = target.call;
  auto &tasks = this->state.walk.tasks;
  const Type *object = tasks.takeAnswer();
  Expr **listener = &call->args[1];
  auto *function = dyn_cast<FunctionExpr>(*listener);
  if (isa<ObjectLitExpr>(*listener)) {
    tasks.pushTask(FinishTargetCall{.target = target, .listenerType = nullptr});

    return tasks.pushTask(CheckArgSlot{.slot = listener, .to = object});
  }
  if (function == nullptr) {
    tasks.pushTask(AfterTargetValue{.target = target, .object = object});

    return tasks.pushTask(CheckExprSlot{.slot = listener, .expected = nullptr});
  }
  std::vector<Param> &params = function->fn->params;
  bool isTypedParam = !params.empty() && params[0].type && !params[0].rest();
  if (!isTypedParam) {
    return this->pushTargetFunction(target, target.eventClass->type);
  }
  tasks.pushTask(AfterTargetParamType{.target = target});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &*params[0].type, .use = TypeUse::STANDALONE});
}

void LibraryEventsRegistry::afterTargetParamType(const AfterTargetParamType &task) {
  const TargetCallDraft &target = task.target;
  const Type *written = this->state.walk.tasks.takeAnswer();
  auto *function = cast<FunctionExpr>(target.call->args[1]);
  const Type *kind = this->listenedEvent(written, function->fn->params[0].loc, target.eventClass);

  return this->pushTargetFunction(target, kind);
}

void LibraryEventsRegistry::pushTargetFunction(const TargetCallDraft &target, const Type *kind) {
  CallExpr *call = target.call;
  auto &tasks = this->state.walk.tasks;
  if (kind == nullptr) {
    tasks.pushChecked(call);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  auto signature = Signature{
      .params = {kind},
      .minArgs = 1,
      .ret = Type::voidTy(),
      .voidResult = true,
      .asyncVoid = true,
  };
  auto *function = cast<FunctionExpr>(call->args[1]);
  tasks.pushTask(FinishTargetCall{.target = target, .listenerType = kind});
  tasks.pushTask(WriteCheckedSlot{.slot = &call->args[1]});

  return tasks.pushTask(
      CheckFunctionExpr{.functionExpr = function, .expected = std::move(signature)});
}

void LibraryEventsRegistry::afterTargetValue(const AfterTargetValue &task) {
  const TargetCallDraft &target = task.target;
  CallExpr *call = target.call;
  auto &tasks = this->state.walk.tasks;
  Expr **listener = &call->args[1];
  const Type *type = (*listener)->type;
  if (!type->isFunction()) {
    tasks.pushTask(FinishTargetCall{.target = target, .listenerType = nullptr});

    return tasks.pushTask(CoerceSlot{.slot = listener, .to = task.object, .what = "the listener"});
  }
  const Type *event = target.eventClass->type;
  bool isNoParams = type->params().empty();
  const Type *kind =
      isNoParams ? event
                 : this->listenedEvent(type->params()[0], (*listener)->loc(), target.eventClass);
  if (kind == nullptr) {
    return tasks.pushChecked(call);
  }
  const auto *handler = Type::function({kind}, 1, false, Type::voidTy());
  tasks.pushTask(FinishTargetCall{.target = target, .listenerType = kind});

  return tasks.pushTask(CoerceSlot{.slot = listener, .to = handler, .what = ""});
}

void LibraryEventsRegistry::finishTargetCall(const FinishTargetCall &task) {
  const TargetCallDraft &target = task.target;
  CallExpr *call = target.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args[1]->type->isError()) {
    return tasks.pushChecked(call);
  }
  auto given = std::vector<bool>(call->args.size(), false);
  given[0] = true;
  given[1] = true;
  bool isFunction = task.listenerType != nullptr;
  auto added =
      isFunction ? LibraryMethod::ADD_FUNCTION_LISTENER : LibraryMethod::ADD_OBJECT_LISTENER;
  auto method = target.isAdd ? added : LibraryMethod::REMOVE_LISTENER_WITH;
  auto bound = target.isAdd && isFunction ? std::vector<const Type *>{task.listenerType}
                                          : std::vector<const Type *>{};
  tasks.pushTask(CallLibraryMethod{
      .call = call,
      .member = target.member,
      .classDecl = target.classDecl,
      .name = LibraryMethodTable::getName(method),
      .bound = std::move(bound),
      .given = std::move(given),
      .receiver = CallReceiver::OWN,
  });
  if (target.isAdd) {
    return;
  }
  this->state.inventory.comparesFunctions = true;

  return tasks.pushTask(ConvertSlot{.slot = &call->args[1], .to = Type::unknown()});
}

const Type *LibraryEventsRegistry::listenedEvent(const Type *type, llvm::SMLoc loc,
                                                 ClassDecl *eventClass) {
  if (type->isError()) {
    return nullptr;
  }
  auto listened = this->checkListenerEvent(type, eventClass);
  if (listened.isErr()) {
    this->state.diagnostics.error(loc, listened.error().message);

    return nullptr;
  }

  return type;
}

void LibraryEventsRegistry::checkEventsFunction(const CheckEventsFunction &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  auto function = EventsFunctionTable::find(task.name);
  if (this->state.library.declarations.isInsideLibrary(call->loc()) || !function.has_value()) {
    return tasks.pushChecked(nullptr);
  }
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  bool isListeners =
      function == EventsFunction::GET_EVENT_LISTENERS || function == EventsFunction::LISTENER_COUNT;
  auto arity = this->state.library.common.checkArity(call, 2, isListeners ? 2 : 3);
  if (arity.isErr()) {
    this->state.diagnostics.error(task.loc, arity.error().message);

    return tasks.pushChecked(call);
  }
  auto draft = EventsFunctionDraft{
      .call = call,
      .functionName = task.name,
      .loc = task.loc,
      .function = *function,
      .isTimed = this->hasTimeoutOption(call),
  };
  tasks.pushTask(AfterEventsObject{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryEventsRegistry::checkEventsLibraryCall(const CheckEventsLibraryCall &task) {
  FuncSymbol *function = task.function;
  bool isEvents = function->decl != nullptr && this->state.library.declarations.findLibraryFunction(
                                                   function->decl->name) == function->decl;
  if (!isEvents) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  auto events = CheckEventsFunction{
      .call = task.call,
      .name = function->decl->name,
      .loc = task.name->loc(),
  };

  return this->checkEventsFunction(events);
}

void LibraryEventsRegistry::afterEventsObject(const AfterEventsObject &task) {
  const EventsFunctionDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *object = call->args[0];
  bool isListeners = draft.function == EventsFunction::GET_EVENT_LISTENERS;
  bool isWait = draft.function == EventsFunction::ONCE || draft.function == EventsFunction::ON;
  if (object->type->isError()) {
    return this->finishLoosely(call, 1);
  }
  if ((isListeners || isWait) && this->isEventTarget(object->type)) {
    return isWait ? this->checkTargetWait(draft) : this->targetListeners(call, object);
  }
  bool isTarget = isListeners || draft.function == EventsFunction::ONCE;
  auto emitter = this->checkEmitterEvents(object->type, draft.functionName, isTarget);
  if (emitter.isErr()) {
    this->state.diagnostics.error(object->loc(), emitter.error().message);

    return this->finishLoosely(call, 1);
  }
  if (isWait) {
    return this->checkEmitterWait(draft, emitter.value());
  }
  auto emitterMember = isListeners ? EmitterMember::LISTENERS : EmitterMember::LISTENER_COUNT;
  auto methodName = EmitterMemberTable::getName(emitterMember);
  auto *member = this->state.ast.make<MemberExpr>(call->loc(), object, methodName);
  member->type = Type::error();
  auto *method =
      this->state.ast.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  tasks.pushTask(AfterEventsMethod{.method = method});

  return tasks.pushTask(CheckEmitterCall{
      .call = method,
      .member = member,
      .classDecl = const_cast<ClassDecl *>(object->type->classDecl()),
      .receiver = CallReceiver::OWN,
  });
}

void LibraryEventsRegistry::afterEventsMethod(const AfterEventsMethod &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();

  return tasks.pushChecked(checked != nullptr ? checked : task.method);
}

void LibraryEventsRegistry::targetListeners(CallExpr *call, Expr *object) {
  auto listenersOf = LibraryMethodTable::getName(LibraryMethod::LISTENERS_OF);
  auto *member = this->state.ast.make<MemberExpr>(call->loc(), object, listenersOf);
  auto *method =
      this->state.ast.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
  method->type = Type::error();

  return this->state.walk.tasks.pushTask(CallLibraryMethod{
      .call = method,
      .member = member,
      .classDecl = const_cast<ClassDecl *>(object->type->classDecl()),
      .name = listenersOf,
      .bound = {},
      .given = {false},
      .receiver = CallReceiver::OWN,
  });
}

void LibraryEventsRegistry::checkTargetWait(const EventsFunctionDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (draft.function != EventsFunction::ONCE) {
    auto message = ErrorTextEventsFunction::targetOn();
    this->state.diagnostics.error(call->args[0]->loc(), message);

    return this->finishLoosely(call, 1);
  }
  auto onceTarget = LibraryFunctionTable::getName(LibraryFunction::ONCE_TARGET);
  tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = onceTarget,
      .bound = {},
      .given = this->waitGiven(call),
      .loc = draft.loc,
  });

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::string()});
}

std::vector<bool> LibraryEventsRegistry::waitGiven(const CallExpr *call) const {
  auto given = std::vector<bool>(call->args.size(), false);
  given[0] = true;
  given[1] = true;

  return given;
}

void LibraryEventsRegistry::checkEmitterWait(const EventsFunctionDraft &draft, const Type *events) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterWaitEvent{.draft = draft, .events = events});

  return tasks.pushTask(
      EventArgsOf{.events = events, .name = &call->args[1], .method = draft.functionName});
}

void LibraryEventsRegistry::afterWaitEvent(const AfterWaitEvent &task) {
  const EventsFunctionDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  const Type *args = tasks.takeAnswer();
  if (args == nullptr) {
    return this->finishLoosely(call, 2);
  }
  bool isOnce = draft.function == EventsFunction::ONCE;
  bool isEnding = this->isWaitEnding(call);
  if (!draft.isTimed && !isEnding) {
    auto *name = dyn_cast<NameExpr>(call->args[0]);
    llvm::StringRef shown = name != nullptr ? name->name : llvm::StringRef();
    auto message = ErrorTextEventsFunction::endless(shown, draft.functionName);
    this->state.diagnostics.error(call->args[0]->loc(), message);

    return this->finishLoosely(call, 2);
  }
  if (isOnce && call->args.size() == 3 && !draft.isTimed) {
    auto message = ErrorTextEventsFunction::options();
    this->state.diagnostics.error(call->args[2]->loc(), message);

    return this->finishLoosely(call, 2);
  }
  if (task.events->isRecord()) {
    tasks.pushAnswer(task.events->valueType());

    return this->afterWaitErrors(
        AfterWaitErrors{.draft = draft, .events = task.events, .args = args});
  }
  auto errorName = EmitterEventTable::getName(EmitterEvent::ERROR_EVENT);
  tasks.pushTask(AfterWaitErrors{.draft = draft, .events = task.events, .args = args});

  return tasks.pushTask(FindEventArgs{.events = task.events, .name = errorName.str()});
}

void LibraryEventsRegistry::afterWaitErrors(const AfterWaitErrors &task) {
  const EventsFunctionDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  const Type *errors = tasks.takeAnswer();
  bool isOnce = draft.function == EventsFunction::ONCE;
  const Type *failure = Type::never();
  if (errors != nullptr) {
    this->state.library.events.markErrorListened(task.events);
    auto value = this->checkOneErrorValue(errors, draft.functionName);
    if (value.isErr()) {
      this->state.diagnostics.error(call->args[1]->loc(), value.error().message);

      return tasks.pushChecked(call);
    }
    failure = value.value();
  }
  if (!draft.isTimed) {
    size_t most = isOnce ? 2 : 3;
    while (call->args.size() < most) {
      call->args.push_back(this->state.ast.make<UndefinedExpr>(call->loc()));
    }
  }
  auto given = this->waitGiven(call);
  auto *flag = this->state.ast.make<BoolLitExpr>(call->loc(), errors != nullptr);
  call->args.push_back(flag);
  given.push_back(true);
  auto variant = this->waitVariant(draft, errors != nullptr);
  const Type *settled = errors != nullptr ? failure : Type::tuple({});
  const Type *second = draft.isTimed ? failure : settled;
  tasks.pushTask(CallLibraryFunction{
      .call = call,
      .name = variant,
      .bound = {task.args, second},
      .given = std::move(given),
      .loc = draft.loc,
  });

  return tasks.pushTask(CheckExprSlot{.slot = &call->args.back(), .expected = nullptr});
}

llvm::StringRef LibraryEventsRegistry::waitVariant(const EventsFunctionDraft &draft,
                                                   bool hasErrors) const {
  bool isOnce = draft.function == EventsFunction::ONCE;
  if (draft.isTimed) {
    auto within = isOnce ? LibraryFunction::ONCE_WITHIN : LibraryFunction::ON_WITHIN;

    return LibraryFunctionTable::getName(within);
  }
  if (!hasErrors) {
    return draft.functionName;
  }
  auto result = isOnce ? LibraryFunction::ONCE_RESULT : LibraryFunction::ON_RESULT;

  return LibraryFunctionTable::getName(result);
}

std::optional<std::string> LibraryEventsRegistry::chainText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const Expr *link = expr;
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = member->object;
    member = dyn_cast<MemberExpr>(link);
  }
  auto root = this->rootText(link);
  if (!root.has_value()) {
    return std::nullopt;
  }
  std::string text = *root;
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

std::optional<std::string> LibraryEventsRegistry::rootText(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name != nullptr) {
    return name->name.str();
  }
  auto *call = dyn_cast<CallExpr>(expr);
  auto *callee = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  if (callee == nullptr) {
    return std::nullopt;
  }
  auto *fd = call->args.size() == 1 ? dyn_cast<IntLitExpr>(call->args[0]) : nullptr;
  bool isStdin = callee->name == ProcessFunctionTable::getName(ProcessFunction::STDIN);
  bool isStdout = callee->name == ProcessFunctionTable::getName(ProcessFunction::STDOUT);
  if (isStdin && call->args.empty()) {
    return ProcessQualifiedNameTable::getQualifiedName(ProcessMember::STDIN).str();
  }
  if (isStdout && fd != nullptr) {
    ProcessMember stream = fd->value == 2 ? ProcessMember::STDERR : ProcessMember::STDOUT;
    return ProcessQualifiedNameTable::getQualifiedName(stream).str();
  }

  return std::nullopt;
}

EmitterMethodKind LibraryEventsRegistry::emitterMethod(llvm::StringRef name) const {
  auto member = EmitterMemberTable::find(name);
  if (!member.has_value()) {
    return EmitterMethodKind::None;
  }
  switch (*member) {
    case EmitterMember::ON:
    case EmitterMember::ADD_LISTENER:
    case EmitterMember::PREPEND_LISTENER:
    case EmitterMember::ONCE:
    case EmitterMember::PREPEND_ONCE_LISTENER:
      return EmitterMethodKind::Listen;
    case EmitterMember::OFF:
    case EmitterMember::REMOVE_LISTENER:
      return EmitterMethodKind::Remove;
    case EmitterMember::EMIT:
      return EmitterMethodKind::Emit;
    case EmitterMember::LISTENERS:
      return EmitterMethodKind::Listeners;
    case EmitterMember::RAW_LISTENERS:
      return EmitterMethodKind::RawListeners;
    case EmitterMember::LISTENER_COUNT:
      return EmitterMethodKind::Count;
    case EmitterMember::REMOVE_ALL_LISTENERS:
      return EmitterMethodKind::RemoveAll;
    case EmitterMember::EVENT_NAMES:
      break;
  }

  return EmitterMethodKind::None;
}

bool LibraryEventsRegistry::canReceive(const Type *type) const {
  return type != nullptr && type->isObject() && !type->isAnyObject() && !type->isTuple() &&
         !type->isWeakRef() && !type->isRegExp() && !type->isMatch() && !type->isDate() &&
         !type->isIntl() && !type->isRuntimeObject() && !type->isCrypto() && !type->isTimer() &&
         !type->isBytes();
}

bool LibraryEventsRegistry::isOwningEmitter(const Expr *emitter) const {
  const auto *root = this->emitterRoot(emitter);
  if (isa<NewExpr>(root)) {
    return true;
  }
  const auto *name = dyn_cast<NameExpr>(root);
  const auto *functionDecl = this->state.walk.functions.getCurrentDecl();
  if (name == nullptr || name->var() == nullptr || functionDecl == nullptr ||
      name->var()->owner() != functionDecl || name->var()->isGlobal() ||
      name->var() == functionDecl->thisVar()) {
    return false;
  }
  auto isParam = std::bind_front(&LibraryEventsRegistry::isParamOf, this, name->var());

  return llvm::none_of(functionDecl->params, isParam);
}

const Expr *LibraryEventsRegistry::emitterRoot(const Expr *emitter) const {
  const Expr *current = emitter;
  const Expr *next = this->emitterLink(current);
  while (next != nullptr) {
    current = next;
    next = this->emitterLink(current);
  }

  return current;
}

const Expr *LibraryEventsRegistry::emitterLink(const Expr *expr) const {
  const auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    return conversion->operand;
  }
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *chained = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (chained != nullptr && this->emitterMethod(chained->name) == EmitterMethodKind::Listen) {
    return chained->object;
  }

  return nullptr;
}

bool LibraryEventsRegistry::isParamOf(const VarSymbol *variable, const Param &param) const {
  return param.sym == variable;
}

bool LibraryEventsRegistry::classifyArg(Expr *arg, const Expr *emitter, int &bits,
                                        ListenerUse use) {
  bool isTimer = use == ListenerUse::TIMER;
  Expr *expr = ExprParts::withoutImplicitCasts(arg);
  bool isTopLevel = (this->state.walk.functions.getCurrentDecl() == nullptr &&
                     !this->state.walk.functions.isInsideAwaitLoop()) ||
                    (emitter != nullptr && this->isOwningEmitter(emitter));
  auto shape = this->listenerShape(expr);
  switch (shape.form) {
    case ListenerForm::Free:
      bits |= isTopLevel ? 0 : kDedupeBit;
      return true;
    case ListenerForm::Receiver:
      if (shape.envFn != nullptr) {
        shape.envFn->receiverEnv = shape.var;
      }
      return this->addListenerReceiver(shape, isTopLevel, bits);
    case ListenerForm::Captures:
      return isTopLevel || this->reportCapturesKept(arg, shape.captured, isTimer);
    case ListenerForm::Unknown:
      return isTopLevel || this->reportUnknownListener(arg, isTimer);
  }

  return false;
}

LibraryEventsRegistry::ListenerShape LibraryEventsRegistry::listenerShape(Expr *expr) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(expr);
  if (functionExpr != nullptr) {
    return this->closureShape(functionExpr->fn);
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  bool isMethod = member != nullptr && member->method() != nullptr &&
                  member->method()->decl != nullptr && !member->method()->decl->isStatic() &&
                  this->canReceive(member->object->type);
  if (isMethod) {
    auto *self = dyn_cast<NameExpr>(member->object);
    return ListenerShape{
        .form = ListenerForm::Receiver,
        .type = member->object->type,
        .var = self != nullptr ? self->var() : nullptr,
        .envFn = nullptr,
        .captured = {},
    };
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (name != nullptr && name->func() != nullptr && name->func()->decl != nullptr) {
    return this->functionShape(name->func()->decl);
  }

  return ListenerShape{
      .form = ListenerForm::Unknown,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = {},
  };
}

LibraryEventsRegistry::ListenerShape
LibraryEventsRegistry::closureShape(FunctionDecl *functionDecl) const {
  if (functionDecl->captures.empty()) {
    return ListenerShape{
        .form = ListenerForm::Free,
        .type = nullptr,
        .var = nullptr,
        .envFn = nullptr,
        .captured = {},
    };
  }
  const VarSymbol *only = functionDecl->captures[0];
  bool isSingle = functionDecl->captures.size() == 1 && functionDecl->group == nullptr &&
                  !functionDecl->isGenerator && !only->boxed() && this->canReceive(only->type);
  if (isSingle) {
    return ListenerShape{
        .form = ListenerForm::Receiver,
        .type = only->type,
        .var = only,
        .envFn = functionDecl,
        .captured = {},
    };
  }

  return ListenerShape{
      .form = ListenerForm::Captures,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = std::vector<const VarSymbol *>(functionDecl->captures.begin(),
                                                 functionDecl->captures.end()),
  };
}

LibraryEventsRegistry::ListenerShape
LibraryEventsRegistry::functionShape(FunctionDecl *functionDecl) const {
  const std::vector<VarSymbol *> &captures =
      functionDecl->group != nullptr ? functionDecl->group->captures : functionDecl->captures;
  ListenerForm form = captures.empty() ? ListenerForm::Free : ListenerForm::Captures;

  return ListenerShape{
      .form = form,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = std::vector<const VarSymbol *>(captures.begin(), captures.end()),
  };
}

bool LibraryEventsRegistry::addListenerReceiver(const ListenerShape &shape, bool topLevel,
                                                int &bits) {
  bits |= kReceiverBit | (topLevel ? 0 : kDedupeBit);
  this->state.inventory.listenerReceivers.push_back(shape.type);
  const auto *functionDecl = this->state.walk.functions.getCurrentDecl();
  bool isSelf = functionDecl != nullptr && functionDecl->isConstructor() &&
                functionDecl->cls() != nullptr && shape.var != nullptr &&
                shape.var == functionDecl->thisVar();
  if (isSelf) {
    this->state.library.events.addSelfListener(functionDecl->cls());
  }

  return true;
}

bool LibraryEventsRegistry::reportUnknownListener(const Expr *arg, bool timer) {
  auto message = ErrorTextListenerLife::unknown(timer);
  this->state.diagnostics.error(arg->loc(), message);

  return false;
}

bool LibraryEventsRegistry::reportCapturesKept(const Expr *arg,
                                               const std::vector<const VarSymbol *> &captured,
                                               bool timer) {
  for (const VarSymbol *variable : captured) {
    if (variable->boxed() || !this->canReceive(variable->type)) {
      auto message = ErrorTextListenerLife::keeps(timer, variable->name);
      this->state.diagnostics.error(arg->loc(), message);
      return false;
    }
  }
  const VarSymbol *kept = captured[0];
  for (const VarSymbol *variable : captured) {
    if (KeywordTable::find(variable->name) == Keyword::THIS) {
      kept = variable;
    }
  }
  auto others = std::vector<const VarSymbol *>();
  for (const VarSymbol *variable : captured) {
    if (variable != kept) {
      others.push_back(variable);
    }
  }
  auto message = ErrorTextListenerLife::captures(timer, captured, others, kept->name);
  this->state.diagnostics.error(arg->loc(), message);

  return false;
}

EmitterKey LibraryEventsRegistry::keyOf(const Expr *root) const {
  root = ExprParts::withoutImplicitCasts(root);
  EmitterKey key;
  auto *name = dyn_cast<NameExpr>(root);
  if (name != nullptr) {
    const VarSymbol *variable =
        name->var() != nullptr ? name->var() : this->state.walk.scopes.findName(name->name).var();
    if (variable != nullptr) {
      key.path.var = variable;
      return key;
    }
  }
  if (std::optional<FieldPath> path = this->state.flow.tracked.fieldPath(root)) {
    key.path = *path;
  } else if (std::optional<std::string> text = this->chainText(root)) {
    key.text = *text;
  } else {
    key.root = root;
  }

  return key;
}

std::string LibraryEventsRegistry::emitterPath(const EmitterKey &key) const {
  if (!key.text.empty()) {
    return key.text;
  }
  if (key.path.var == nullptr) {
    return "";
  }
  std::string name = key.path.var->name.str();
  for (llvm::StringRef part : key.path.names) {
    name += "." + part.str();
  }

  return name;
}

void LibraryEventsRegistry::walkDroppedListeners() {
  auto &store = this->state.library.events;
  for (const DroppedNew &dropped : store.getDroppedNews()) {
    if (this->hasSelfListener(dropped.cls)) {
      auto message = ErrorTextEmitterNew::dropped(dropped.cls->name);
      this->state.diagnostics.error(dropped.loc, message);
    }
  }
}

bool LibraryEventsRegistry::hasSelfListener(const ClassDecl *classDecl) const {
  auto &store = this->state.library.events;
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (store.isSelfListener(current)) {
      return true;
    }
  }

  return false;
}

void LibraryEventsRegistry::walkErrorListeners() {
  auto &store = this->state.library.events;
  for (const ErrorEmit &emit : store.getErrorEmits()) {
    if (!store.isErrorListened(emit.events)) {
      auto message = ErrorTextUnlistened::unheard();
      this->state.diagnostics.error(emit.loc, message);
    }
  }
}

bool LibraryEventsRegistry::isOwnEmit(const EventSite *site, const SentEmit &emit) const {
  if (emit.callReceiver == CallReceiver::SUPER) {
    return true;
  }
  const Expr *object = emit.member->object;
  object = ExprParts::withoutImplicitCasts(object);
  auto *self = dyn_cast<NameExpr>(object);
  if (self != nullptr && KeywordTable::find(self->name) == Keyword::THIS) {
    return true;
  }
  auto *module = this->findModuleAt(emit.call->loc());

  return module != nullptr && module == site->owner;
}

bool LibraryEventsRegistry::isMentioningParam(const Type *type) const {
  return TypeQueries::mentionsParam(type);
}

EventRecord *LibraryEventsRegistry::findEvent(EventSite *site, llvm::StringRef name) const {
  for (EventRecord &record : site->events) {
    if (record.name == name) {
      return &record;
    }
  }

  return nullptr;
}

bool LibraryEventsRegistry::isFrozen(const EventRecord *record) const {
  return record != nullptr && record->state == EventState::DONE && record->args != nullptr;
}

std::string LibraryEventsRegistry::fileOfPlace(const std::string &place) const {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

std::vector<FunctionDecl *> LibraryEventsRegistry::classMethods(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

bool LibraryEventsRegistry::isHoldingClassExpr(const Stmt *where) const {
  auto exprs = AstWalker::allExprsIn(where);

  auto isClass = std::bind_front(&LibraryEventsRegistry::isClassExpr, this);

  return llvm::any_of(exprs, isClass);
}

bool LibraryEventsRegistry::isClassExpr(const Expr *expr) const {
  return llvm::isa<ClassExpr>(expr);
}

bool LibraryEventsRegistry::canTryFunction(const FunctionDecl *functionDecl) const {
  const ClassDecl *classDecl = functionDecl->cls();
  bool isGeneric = functionDecl->sym != nullptr && functionDecl->sym->isGeneric();
  bool isGenericClass = classDecl != nullptr && classDecl->isGeneric();
  bool isUsable = functionDecl->body != nullptr && functionDecl->sym != nullptr && !isGeneric &&
                  !isGenericClass;

  return isUsable && !this->isHoldingClassExpr(functionDecl);
}

void LibraryEventsRegistry::addCandidates(const Stmt *where, Module *module,
                                          FunctionDecl *functionDecl, size_t index,
                                          std::vector<EmitCandidate> &found) const {
  for (const Expr *expr : AstWalker::allExprsIn(where)) {
    auto candidate = this->emitCandidate(expr);
    if (candidate.has_value()) {
      candidate->module = module;
      candidate->function = functionDecl;
      candidate->index = index;
      found.push_back(std::move(candidate.value()));
    }
  }
}

std::optional<EmitCandidate> LibraryEventsRegistry::emitCandidate(const Expr *expr) const {
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  bool isEmit = member != nullptr &&
                EmitterMemberTable::find(member->name) == EmitterMember::EMIT &&
                !call->args.empty();
  if (!isEmit) {
    return std::nullopt;
  }
  const auto *name = dyn_cast<StringLitExpr>(call->args[0]);
  if (name == nullptr) {
    return std::nullopt;
  }
  const auto *receiver = dyn_cast<NameExpr>(member->object);
  bool isSelf = receiver != nullptr && this->isReceiverKeyword(receiver->name);
  EmitCandidate candidate;
  candidate.call = call;
  candidate.event = ToUtf8::toUtf8(name->value);
  candidate.self = isSelf;

  return candidate;
}

void LibraryEventsRegistry::defineMapFields(EventSite *site) const {
  auto fields = std::vector<Field>();
  for (const EventRecord &record : site->events) {
    if (record.args != nullptr) {
      fields.push_back({.name = record.name, .type = record.args});
    }
  }
  site->map->defineFields(std::move(fields));
}

std::string LibraryEventsRegistry::shortPlaceOf(llvm::SMLoc loc) const {
  auto place = this->state.library.common.placeOf(loc);
  auto file = this->fileOfPlace(place);
  std::string name = llvm::sys::path::filename(file).str();

  return name + place.substr(file.size());
}

Module *LibraryEventsRegistry::findModuleAt(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = this->state.diagnostics.sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (id == 0) {
    return nullptr;
  }
  for (Module *module : this->state.file.modules) {
    if (llvm::is_contained(module->buffers, id)) {
      return module;
    }
  }

  return nullptr;
}

Type *LibraryEventsRegistry::siteMap(const std::string &key, const std::string &name, Module *owner,
                                     const ClassDecl *classDecl) {
  auto &store = this->state.library.events;
  auto *known = store.findSite(key);
  if (known != nullptr) {
    return known->map;
  }
  auto *map = Type::declareObject(name);
  map->defineFields({});
  EventSite site;
  site.key = key;
  site.map = map;
  site.owner = owner;
  site.cls = classDecl;
  auto *added = store.addSite(std::move(site));

  return added->map;
}

Type *LibraryEventsRegistry::classEventMap(const ClassDecl *classDecl) {
  std::string key =
      "class " + this->state.library.common.placeOf(classDecl->loc()) + " " + classDecl->name.str();
  std::string name = "events of " + classDecl->name.str();
  auto *declared = this->state.declaration.modules.findDeclModule(classDecl);
  Module *owner = declared != nullptr ? declared : this->findModuleAt(classDecl->loc());

  return this->siteMap(key, name, owner, classDecl);
}

Type *LibraryEventsRegistry::newEventMap(llvm::SMLoc loc) {
  std::string key = "new " + this->state.library.common.placeOf(loc);
  std::string name = "events at " + this->shortPlaceOf(loc);
  auto *owner = this->findModuleAt(loc);

  return this->siteMap(key, name, owner, nullptr);
}

EventRecord *LibraryEventsRegistry::addEvent(EventSite *site, llvm::StringRef name) {
  EventRecord record;
  record.name = name.str();
  site->events.push_back(std::move(record));

  return &site->events.back();
}

void LibraryEventsRegistry::rememberEventArgs(EventSite *site, EventRecord *record,
                                              const Type *args, llvm::SMLoc loc) {
  record->args = args;
  if (record->where.empty()) {
    record->where = this->shortPlaceOf(loc);
  }
  this->defineMapFields(site);
}

const std::vector<EmitCandidate> &LibraryEventsRegistry::getModuleCandidates(Module *module) {
  auto &store = this->state.library.events;
  if (store.hasCandidates(module)) {
    return store.getCandidates(module);
  }
  auto found = std::vector<EmitCandidate>();
  std::vector<Stmt *> &items = module->file.items;
  for (size_t index = 0; index < items.size(); ++index) {
    Stmt *item = items[index];
    auto *functionDecl = dyn_cast<FunctionDecl>(item);
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (functionDecl != nullptr) {
      this->addCandidates(functionDecl, module, functionDecl, index, found);
    } else if (classDecl != nullptr) {
      for (FunctionDecl *method : this->classMethods(classDecl)) {
        this->addCandidates(method, module, method, index, found);
      }
    } else {
      this->addCandidates(item, module, nullptr, index, found);
    }
  }
  store.rememberCandidates(module, std::move(found));

  return store.getCandidates(module);
}

std::vector<EmitCandidate> LibraryEventsRegistry::getSiteCandidates(const EventSite *site) {
  auto found = std::vector<EmitCandidate>();
  for (Module *module : this->state.file.modules) {
    bool isOwn = module == site->owner;
    bool isSkipped = module->builtin || (!isOwn && site->cls == nullptr);
    if (!isSkipped) {
      this->addModuleCandidates(site, module, found);
    }
  }
  auto late = this->getLateCandidates();
  for (const EmitCandidate &candidate : late) {
    if (this->isSubclassEmit(site, candidate)) {
      found.push_back(candidate);
    }
  }

  return found;
}

void LibraryEventsRegistry::addModuleCandidates(const EventSite *site, Module *module,
                                                std::vector<EmitCandidate> &found) {
  bool isOwn = module == site->owner;
  for (const EmitCandidate &candidate : this->getModuleCandidates(module)) {
    if (isOwn || this->isSubclassEmit(site, candidate)) {
      found.push_back(candidate);
    }
  }
}

std::vector<EmitCandidate> LibraryEventsRegistry::getLateCandidates() {
  auto found = std::vector<EmitCandidate>();
  for (ClassDecl *classDecl : this->state.code.late.getUncheckedLateClasses()) {
    auto *module = this->findModuleAt(classDecl->loc());
    for (FunctionDecl *method : this->classMethods(classDecl)) {
      this->addCandidates(method, module, method, 0, found);
    }
  }

  return found;
}

bool LibraryEventsRegistry::isSubclassEmit(const EventSite *site,
                                           const EmitCandidate &candidate) const {
  const ClassDecl *classDecl = candidate.function != nullptr ? candidate.function->cls() : nullptr;

  return site->cls != nullptr && candidate.self && classDecl != nullptr &&
         classDecl->derivesFrom(site->cls);
}

bool LibraryEventsRegistry::canTryStatement(const EmitCandidate &candidate) {
  auto &modules = this->state.declaration.modules;
  if (modules.getCurrentModule() != candidate.module) {
    return false;
  }
  auto &here = modules.getModuleState(candidate.module);

  return here.busy && candidate.index >= here.next;
}

bool LibraryEventsRegistry::isReceiverKeyword(llvm::StringRef name) const {
  auto keyword = KeywordTable::find(name);

  return keyword == Keyword::THIS || keyword == Keyword::SUPER;
}

LibraryEventsRegistry::ResultListenerThis
LibraryEventsRegistry::checkListenerThis(const Type *self, const Type *receiver) const {
  if (!self->isUnknown() && !receiver->isSubtypeOf(self)) {
    auto message = ErrorTextListener::thisType(self->name(), receiver->name());

    return err(LibraryEventsRegistryListenerThisError{
        .code = LibraryEventsRegistryListenerThisErrorCode::OTHER_RECEIVER,
        .message = message,
    });
  }

  return ok(self);
}

bool LibraryEventsRegistry::isListenerTakingThis(const ListenerWantDraft &want, const Type *self,
                                                 llvm::SMLoc loc) {
  if (self->isError()) {
    return false;
  }
  auto taken = this->checkListenerThis(self, want.receiver);
  if (taken.isErr()) {
    this->state.diagnostics.error(loc, taken.error().message);
    return false;
  }

  return true;
}

const Type *LibraryEventsRegistry::withThis(const ListenerWantDraft &want, const Type *self) const {
  const Type *sync = want.sync;
  std::vector<const Type *> params{self};
  params.insert(params.end(), sync->params().begin(), sync->params().end());

  return Type::function(params, sync->minArgs() + 1, sync->hasRest(), Type::voidTy(), true);
}

void LibraryEventsRegistry::noteListener(MemberExpr *member, const Type *events,
                                         llvm::StringRef event, const Type *errors) {
  bool isErrorEvent = EmitterEventTable::find(event) == EmitterEvent::ERROR_EVENT;
  if (isErrorEvent) {
    this->state.library.events.markErrorListened(events);
  }
  if (events->isRecord() || (!isErrorEvent && errors == nullptr)) {
    return;
  }
  const Expr *root = member->object;
  while (true) {
    root = ExprParts::withoutImplicitCasts(root);
    auto *inner = dyn_cast<CallExpr>(root);
    auto *chained = inner != nullptr ? dyn_cast<MemberExpr>(inner->callee) : nullptr;
    if (chained == nullptr || this->emitterMethod(chained->name) != EmitterMethodKind::Listen) {
      break;
    }
    root = chained->object;
  }
  auto key = this->keyOf(root);
  if (isErrorEvent) {
    return this->state.walk.flow.listenForErrors(key);
  }
  this->state.walk.flow.addPendingErrors(key, member->loc());
}

LibraryEventsRegistry::ResultListenerEvent
LibraryEventsRegistry::checkListenerEvent(const Type *type, const ClassDecl *eventClass) const {
  const auto *declared = type->classDecl();
  if (declared == nullptr || !declared->derivesFrom(eventClass) || type->isOptional()) {
    auto message = ErrorTextEventTarget::listener(type->name());

    return err(LibraryEventsRegistryListenerEventError{
        .code = LibraryEventsRegistryListenerEventErrorCode::NOT_AN_EVENT,
        .message = message,
    });
  }

  return ok(type);
}

LibraryEventsRegistry::ResultOneErrorValue
LibraryEventsRegistry::checkOneErrorValue(const Type *errors, llvm::StringRef functionName) const {
  if (!errors->isTuple() || errors->fields().size() != 1) {
    auto message = ErrorTextEventsFunction::errorValue(functionName, errors->name());

    return err(LibraryEventsRegistryOneErrorValueError{
        .code = LibraryEventsRegistryOneErrorValueErrorCode::SEVERAL_VALUES,
        .message = message,
    });
  }

  return ok(errors->fields().front().type);
}

LibraryEventsRegistry::ResultEmitterEvents
LibraryEventsRegistry::checkEmitterEvents(const Type *type, llvm::StringRef functionName,
                                          bool target) {
  const auto *events = this->state.library.emitters.eventMapOf(type);
  if (events == nullptr) {
    auto message = ErrorTextEventsFunction::notEmitter(functionName, target, type->name());

    return err(LibraryEventsRegistryEmitterEventsError{
        .code = LibraryEventsRegistryEmitterEventsErrorCode::NOT_AN_EMITTER,
        .message = message,
    });
  }

  return ok(events);
}

bool LibraryEventsRegistry::isEventTarget(const Type *type) const {
  const auto *target =
      this->state.library.declarations.findLibraryClass(LibraryClass::EVENT_TARGET);
  const auto *classDecl = type->classDecl();

  return target != nullptr && classDecl != nullptr && classDecl->derivesFrom(target);
}

bool LibraryEventsRegistry::hasTimeoutOption(const CallExpr *call) const {
  if (call->args.size() != 3) {
    return false;
  }
  auto *options = dyn_cast<ObjectLitExpr>(call->args[2]);
  if (options == nullptr) {
    return false;
  }
  for (const ObjectLitExpr::Property &property : options->props) {
    if (!property.spread() && NodeOptionTable::find(property.name) == NodeOption::TIMEOUT) {
      return true;
    }
  }

  return false;
}

bool LibraryEventsRegistry::isWaitEnding(const CallExpr *call) const {
  const auto *source = call->args[0]->type->classDecl();
  const ClassDecl *root =
      source != nullptr && source->generic() != nullptr ? source->generic() : source;
  bool isEmitterRoot =
      root != nullptr && (LibraryClassTable::isName(root->name, LibraryClass::EVENT_EMITTER) ||
                          LibraryClassTable::isName(root->name, LibraryClass::EVENT_EMITTER_BASE));
  bool isEnding = root != nullptr && root->library() && !isEmitterRoot;
  bool isTopLevel = this->state.walk.functions.getCurrentDecl() == nullptr &&
                    !this->state.walk.functions.isInsideAwaitLoop();
  bool isRunningOnce = isTopLevel || this->isOwningEmitter(call->args[0]);

  return isEnding || isRunningOnce;
}

}
