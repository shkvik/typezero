#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The methods of node's EventEmitter that name an event, by what the
// program gives them.
enum class EmitterMethod {
  None,
  Listen,
  Remove,
  Emit,
  Listeners,
  RawListeners,
  Count,
  RemoveAll,
  Chain
};

EmitterMethod emitterMethod(llvm::StringRef name) {
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
  if (name == "setMaxListeners") {
    return EmitterMethod::Chain;
  }

  return EmitterMethod::None;
}

// The events every emitter has, which its code emits itself.
bool isServiceEvent(llvm::StringRef name) {
  return name == "newListener" || name == "removeListener";
}

// The file of a place that placeOf gives: what comes before its line and
// column.
std::string fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

// "main.tz:3:13" of a place, for messages.
std::string shortPlace(const std::string &place) {
  std::string file = fileOfPlace(place);

  return llvm::sys::path::filename(file).str() + place.substr(file.size());
}

} // namespace

EmitterChecker::EmitterChecker(Sema &sema) : sema_(sema) {}

Expr *EmitterChecker::checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }

    return expr;
  };
  const Type *events = nullptr;
  if (!expr->typeArgs.empty()) {
    if (expr->typeArgs.size() == 1) {
      events = sema_.typeResolver().resolveType(expr->typeArgs[0], /*allowVoid=*/false);
    }
  } else if (const Type *want = expected ? expected->nonOptional() : nullptr;
             want && ClassDeclarer::classOf(want) && ClassDeclarer::classOf(want)->generic == generic) {
    // `const bus: EventEmitter<Events> = new EventEmitter()`: the map of the
    // place it goes.
    events = ClassDeclarer::classOf(want)->typeArgs[0];
  } else {
    events = sema_.eventMapInference().inferredEventMap("new " + sema_.eventMapInference().placeOf(expr->loc()));
    if (!events) {
      sema_.diags_.error(expr->loc(), "'new EventEmitter()' needs its event map here, e.g. 'new "
                                "EventEmitter<{ data: [chunk: string]; end: [] }>()'");
      return checkArgsLoosely();
    }
  }
  if (!events) { // reported by instantiateClass
    return nullptr;
  }
  if (events->isError()) {
    return checkArgsLoosely();
  }
  ClassDecl *inst = sema_.genericClassInstantiator().instantiateClass(generic, {events}, expr->loc());
  if (!inst) {
    return checkArgsLoosely();
  }
  expr->typeArgs.clear();
  // `{ captureRejections: true }`: the emitter emits 'error' with the Error
  // of each rejected Promise of an async listener, so that is what the
  // listeners of 'error' get.
  if (!expr->args.empty()) {
    if (auto *options = dyn_cast<ObjectLitExpr>(expr->args[0])) {
      for (const ObjectLitExpr::Property &prop : options->props) {
        auto *off = dyn_cast<BoolLitExpr>(prop.value);
        if (prop.name != "captureRejections" || (off && !off->value)) {
          continue;
        }
        const Type *error = events->isRecord() ? events->valueType()
                            : events->isObject() && events->field("error")
                                ? events->field("error")->type
                                : nullptr;
        const Type *wanted = Type::tuple({sema_.errorClass_->type});
        if (error && error != wanted && !mentionsParam(error)) {
          sema_.diags_.error(prop.loc, "an emitter that captures rejections emits 'error' with the Error "
                                 "of a rejected async listener, but its event map gives 'error' '" +
                                     error->name() + "'; give it '[err: Error]'");
        }
      }
    }
  }

  return sema_.methodCallChecker().checkClassNew(expr, inst, expected);
}

Expr *EmitterChecker::collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site) {
  call->type = Type::boolTy();
  member->type = Type::error();
  llvm::SMLoc loc = call->args.empty() ? member->loc() : call->args[0]->loc();
  std::vector<const Type *> elements;
  bool known = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    call->args[i] = i == 0 ? sema_.argumentChecker().checkArg(call->args[i], Type::string()) : sema_.exprChecker().checkExpr(call->args[i]);
    const Type *type = call->args[i]->type;
    if (type->isError() || type->isVoid() || mentionsParam(type)) {
      known = false;
    }
    if (i > 0) {
      elements.push_back(type->isLiteral() ? type->literalBase() : type);
    }
  }
  std::optional<std::string> key = call->args.empty() ? std::nullopt : sema_.keyedFieldChecker().constantKey(call->args[0]);
  if (!key || !known || isServiceEvent(*key)) {
    return call;
  }
  const Type *found = Type::tuple(elements);
  std::vector<EventInference::Event> &events = sema_.inference_->maps[site];
  for (EventInference::Event &e : events) {
    if (e.name != *key) {
      continue;
    }
    // An event emitted with other types: those that hold both, if any, as
    // int and double make double; the last round reports others.
    const Type *before = sema_.eventMapInference().fromPortable(e.args, loc);
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
      e.args = sema_.eventMapInference().portable(now, seen);
      sema_.inference_->changed = true;
    }
    return call;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  events.push_back({*key, sema_.eventMapInference().portable(found, seen), shortPlace(sema_.eventMapInference().placeOf(loc))});
  sema_.inference_->changed = true;

  return call;
}

Expr *EmitterChecker::callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                              llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                              const std::vector<bool> *given, bool super) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false);
  assert(!methods.empty() && "a method of the emitter's code");
  FuncSymbol *sym = methods[0]->sym;
  sema_.returnInferrer().resolveParams(sym);
  llvm::SMLoc loc = member->loc();
  std::vector<const Type *> params = sym->params;
  const Type *ret = sema_.returnInferrer().returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric()) {
    llvm::SMLoc outerLoc = std::exchange(sema_.instanceLoc_, loc);
    for (const Type *&p : params) {
      p = sema_.typeParams().substitute(p, sym->typeParams, bound);
    }
    ret = ret ? sema_.typeParams().substitute(ret, sym->typeParams, bound) : Type::error();
    sema_.instanceLoc_ = outerLoc;
    // Generic code is checked with its types opaque, and not compiled: the
    // copies of the methods are for the types of its instances.
    if (!sema_.trial_ && llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); })) {
      if (FuncSymbol *instance = sema_.genericInstantiator().instantiate(sym, bound, loc)) {
        callee = instance;
      }
    }
  }
  call->superCall = super;
  if (sym->isGeneric() || super) {
    call->method = callee;
  } else if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
    call->method = impl->sym;
  } else {
    call->method = sym;
    call->virtualCall = true;
  }
  member->type = sema_.returnInferrer().functionType(callee);
  call->type = ret ? ret : Type::error();
  // A copy takes the elements of an argument list as parameters of their
  // own (see expandArgumentList).
  if (callee != sym) {
    sema_.returnInferrer().resolveParams(callee);
    return sema_.argumentChecker().checkArguments(call, callee->params, callee->minArgs, callee->hasRest, loc, given);
  }

  return sema_.argumentChecker().checkArguments(call, params, sym->minArgs, sym->hasRest, loc, given);
}

Expr *EmitterChecker::checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super) {
  EmitterMethod kind = emitterMethod(member->name);
  if (kind == EmitterMethod::None || sema_.libraryCalls().inLibrary(call->loc())) {
    return nullptr;
  }
  // A class down the chain may declare a method of that name itself.
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, /*isStatic=*/false);
  if (!methods.empty() && !methods[0]->cls->library) {
    return nullptr;
  }
  const Type *receiver = member->object->type; // `this` for super.m()
  const Type *events = sema_.eventMaps().eventMapOf(receiver);
  if (!events) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    sema_.diags_.error(member->loc(), "'" + member->name +
                                    "' takes no type arguments: the event map of "
                                    "the emitter gives the types");
    member->typeArgs.clear();
  }
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = sema_.exprChecker().checkExpr(call->args[i]);
    }
  };
  auto flag = [&](bool value) { return sema_.exprChecker().checkExpr(sema_.ctx_.make<BoolLitExpr>(call->loc(), value)); };
  llvm::StringRef name = member->name;
  std::string event;
  switch (kind) {
  case EmitterMethod::Emit: {
    if (call->args.empty()) {
      sema_.diags_.error(member->loc(), "'emit' needs the name of the event, e.g. 'emit(\"data\", ...)'");
      return call;
    }
    // A round looking for event maps records what the emits pass.
    if (auto open = sema_.openMaps_.find(events); open != sema_.openMaps_.end()) {
      return collectEmitterCall(call, member, open->second);
    }
    const Type *args = sema_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    size_t count = call->args.size() - 1;
    size_t min = args->tupleMin();
    size_t max = args->tupleRest() ? ~size_t(0) : args->fields().size();
    // Of a map found from emits: where the event is first emitted, which
    // gave it its arguments.
    std::string first;
    if (auto inferred = sema_.inferredMaps_.find(events); inferred != sema_.inferredMaps_.end()) {
      for (const EventInference::Event &e : sema_.inference_->maps[inferred->second]) {
        if (e.name == event) {
          first = e.where;
        }
      }
    }
    if (count < min || count > max) {
      if (!first.empty()) {
        sema_.diags_.error(member->loc(), "'" + event + "' is emitted with " + args->name().str() +
                                        " at " + first + ", but with " + std::to_string(count) +
                                        " argument" + (count == 1 ? "" : "s") + " here");
      } else {
        sema_.diags_.error(member->loc(), "the listeners of '" + event + "' get '" + args->name().str() +
                                        "': " + argumentCount(unsigned(min), unsigned(max)) +
                                        ", but got " + std::to_string(count));
      }
      checkArgsLoosely(1);
      return call;
    }
    std::vector<bool> given(call->args.size(), false);
    given[0] = true;
    if (!first.empty()) {
      // Both places in the message (docs/events.md, section 2).
      for (size_t i = 1; i < call->args.size() && i - 1 < args->fields().size(); ++i) {
        const Type *wanted = args->fields()[i - 1].type;
        call->args[i] = sema_.exprChecker().checkExpr(call->args[i], wanted);
        const Type *type = call->args[i]->type;
        if (!type->isError() && !TypeRelations::isImplicitlyConvertible(type, wanted)) {
          sema_.diags_.error(call->args[i]->loc(), "'" + event + "' is emitted with " +
                                                 args->name().str() + " at " + first +
                                                 ", but with '" + type->name().str() +
                                                 "' here; an event has one type of arguments");
          checkArgsLoosely(i + 1);
          return call;
        }
        call->args[i] = sema_.typeRelations().coerce(call->args[i], wanted);
        given[i] = true;
      }
    }
    return callLibraryMethod(call, member, cls, "emit", {args}, &given, super);
  }
  case EmitterMethod::Listen:
  case EmitterMethod::Remove: {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 2, 2)) {
      return call;
    }
    const Type *args = sema_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    Sema::ListenerKind listener = Sema::ListenerKind::Plain;
    call->args[1] = sema_.eventMaps().checkListener(call->args[1], args, event, receiver, listener);
    if (call->args[1]->type->isError()) {
      return call;
    }
    if (kind == EmitterMethod::Listen) {
      call->args.push_back(
          sema_.exprChecker().checkExpr(sema_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(listener)), Type::intTy()));
    } else {
      sema_.file_->comparesFunctions = true; // the listener is found by what it is
    }
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver; // `this`
    return call;
  }
  case EmitterMethod::Listeners:
  case EmitterMethod::RawListeners: {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 1)) {
      return call;
    }
    const Type *args = sema_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      return call;
    }
    call->args.push_back(flag(kind == EmitterMethod::RawListeners));
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, "#listenersOf", {args}, &given, super);
    call->type = Type::array(EventMaps::listenerType(args, Type::voidTy()));
    return call;
  }
  case EmitterMethod::Count: {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    const Type *args = sema_.eventMaps().eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    if (call->args.size() == 2) {
      Sema::ListenerKind listener = Sema::ListenerKind::Plain;
      call->args[1] = sema_.eventMaps().checkListener(call->args[1], args, event, receiver, listener);
      if (call->args[1]->type->isError()) {
        return call;
      }
      sema_.file_->comparesFunctions = true;
    }
    std::vector<bool> given(call->args.size(), true);
    return callLibraryMethod(call, member, cls, name, {}, &given, super);
  }
  case EmitterMethod::RemoveAll: {
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 1)) {
      return call;
    }
    if (!call->args.empty() && !sema_.eventMaps().eventArgs(events, call->args[0], name, &event)) {
      return call;
    }
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::Chain:
    callLibraryMethod(call, member, cls, name, {}, nullptr, super);
    if (!call->type->isError()) {
      call->type = receiver;
    }
    return call;
  case EmitterMethod::None:
    break;
  }

  return nullptr;
}

std::string EmitterChecker::libraryExportHint(llvm::StringRef name) {
  if (name == "errorMonitor" || name == "captureRejectionSymbol") {
    return ": in node it is a symbol, and tz has no symbols of its own; the names of events are "
           "strings (docs/events.md, section 4)";
  }
  if (name == "defaultMaxListeners") {
    return "; it is 'EventEmitter.defaultMaxListeners'";
  }
  if (name == "captureRejections") {
    return "; capture the rejections of each emitter: 'new EventEmitter({ captureRejections: true "
           "})'";
  }
  if (name == "addAbortListener") {
    return " yet; listen to the signal: 'signal.addEventListener(\"abort\", listener, { once: "
           "true })'";
  }
  if (name == "EventEmitterAsyncResource" || name == "usingDomains") {
    return ": tz has neither async_hooks nor domains";
  }
  if (name == "EventTarget" || name == "Event" || name == "CustomEvent") {
    return "; '" + name.str() + "' is global, as in node: use it without an import";
  }

  return "";
}

void EmitterChecker::reportEmitterMethodValue(MemberExpr *member) {
  sema_.diags_.error(member->loc(), "'" + member->name +
                                  "' of an emitter can only be called: the event "
                                  "map checks each call by the name of its event; wrap it in an "
                                  "arrow, e.g. '(listener) => bus." +
                                  member->name + "(\"data\", listener)'");
}

} // namespace tz
