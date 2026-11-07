#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/flow/field_facts/field_facts.h"
#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker.h"
#include "compiler/pipeline/check_program/topics/events/event_map_inference/event_map_inference.h"
#include "compiler/pipeline/check_program/topics/events/event_maps/event_maps.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
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

std::string EmitterChecker::chainText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *call = dyn_cast<CallExpr>(expr); call && isa<NameExpr>(call->callee)) {
    llvm::StringRef fn = cast<NameExpr>(call->callee)->name;
    auto *fd = call->args.size() == 1 ? dyn_cast<IntLitExpr>(call->args[0]) : nullptr;
    if (fn == "processStdin" && call->args.empty()) {
      return "process.stdin";
    }
    if (fn == "processStdout" && fd) {
      return fd->value == 2 ? "process.stderr" : "process.stdout";
    }
    return "";
  }
  if (auto *member = dyn_cast<MemberExpr>(expr); member && !member->optional) {
    std::string object = EmitterChecker::chainText(member->object);
    return object.empty() ? "" : object + "." + member->name.str();
  }

  return "";
}

bool EmitterChecker::isServiceEvent(llvm::StringRef name) {
  return name == "newListener" || name == "removeListener";
}

std::string EmitterChecker::fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

std::string EmitterChecker::shortPlace(const std::string &place) {
  std::string file = EmitterChecker::fileOfPlace(place);

  return llvm::sys::path::filename(file).str() + place.substr(file.size());
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

EmitterChecker::EmitterChecker(CheckState &owner)
    : state(owner) {}

void EmitterChecker::noteListener(MemberExpr *member, const Type *events, llvm::StringRef event) {
  if (event == "error") {
    this->state.getEventStore().markErrorListened(events);
  }
  if (events->isRecord() || (event != "error" && !events->field("error"))) {
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
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  bool topLevel = (!this->state.getFunctionStack().getCurrentDecl() &&
                   !this->state.getFunctionStack().isInsideAwaitLoop()) ||
                  (emitter && this->ownsEmitter(emitter));
  auto receiver = [&](const Type *type, const VarSymbol *var) {
    bits |= kReceiverBit | (topLevel ? 0 : kDedupeBit);
    this->state.getProgramUse().addListenerReceiver(type);
    if (const FunctionDecl *fn = this->state.getFunctionStack().getCurrentDecl();
        fn && fn->isConstructor() && fn->cls() && var && var == fn->thisVar()) {
      this->state.getEventStore().addSelfListener(fn->cls());
    }
    return true;
  };
  std::vector<const VarSymbol *> captured;
  bool known = true;
  if (auto *fe = dyn_cast<FunctionExpr>(e)) {
    FunctionDecl *fn = fe->fn;
    if (fn->captures.empty()) {
      bits |= topLevel ? 0 : kDedupeBit;
      return true;
    }
    if (const VarSymbol *only = fn->captures[0]; fn->captures.size() == 1 && !fn->group &&
                                                 !fn->isGenerator && !only->boxed() &&
                                                 EmitterChecker::canReceive(only->type)) {
      fn->receiverEnv = only;
      return receiver(only->type, only);
    }
    captured.assign(fn->captures.begin(), fn->captures.end());
  } else if (auto *member = dyn_cast<MemberExpr>(e);
             member && member->method() && member->method()->decl &&
             !member->method()->decl->isStatic() &&
             EmitterChecker::canReceive(member->object->type)) {
    auto *self = dyn_cast<NameExpr>(member->object);
    return receiver(member->object->type, self ? self->var() : nullptr);
  } else if (auto *name = dyn_cast<NameExpr>(e); name && name->func() && name->func()->decl) {
    const FunctionDecl *fn = name->func()->decl;
    const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;
    if (captures.empty()) {
      bits |= topLevel ? 0 : kDedupeBit;
      return true;
    }
    captured.assign(captures.begin(), captures.end());
  } else {
    known = false;
  }
  if (topLevel) {
    return true;
  }
  if (!known) {
    std::string message = ErrorTextListenerLife::unknown(timer);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  auto bad = llvm::find_if(captured, [](const VarSymbol *var) {
    return var->boxed() || !EmitterChecker::canReceive(var->type);
  });
  if (bad != captured.end()) {
    std::string message = ErrorTextListenerLife::keeps(timer, (*bad)->name);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
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
    const VarSymbol *var = name->var() ? name->var() : Scopes(this->state).lookup(name->name).var();
    if (var) {
      key.path.var = var;
      return key;
    }
  }
  if (std::optional<FieldPath> path = FieldFacts(this->state).fieldPath(root)) {
    key.path = *path;
  } else if (std::string text = EmitterChecker::chainText(root); !text.empty()) {
    key.text = text;
  } else {
    key.root = root;
  }

  return key;
}

void EmitterChecker::noteErrorsHandled(const Expr *emitter) {
  if (emitter->type) {
    if (const Type *events = EventMaps(this->state).eventMapOf(emitter->type)) {
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
  for (const auto &[key, loc] : this->state.getFlowState().takePendingErrors()) {
    EventStore &store = this->state.getEventStore();
    if (store.isErrorSubscriptionReported(loc)) {
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
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return expr;
  };
  const Type *events = nullptr;
  if (!expr->typeArgs.empty()) {
    if (expr->typeArgs.size() == 1) {
      events = TypeResolver(this->state).resolveType(expr->typeArgs[0], false);
    }
  } else if (const Type *want = expected ? expected->nonOptional() : nullptr;
             want && ClassDeclarer::classOf(want) &&
             ClassDeclarer::classOf(want)->generic() == generic) {
    events = ClassDeclarer::classOf(want)->typeArgs[0];
  } else {
    events = EventMapInference(this->state)
                 .inferredEventMap("new " + EventMapInference(this->state).placeOf(expr->loc()));
    if (!events) {
      std::string message = ErrorTextEmitterNew::needsMap();
      this->state.getDiagnostics().error(expr->loc(), message);
      return checkArgsLoosely();
    }
  }
  if (!events) {
    return nullptr;
  }
  if (events->isError()) {
    return checkArgsLoosely();
  }
  GenericClassInstantiator::ResultClassMade made =
      GenericClassInstantiator(this->state).instantiateClass(generic, {events}, expr->loc());
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
  return MethodCallChecker(this->state).checkClassNew(expr, inst, expected);
}

Expr *EmitterChecker::collectEmitterCall(CallExpr *call, MemberExpr *member,
                                         const std::string &site) {
  call->type = Type::boolTy();
  member->type = Type::error();
  llvm::SMLoc loc = call->args.empty() ? member->loc() : call->args[0]->loc();
  if (call->args.size() > 2) {
    std::string message = ErrorTextEmitted::oneValue("message");
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  std::vector<const Type *> elements;
  bool known = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    call->args[i] = i == 0 ? ArgumentChecker(this->state).checkArg(call->args[i], Type::string())
                           : ExprChecker(this->state).checkExpr(call->args[i]);
    const Type *type = call->args[i]->type;
    if (type->isError() || type->isVoid() || TypeQueries::mentionsParam(type)) {
      known = false;
    }
    if (i > 0) {
      elements.push_back(type->isLiteral() ? type->literalBase() : type);
    }
  }
  std::optional<std::string> key =
      call->args.empty() ? std::nullopt : KeyedFieldChecker(this->state).constantKey(call->args[0]);
  if (!key || !known || EmitterChecker::isServiceEvent(*key)) {
    return call;
  }
  const Type *found = Type::tuple(elements);
  EventStore &store = this->state.getEventStore();
  if (const EventInference::Event *e = store.findInferredEvent(site, *key)) {
    const Type *before = EventMapInference(this->state).fromPortable(e->args, loc);
    if (!before || before == found || before->fields().size() != elements.size()) {
      return call;
    }
    std::vector<const Type *> wider;
    for (size_t i = 0; i < elements.size(); ++i) {
      const Type *both = TypeRelations::unify(before->fields()[i].type, elements[i]);
      if (!both) {
        return call;
      }
      wider.push_back(both);
    }
    if (const Type *now = Type::tuple(wider); now != before) {
      llvm::SmallPtrSet<const Type *, 8> seen;
      store.changeInferredEvent(site, *key, EventMapInference(this->state).portable(now, seen));
    }
    return call;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  store.addInferredEvent(site,
                         {*key, EventMapInference(this->state).portable(found, seen),
                          EmitterChecker::shortPlace(EventMapInference(this->state).placeOf(loc))});

  return call;
}

Expr *EmitterChecker::callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                        llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                                        const std::vector<bool> *given, bool super) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  assert(!methods.empty() && "a method of the emitter's code");
  FuncSymbol *sym = methods[0]->sym;
  ReturnInferrer(this->state).resolveParams(sym);
  llvm::SMLoc loc = member->loc();
  std::vector<const Type *> params = sym->params;
  const Type *ret = ReturnInferrer(this->state).returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric()) {
    this->state.getGenericStore().enterInstanceLoc(loc);
    for (const Type *&p : params) {
      p = TypeParams(this->state).substitute(p, sym->typeParams, bound);
    }
    ret = ret ? TypeParams(this->state).substitute(ret, sym->typeParams, bound) : Type::error();
    this->state.getGenericStore().leaveInstanceLoc();
    if (!this->state.getFunctionStack().isInsideTrial() &&
        llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); })) {
      if (FuncSymbol *instance = GenericInstantiator(this->state).instantiate(sym, bound, loc)) {
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
  member->type = ReturnInferrer(this->state).functionType(callee);
  call->type = ret ? ret : Type::error();
  if (callee != sym) {
    ReturnInferrer(this->state).resolveParams(callee);
    return ArgumentChecker(this->state)
        .checkArguments(call, callee->params, callee->minArgs, callee->hasRest, loc, given);
  }

  return ArgumentChecker(this->state)
      .checkArguments(call, params, sym->minArgs, sym->hasRest, loc, given);
}

Expr *EmitterChecker::checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                                       bool super) {
  EmitterMethod kind = EmitterChecker::emitterMethod(member->name);
  bool library = LibraryCalls(this->state).inLibrary(call->loc());
  if (kind == EmitterMethod::None || (library && kind != EmitterMethod::Emit)) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, false);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  const Type *events = EventMaps(this->state).eventMapOf(receiver);
  if (!events || (library && TypeQueries::mentionsParam(events))) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    std::string message = ErrorTextEmitted::typeArgs(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    member->typeArgs.clear();
  }
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
    }
  };
  auto flag = [&](bool value) {
    return ExprChecker(this->state)
        .checkExpr(this->state.getAst().make<BoolLitExpr>(call->loc(), value));
  };
  llvm::StringRef name = member->name;
  std::string event;
  switch (kind) {
  case EmitterMethod::Emit: {
    if (call->args.empty()) {
      std::string message = ErrorTextEmitted::noName();
      this->state.getDiagnostics().error(member->loc(), message);
      return call;
    }
    if (std::optional<std::string> site = this->state.getEventStore().findOpenMapSite(events)) {
      return this->collectEmitterCall(call, member, *site);
    }
    const Type *args = EventMaps(this->state).eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    size_t count = call->args.size() - 1;
    if (count > 1) {
      std::string message = ErrorTextEmitted::oneValue(event);
      this->state.getDiagnostics().error(call->args[2]->loc(), message);
      checkArgsLoosely(1);
      return call;
    }
    size_t min = args->tupleMin();
    size_t max = args->tupleRest() ? ~size_t(0) : args->fields().size();
    std::string first;
    if (std::optional<std::string> site = this->state.getEventStore().findInferredMapSite(events)) {
      for (const EventInference::Event &e : this->state.getEventStore().getInferredEvents(*site)) {
        if (e.name == event) {
          first = e.where;
        }
      }
    }
    if (count < min || count > max) {
      std::string message = !first.empty()
                                ? ErrorTextEmitted::countAt(event, args->name(), first, count)
                                : ErrorTextEmitted::arity(event, args->name(), min, max, count);
      this->state.getDiagnostics().error(member->loc(), message);
      checkArgsLoosely(1);
      return call;
    }
    if (event == "error" && !library) {
      this->state.getEventStore().addErrorEmit(events, member->loc());
    }
    std::vector<bool> given(call->args.size(), false);
    given[0] = true;
    if (!first.empty()) {
      for (size_t i = 1; i < call->args.size() && i - 1 < args->fields().size(); ++i) {
        const Type *wanted = args->fields()[i - 1].type;
        call->args[i] = ExprChecker(this->state).checkExpr(call->args[i], wanted);
        const Type *type = call->args[i]->type;
        if (!type->isError() && !TypeRelations::isImplicitlyConvertible(type, wanted)) {
          std::string message = ErrorTextEmitted::typeAt(event, args->name(), first, type->name());
          this->state.getDiagnostics().error(call->args[i]->loc(), message);
          checkArgsLoosely(i + 1);
          return call;
        }
        call->args[i] = TypeRelations(this->state).coerce(call->args[i], wanted);
        given[i] = true;
      }
    }
    return this->callLibraryMethod(call, member, cls, "emit", {args}, &given, super);
  }
  case EmitterMethod::Listen:
  case EmitterMethod::Remove: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 2, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    const Type *args = EventMaps(this->state).eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    ListenerKind listener = ListenerKind::Plain;
    call->args[1] =
        EventMaps(this->state).checkListener(call->args[1], args, event, receiver, listener);
    if (call->args[1]->type->isError()) {
      return call;
    }
    if (kind == EmitterMethod::Listen) {
      this->noteListener(member, events, event);
      int bits = int(listener);
      if (!this->classifyListener(call->args[1], member->object, bits)) {
        return call;
      }
      call->args.push_back(
          ExprChecker(this->state)
              .checkExpr(this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(bits)),
                         Type::intTy()));
    } else {
      this->state.getProgramUse().markComparesFunctions();
    }
    std::vector<bool> given(call->args.size(), true);
    this->callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::Listeners:
  case EmitterMethod::RawListeners: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    const Type *args = EventMaps(this->state).eventArgs(events, call->args[0], name, &event);
    if (!args) {
      return call;
    }
    call->args.push_back(flag(kind == EmitterMethod::RawListeners));
    std::vector<bool> given(call->args.size(), true);
    this->callLibraryMethod(call, member, cls, "listenersOf", {args}, &given, super);
    call->type = Type::array(EventMaps::listenerType(args, Type::voidTy()));
    return call;
  }
  case EmitterMethod::Count: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    const Type *args = EventMaps(this->state).eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    if (call->args.size() == 2) {
      ListenerKind listener = ListenerKind::Plain;
      call->args[1] =
          EventMaps(this->state).checkListener(call->args[1], args, event, receiver, listener);
      if (call->args[1]->type->isError()) {
        return call;
      }
      this->state.getProgramUse().markComparesFunctions();
    }
    std::vector<bool> given(call->args.size(), true);
    return this->callLibraryMethod(call, member, cls, name, {}, &given, super);
  }
  case EmitterMethod::RemoveAll: {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    if (!call->args.empty() &&
        !EventMaps(this->state).eventArgs(events, call->args[0], name, &event)) {
      return call;
    }
    std::vector<bool> given(call->args.size(), true);
    this->callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::None:
    break;
  }

  return nullptr;
}

void EmitterChecker::reportEmitterMethodValue(MemberExpr *member) {
  std::string message = ErrorTextEmitted::methodVal(member->name);
  this->state.getDiagnostics().error(member->loc(), message);
}

}
