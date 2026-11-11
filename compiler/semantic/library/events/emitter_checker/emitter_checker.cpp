#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int kReceiverBit = 16;

constexpr int kDedupeBit = 32;

}

EmitterChecker::EmitterChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->eventMaps = &all.eventMaps;
  this->fieldFacts = &all.fieldFacts;
  this->typeParams = &all.typeParams;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->eventMapInference = &all.eventMapInference;
  this->methodCallChecker = &all.methodCallChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

std::optional<std::string> EmitterChecker::chainText(const Expr *expr) {
  std::vector<llvm::StringRef> names;
  const Expr *link = expr;
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = member->object;
  }
  std::optional<std::string> root = EmitterChecker::rootText(link);
  if (!root.has_value()) {
    return std::nullopt;
  }
  std::string text = *root;
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

std::optional<std::string> EmitterChecker::rootText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  auto *call = dyn_cast<CallExpr>(expr);
  auto *callee = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  if (callee == nullptr) {
    return std::nullopt;
  }
  auto *fd = call->args.size() == 1 ? dyn_cast<IntLitExpr>(call->args[0]) : nullptr;
  if (callee->name == "processStdin" && call->args.empty()) {
    return "process.stdin";
  }
  if (callee->name == "processStdout" && fd != nullptr) {
    return fd->value == 2 ? "process.stderr" : "process.stdout";
  }

  return std::nullopt;
}

EmitterChecker::EmitterMethod EmitterChecker::emitterMethod(llvm::StringRef name) {
  if (name == "on" || name == "addListener" || name == "prependListener" || name == "once" ||
      name == "prependOnceListener") {
    return EmitterMethod::Listen;
  }
  if (name == "off" || name == "removeListener") {
    return EmitterMethod::Remove;
  }
  if (name == "emit") {
    return EmitterMethod::Emit;
  }
  if (name == "listeners") {
    return EmitterMethod::Listeners;
  }
  if (name == "rawListeners") {
    return EmitterMethod::RawListeners;
  }
  if (name == "listenerCount") {
    return EmitterMethod::Count;
  }
  if (name == "removeAllListeners") {
    return EmitterMethod::RemoveAll;
  }

  return EmitterMethod::None;
}

void EmitterChecker::noteListener(MemberExpr *member, const Type *events, llvm::StringRef event) {
  if (event == "error") {
    this->state.getEventStore().markErrorListened(events);
  }
  if (events->isRecord()) {
    return;
  }
  const Type *errors = this->eventMaps->findEventArgs(events, "error");
  if (event != "error" && errors == nullptr) {
    return;
  }
  const Expr *root = member->object;
  while (true) {
    while (auto *conv = dyn_cast<CastExpr>(root)) {
      if (!conv->isImplicit()) {
        break;
      }
      root = conv->operand;
    }
    auto *inner = dyn_cast<CallExpr>(root);
    auto *chained = inner ? dyn_cast<MemberExpr>(inner->callee) : nullptr;
    if (!chained || EmitterChecker::emitterMethod(chained->name) != EmitterMethod::Listen) {
      break;
    }
    root = chained->object;
  }
  EmitterKey key = this->keyOf(root);
  if (event == "error") {
    this->state.getFlowState().listenForErrors(key);
    return;
  }
  this->state.getFlowState().addPendingErrors(key, member->loc());
}

bool EmitterChecker::canReceive(const Type *type) {
  return type && type->isObject() && !type->isAnyObject() && !type->isTuple() &&
         !type->isWeakRef() && !type->isRegExp() && !type->isMatch() && !type->isDate() &&
         !type->isIntl() && !type->isRuntimeObject() && !type->isCrypto() && !type->isTimer() &&
         !type->isBytes();
}

bool EmitterChecker::ownsEmitter(const Expr *emitter) const {
  while (true) {
    if (auto *conv = dyn_cast<CastExpr>(emitter); conv && conv->isImplicit()) {
      emitter = conv->operand;
      continue;
    }
    auto *call = dyn_cast<CallExpr>(emitter);
    auto *chained = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
    if (chained && EmitterChecker::emitterMethod(chained->name) == EmitterMethod::Listen) {
      emitter = chained->object;
      continue;
    }
    break;
  }
  if (isa<NewExpr>(emitter)) {
    return true;
  }
  auto *name = dyn_cast<NameExpr>(emitter);
  const FunctionDecl *fn = this->state.getFunctionStack().getCurrentDecl();
  if (!name || !name->var() || !fn || name->var()->owner() != fn || name->var()->isGlobal() ||
      name->var() == fn->thisVar()) {
    return false;
  }

  return llvm::none_of(fn->params, [&](const Param &p) { return p.sym == name->var(); });
}

bool EmitterChecker::classifyListener(Expr *arg, const Expr *emitter, int &bits, bool timer) {
  Expr *e = arg;
  auto *conv = dyn_cast<CastExpr>(e);
  while (conv != nullptr && conv->isImplicit()) {
    e = conv->operand;
    conv = dyn_cast<CastExpr>(e);
  }
  bool topLevel = (this->state.getFunctionStack().getCurrentDecl() == nullptr &&
                   !this->state.getFunctionStack().isInsideAwaitLoop()) ||
                  (emitter != nullptr && this->ownsEmitter(emitter));
  ListenerShape shape = EmitterChecker::listenerShape(e);
  switch (shape.form) {
    case ListenerForm::Free:
      bits |= topLevel ? 0 : kDedupeBit;
      return true;
    case ListenerForm::Receiver:
      if (shape.envFn != nullptr) {
        shape.envFn->receiverEnv = shape.var;
      }
      return this->addListenerReceiver(shape, topLevel, bits);
    case ListenerForm::Captures:
      return topLevel || this->reportsCapturesKept(arg, shape.captured, timer);
    case ListenerForm::Unknown:
      return topLevel || this->reportsUnknownListener(arg, timer);
  }

  return false;
}

EmitterChecker::ListenerShape EmitterChecker::listenerShape(Expr *e) {
  auto *fe = dyn_cast<FunctionExpr>(e);
  if (fe != nullptr) {
    return EmitterChecker::closureShape(fe->fn);
  }
  auto *member = dyn_cast<MemberExpr>(e);
  bool method = member != nullptr && member->method() != nullptr &&
                member->method()->decl != nullptr && !member->method()->decl->isStatic() &&
                EmitterChecker::canReceive(member->object->type);
  if (method) {
    auto *self = dyn_cast<NameExpr>(member->object);
    return ListenerShape{
        .form = ListenerForm::Receiver,
        .type = member->object->type,
        .var = self != nullptr ? self->var() : nullptr,
        .envFn = nullptr,
        .captured = {},
    };
  }
  auto *name = dyn_cast<NameExpr>(e);
  if (name != nullptr && name->func() != nullptr && name->func()->decl != nullptr) {
    return EmitterChecker::functionShape(name->func()->decl);
  }

  return ListenerShape{
      .form = ListenerForm::Unknown,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = {},
  };
}

EmitterChecker::ListenerShape EmitterChecker::closureShape(FunctionDecl *fn) {
  if (fn->captures.empty()) {
    return ListenerShape{
        .form = ListenerForm::Free,
        .type = nullptr,
        .var = nullptr,
        .envFn = nullptr,
        .captured = {},
    };
  }
  const VarSymbol *only = fn->captures[0];
  bool single = fn->captures.size() == 1 && fn->group == nullptr && !fn->isGenerator &&
                !only->boxed() && EmitterChecker::canReceive(only->type);
  if (single) {
    return ListenerShape{
        .form = ListenerForm::Receiver,
        .type = only->type,
        .var = only,
        .envFn = fn,
        .captured = {},
    };
  }

  return ListenerShape{
      .form = ListenerForm::Captures,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = std::vector<const VarSymbol *>(fn->captures.begin(), fn->captures.end()),
  };
}

EmitterChecker::ListenerShape EmitterChecker::functionShape(FunctionDecl *fn) {
  const std::vector<VarSymbol *> &captures =
      fn->group != nullptr ? fn->group->captures : fn->captures;
  ListenerForm form = captures.empty() ? ListenerForm::Free : ListenerForm::Captures;

  return ListenerShape{
      .form = form,
      .type = nullptr,
      .var = nullptr,
      .envFn = nullptr,
      .captured = std::vector<const VarSymbol *>(captures.begin(), captures.end()),
  };
}

bool EmitterChecker::addListenerReceiver(const ListenerShape &shape, bool topLevel, int &bits) {
  bits |= kReceiverBit | (topLevel ? 0 : kDedupeBit);
  this->state.getInventoryStore().addListenerReceiver(shape.type);
  const FunctionDecl *fn = this->state.getFunctionStack().getCurrentDecl();
  bool self = fn != nullptr && fn->isConstructor() && fn->cls() != nullptr &&
              shape.var != nullptr && shape.var == fn->thisVar();
  if (self) {
    this->state.getEventStore().addSelfListener(fn->cls());
  }

  return true;
}

bool EmitterChecker::reportsUnknownListener(const Expr *arg, bool timer) {
  std::string message = ErrorTextListenerLife::unknown(timer);
  this->state.getDiagnostics().error(arg->loc(), message);

  return false;
}

bool EmitterChecker::reportsCapturesKept(const Expr *arg,
                                         const std::vector<const VarSymbol *> &captured,
                                         bool timer) {
  for (const VarSymbol *var : captured) {
    if (var->boxed() || !EmitterChecker::canReceive(var->type)) {
      std::string message = ErrorTextListenerLife::keeps(timer, var->name);
      this->state.getDiagnostics().error(arg->loc(), message);
      return false;
    }
  }
  const VarSymbol *kept = captured[0];
  for (const VarSymbol *var : captured) {
    if (var->name == "this") {
      kept = var;
    }
  }
  std::vector<const VarSymbol *> others;
  for (const VarSymbol *var : captured) {
    if (var != kept) {
      others.push_back(var);
    }
  }
  std::string message = ErrorTextListenerLife::captures(timer, captured, others, kept->name);
  this->state.getDiagnostics().error(arg->loc(), message);

  return false;
}

EmitterKey EmitterChecker::keyOf(const Expr *root) const {
  while (auto *conv = dyn_cast<CastExpr>(root)) {
    if (!conv->isImplicit()) {
      break;
    }
    root = conv->operand;
  }
  EmitterKey key;
  if (auto *name = dyn_cast<NameExpr>(root)) {
    const VarSymbol *var = name->var() ? name->var() : this->scopes->lookup(name->name).var();
    if (var) {
      key.path.var = var;
      return key;
    }
  }
  if (std::optional<FieldPath> path = this->fieldFacts->fieldPath(root)) {
    key.path = *path;
  } else if (std::optional<std::string> text = EmitterChecker::chainText(root)) {
    key.text = *text;
  } else {
    key.root = root;
  }

  return key;
}

void EmitterChecker::noteErrorsHandled(const Expr *emitter) {
  if (emitter->type) {
    if (const Type *events = this->eventMaps->eventMapOf(emitter->type)) {
      this->state.getEventStore().markErrorListened(events);
    }
  }
  EmitterKey key = this->keyOf(emitter);
  this->state.getFlowState().listenForErrors(key);
}

std::string EmitterChecker::emitterPath(const EmitterKey &key) const {
  if (!key.text.empty()) {
    return key.text;
  }
  if (!key.path.var) {
    return "";
  }
  std::string name = key.path.var->name.str();
  for (llvm::StringRef part : key.path.names) {
    name += "." + part.str();
  }

  return name;
}

void EmitterChecker::reportUnlistenedErrors() {
  bool muted = this->state.getDiagnostics().isMuted();
  for (const auto &[key, loc] : this->state.getFlowState().takePendingErrors()) {
    EventStore &store = this->state.getEventStore();
    if (muted || store.isErrorSubscriptionReported(loc)) {
      continue;
    }
    store.markErrorSubscriptionReported(loc);
    std::string path = this->emitterPath(key);
    std::string message = ErrorTextUnlistened::mayEmit(path);
    this->state.getDiagnostics().error(loc, message);
  }
}

void EmitterChecker::checkDroppedListeners() {
  EventStore &store = this->state.getEventStore();
  for (const DroppedNew &dropped : store.getDroppedNews()) {
    const ClassDecl *cls = dropped.cls;
    llvm::SMLoc loc = dropped.loc;
    for (const ClassDecl *c = cls; c; c = c->parent) {
      if (store.isSelfListener(c)) {
        std::string message = ErrorTextEmitterNew::dropped(cls->name);
        this->state.getDiagnostics().error(loc, message);
        break;
      }
    }
  }
}

void EmitterChecker::checkErrorListeners() {
  EventStore &store = this->state.getEventStore();
  for (const ErrorEmit &emit : store.getErrorEmits()) {
    if (!store.isErrorListened(emit.events)) {
      std::string message = ErrorTextUnlistened::unheard();
      this->state.getDiagnostics().error(emit.loc, message);
    }
  }
}

Expr *EmitterChecker::checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }

    return expr;
  };
  const Type *events = nullptr;
  if (!expr->typeArgs.empty()) {
    if (expr->typeArgs.size() == 1) {
      events = this->typeResolver->resolveType(expr->typeArgs[0], false);
    }
  } else if (const Type *want = expected ? expected->nonOptional() : nullptr;
             want && ClassDeclarer::classOf(want) &&
             ClassDeclarer::classOf(want)->generic() == generic) {
    events = ClassDeclarer::classOf(want)->typeArgs[0];
  } else {
    events = this->eventMapInference->newEventMap(expr->loc());
  }
  if (!events) {
    return nullptr;
  }
  if (events->isError()) {
    return checkArgsLoosely();
  }
  GenericClassInstantiator::ResultClassMade made =
      this->genericClassInstantiator->instantiateClass(generic, {events}, expr->loc());
  if (made.isErr() &&
      made.error().code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    llvm::SMLoc at =
        made.error().code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES
            ? generic->loc()
            : expr->loc();
    this->state.getDiagnostics().error(at, made.error().message);
  }
  ClassDecl *inst = made.isOk() ? made.value() : nullptr;
  if (!inst) {
    return checkArgsLoosely();
  }
  expr->typeArgs.clear();
  return this->methodCallChecker->checkClassNew(expr, inst, expected);
}

void EmitterChecker::checkLooseArgs(CallExpr *call, size_t from) {
  for (size_t i = from; i < call->args.size(); ++i) {
    call->args[i] = this->exprChecker->checkExpr(call->args[i]);
  }
}

bool EmitterChecker::isOwnEmit(const EventSite *site, const SentEmit &emit) const {
  if (emit.super) {
    return true;
  }
  const Expr *object = emit.member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit()) {
      break;
    }
    object = conv->operand;
  }
  auto *self = dyn_cast<NameExpr>(object);
  if (self != nullptr && self->name == "this") {
    return true;
  }
  Module *module = this->eventMapInference->findModuleAt(emit.call->loc());

  return module != nullptr && module == site->owner;
}

const Type *EmitterChecker::inferredEmitArgs(EventSite *site, const std::string &event,
                                             llvm::SMLoc loc, std::string &first) {
  EventRecord *record = this->eventMapInference->completeEvent(site, event);
  if (record->args != nullptr) {
    first = record->where;
    return record->args;
  }
  std::string owner =
      site->owner != nullptr ? llvm::sys::path::filename(site->owner->path).str() : "its module";
  std::string message = ErrorTextEmitted::foreign(event, owner);
  if (record->state == EventState::COMPLETING) {
    message = ErrorTextEventName::circular(event);
  } else if (!record->untyped.empty()) {
    message = ErrorTextEventName::untyped(event, record->untyped);
  }
  this->state.getDiagnostics().error(loc, message);

  return nullptr;
}

Expr *EmitterChecker::collectEmitterCall(const SentEmit &emit, EventSite *site,
                                         const std::string &event, bool library) {
  CallExpr *call = emit.call;
  MemberExpr *member = emit.member;
  call->type = Type::boolTy();
  size_t count = call->args.size() - 1;
  if (count > 1) {
    std::string message = ErrorTextEmitted::oneValue(event);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->checkLooseArgs(call, 1);
    return call;
  }
  EventRecord *record = EventMapInference::findEvent(site, event);
  if (record == nullptr) {
    record = this->eventMapInference->addEvent(site, event);
  }
  const Type *before = record->args;
  if (before != nullptr && before->fields().size() != count) {
    std::string message = ErrorTextEmitted::countAt(event, before->name(), record->where, count);
    this->state.getDiagnostics().error(member->loc(), message);
    this->checkLooseArgs(call, 1);
    return call;
  }
  std::vector<const Type *> elements;
  if (count == 1) {
    std::optional<const Type *> element = this->emittedValueType(call, record, event);
    if (!element.has_value()) {
      return call;
    }
    elements.push_back(*element);
  }
  const Type *args = Type::tuple(elements);
  if (args != before) {
    this->eventMapInference->rememberEventArgs(site, record, args, call->args[0]->loc());
  }
  if (this->state.getFunctionStack().isInsideTrial()) {
    return call;
  }
  if (event == "error" && !library) {
    this->state.getEventStore().addErrorEmit(site->map, member->loc());
  }
  if (this->state.getEventStore().hasFinishedSentEmits()) {
    this->finishEmit(emit, args);
    return call;
  }
  record->sent.push_back(emit);

  return call;
}

std::optional<const Type *> EmitterChecker::emittedValueType(CallExpr *call,
                                                             const EventRecord *record,
                                                             const std::string &event) {
  const Type *before = record->args;
  const Type *wanted = before != nullptr ? before->fields()[0].type : nullptr;
  call->args[1] = this->exprChecker->checkExpr(call->args[1], wanted);
  const Type *type = call->args[1]->type;
  if (type->isVoid()) {
    std::string message = ErrorTextEmitted::voidValue(event);
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return std::nullopt;
  }
  if (type->isError() || TypeQueries::mentionsParam(type)) {
    return std::nullopt;
  }
  const Type *found = type->isLiteral() ? type->literalBase() : type;
  bool fits = wanted != nullptr && TypeRelations::isImplicitlyConvertible(type, wanted);
  const Type *wider = wanted != nullptr && !fits ? TypeRelations::unify(wanted, found) : nullptr;
  const Type *element = fits ? wanted : wanted != nullptr ? wider : found;
  if (element == nullptr) {
    std::string message =
        ErrorTextEmitted::typeAt(event, before->name(), record->where, type->name());
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    return std::nullopt;
  }

  return element;
}

void EmitterChecker::finishEmit(const SentEmit &emit, const Type *args) {
  CallExpr *call = emit.call;
  if (call->args.size() > 1) {
    call->args[1] = this->typeRelations->coerce(call->args[1], args->fields()[0].type);
  }
  std::vector<bool> given(call->args.size(), true);
  this->callLibraryMethod(call, emit.member, emit.cls, "emit", {args}, &given, emit.super);
}

void EmitterChecker::finishSentEmits() {
  EventStore &store = this->state.getEventStore();
  store.markSentEmitsFinished();
  for (EventSite *site : store.getSites()) {
    for (EventRecord &record : site->events) {
      record.state = EventState::DONE;
      for (const SentEmit &emit : record.sent) {
        this->finishEmit(emit, record.args);
      }
      record.sent.clear();
    }
  }
}

Expr *EmitterChecker::callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                        llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                                        const std::vector<bool> *given, bool super) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  assert(!methods.empty() && "a method of the emitter's code");
  FuncSymbol *sym = methods[0]->sym;
  llvm::SMLoc loc = member->loc();
  std::vector<const Type *> params = sym->params;
  const Type *ret = sym->getReturnType();
  FuncSymbol *callee = sym;
  if (sym->isGeneric()) {
    this->state.getGenericStore().enterInstanceLoc(loc);
    for (const Type *&p : params) {
      p = this->typeParams->substitute(p, sym->typeParams, bound);
    }
    ret = ret ? this->typeParams->substitute(ret, sym->typeParams, bound) : Type::error();
    this->state.getGenericStore().leaveInstanceLoc();
    if (!this->state.getFunctionStack().isInsideTrial() &&
        llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); })) {
      if (FuncSymbol *instance = this->genericInstantiator->instantiate(sym, bound, loc)) {
        callee = instance;
      }
    }
  }
  if (super) {
    call->target = SuperCall{.method = callee};
  } else if (sym->isGeneric()) {
    call->target = MethodCall{.method = callee};
  } else if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->target = MethodCall{.method = impl->sym};
  } else {
    call->target = MethodCall{.method = sym, .virtualCall = true};
  }
  member->type = this->returnInferrer->functionType(callee);
  call->type = ret ? ret : Type::error();
  if (callee != sym) {
    return this->argumentChecker->checkArguments(call, callee->params, callee->minArgs,
                                                 callee->hasRest, loc, given);
  }

  return this->argumentChecker->checkArguments(call, params, sym->minArgs, sym->hasRest, loc,
                                               given);
}

Expr *EmitterChecker::checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                       bool super) {
  EmitterMethod kind = EmitterChecker::emitterMethod(member->name);
  bool library = this->libraryCalls->inLibrary(call->loc());
  if (kind == EmitterMethod::None || (library && kind != EmitterMethod::Emit)) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, false);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  const Type *events = this->eventMaps->eventMapOf(receiver);
  if (events == nullptr || (library && TypeQueries::mentionsParam(events))) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    std::string message = ErrorTextEmitted::typeArgs(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->typeArgs.clear();
  }
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  EmitterCall emitter{
      .call = call,
      .member = member,
      .cls = cls,
      .receiver = receiver,
      .events = events,
      .super = super,
  };
  switch (kind) {
    case EmitterMethod::Emit:
      return this->checkEmit(emitter, library);
    case EmitterMethod::Listen:
    case EmitterMethod::Remove:
      return this->checkListenCall(emitter, kind);
    case EmitterMethod::Listeners:
    case EmitterMethod::RawListeners:
      return this->checkListenersCall(emitter, kind);
    case EmitterMethod::Count:
      return this->checkCountCall(emitter);
    case EmitterMethod::RemoveAll:
      return this->checkRemoveAllCall(emitter);
    case EmitterMethod::None:
      return nullptr;
  }

  return nullptr;
}

bool EmitterChecker::hasWrongArity(const EmitterCall &emitter, unsigned min, unsigned max) {
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(emitter.call, min, max);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(emitter.member->loc(), arity.error().message);
  }

  return arity.isErr();
}

Expr *EmitterChecker::checkEmit(const EmitterCall &emitter, bool library) {
  CallExpr *call = emitter.call;
  if (call->args.empty()) {
    std::string message = ErrorTextEmitted::noName();
    this->state.getDiagnostics().error(emitter.member->loc(), message);
    return call;
  }
  EventSite *site = this->state.getEventStore().findSiteOfMap(emitter.events);
  if (site != nullptr) {
    return this->checkSiteEmit(emitter, site, library);
  }
  std::string event;
  const Type *args =
      this->eventMaps->eventArgs(emitter.events, call->args[0], emitter.member->name, &event);
  EmitShape shape{
      .args = args,
      .event = event,
      .first = "",
  };

  return this->checkEmitShape(emitter, shape, library);
}

Expr *EmitterChecker::checkSiteEmit(const EmitterCall &emitter, EventSite *site, bool library) {
  CallExpr *call = emitter.call;
  std::optional<std::string> key = this->eventMaps->eventKey(call->args[0], emitter.member->name);
  if (!key.has_value()) {
    this->checkLooseArgs(call, 1);
    return call;
  }
  SentEmit emit{
      .call = call,
      .member = emitter.member,
      .cls = emitter.cls,
      .super = emitter.super,
  };
  EventRecord *record = EventMapInference::findEvent(site, *key);
  bool service = EventMaps::isServiceEvent(*key);
  if (!service && this->isOwnEmit(site, emit) && !EventMapInference::isFrozen(record)) {
    return this->collectEmitterCall(emit, site, *key, library);
  }
  std::string first;
  const Type *args = service ? EventMaps::serviceEventArgs()
                             : this->inferredEmitArgs(site, *key, call->args[0]->loc(), first);
  EmitShape shape{
      .args = args,
      .event = *key,
      .first = first,
  };

  return this->checkEmitShape(emitter, shape, library);
}

Expr *EmitterChecker::checkEmitShape(const EmitterCall &emitter, const EmitShape &shape,
                                     bool library) {
  CallExpr *call = emitter.call;
  MemberExpr *member = emitter.member;
  if (shape.args == nullptr) {
    this->checkLooseArgs(call, 1);
    return call;
  }
  size_t count = call->args.size() - 1;
  if (count > 1) {
    std::string message = ErrorTextEmitted::oneValue(shape.event);
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->checkLooseArgs(call, 1);
    return call;
  }
  size_t min = shape.args->tupleMin();
  size_t max = shape.args->tupleRest() ? ~size_t(0) : shape.args->fields().size();
  if (count < min || count > max) {
    std::string message =
        !shape.first.empty()
            ? ErrorTextEmitted::countAt(shape.event, shape.args->name(), shape.first, count)
            : ErrorTextEmitted::arity(shape.event, shape.args->name(), min, max, count);
    this->state.getDiagnostics().error(member->loc(), message);
    this->checkLooseArgs(call, 1);
    return call;
  }
  if (shape.event == "error" && !library) {
    this->state.getEventStore().addErrorEmit(emitter.events, member->loc());
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = true;
  if (!shape.first.empty() && !this->emitValuesFit(call, shape, given)) {
    return call;
  }

  return this->callLibraryMethod(call, member, emitter.cls, "emit", {shape.args}, &given,
                                 emitter.super);
}

bool EmitterChecker::emitValuesFit(CallExpr *call, const EmitShape &shape,
                                   std::vector<bool> &given) {
  for (size_t i = 1; i < call->args.size() && i - 1 < shape.args->fields().size(); ++i) {
    const Type *wanted = shape.args->fields()[i - 1].type;
    call->args[i] = this->exprChecker->checkExpr(call->args[i], wanted);
    const Type *type = call->args[i]->type;
    if (!type->isError() && !TypeRelations::isImplicitlyConvertible(type, wanted)) {
      std::string message =
          ErrorTextEmitted::typeAt(shape.event, shape.args->name(), shape.first, type->name());
      this->state.getDiagnostics().error(call->args[i]->loc(), message);
      this->checkLooseArgs(call, i + 1);
      return false;
    }
    call->args[i] = this->typeRelations->coerce(call->args[i], wanted);
    given[i] = true;
  }

  return true;
}

Expr *EmitterChecker::checkListenCall(const EmitterCall &emitter, EmitterMethod kind) {
  CallExpr *call = emitter.call;
  MemberExpr *member = emitter.member;
  if (this->hasWrongArity(emitter, 2, 2)) {
    return call;
  }
  std::string event;
  const Type *args =
      this->eventMaps->eventArgs(emitter.events, call->args[0], member->name, &event);
  if (args == nullptr) {
    this->checkLooseArgs(call, 1);
    return call;
  }
  ListenerKind listener = ListenerKind::Plain;
  call->args[1] =
      this->eventMaps->checkListener(call->args[1], args, event, emitter.receiver, listener);
  if (call->args[1]->type->isError()) {
    return call;
  }
  if (kind == EmitterMethod::Listen) {
    bool kept = this->addListenerBits(emitter, event, listener);
    if (!kept) {
      return call;
    }
  } else {
    this->state.getInventoryStore().markComparesFunctions();
  }
  std::vector<bool> given(call->args.size(), true);
  this->callLibraryMethod(call, member, emitter.cls, member->name, {}, &given, emitter.super);
  call->type = emitter.receiver;

  return call;
}

bool EmitterChecker::addListenerBits(const EmitterCall &emitter, llvm::StringRef event,
                                     ListenerKind listener) {
  CallExpr *call = emitter.call;
  this->noteListener(emitter.member, emitter.events, event);
  int bits = int(listener);
  if (!this->classifyListener(call->args[1], emitter.member->object, bits)) {
    return false;
  }
  auto *flags = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(bits));
  call->args.push_back(this->exprChecker->checkExpr(flags, Type::intTy()));

  return true;
}

Expr *EmitterChecker::checkListenersCall(const EmitterCall &emitter, EmitterMethod kind) {
  CallExpr *call = emitter.call;
  if (this->hasWrongArity(emitter, 1, 1)) {
    return call;
  }
  std::string event;
  const Type *args =
      this->eventMaps->eventArgs(emitter.events, call->args[0], emitter.member->name, &event);
  if (args == nullptr) {
    return call;
  }
  auto *raw =
      this->state.getAst().make<BoolLitExpr>(call->loc(), kind == EmitterMethod::RawListeners);
  call->args.push_back(this->exprChecker->checkExpr(raw));
  std::vector<bool> given(call->args.size(), true);
  this->callLibraryMethod(call, emitter.member, emitter.cls, "listenersOf", {args}, &given,
                          emitter.super);
  call->type = Type::array(EventMaps::listenerType(args, Type::voidTy()));

  return call;
}

Expr *EmitterChecker::checkCountCall(const EmitterCall &emitter) {
  CallExpr *call = emitter.call;
  MemberExpr *member = emitter.member;
  if (this->hasWrongArity(emitter, 1, 2)) {
    return call;
  }
  std::string event;
  const Type *args =
      this->eventMaps->eventArgs(emitter.events, call->args[0], member->name, &event);
  if (args == nullptr) {
    this->checkLooseArgs(call, 1);
    return call;
  }
  if (call->args.size() == 2) {
    ListenerKind listener = ListenerKind::Plain;
    call->args[1] =
        this->eventMaps->checkListener(call->args[1], args, event, emitter.receiver, listener);
    if (call->args[1]->type->isError()) {
      return call;
    }
    this->state.getInventoryStore().markComparesFunctions();
  }
  std::vector<bool> given(call->args.size(), true);

  return this->callLibraryMethod(call, member, emitter.cls, member->name, {}, &given,
                                 emitter.super);
}

Expr *EmitterChecker::checkRemoveAllCall(const EmitterCall &emitter) {
  CallExpr *call = emitter.call;
  MemberExpr *member = emitter.member;
  if (this->hasWrongArity(emitter, 0, 1)) {
    return call;
  }
  std::string event;
  bool unknown = !call->args.empty() && this->eventMaps->eventArgs(emitter.events, call->args[0],
                                                                   member->name, &event) == nullptr;
  if (unknown) {
    return call;
  }
  std::vector<bool> given(call->args.size(), true);
  this->callLibraryMethod(call, member, emitter.cls, member->name, {}, &given, emitter.super);
  call->type = emitter.receiver;

  return call;
}

void EmitterChecker::reportEmitterMethodValue(MemberExpr *member) {
  std::string message = ErrorTextEmitted::methodVal(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
}

}
