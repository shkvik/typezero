#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/functions/function_value_checker/function_value_checker.h"
#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// The events every emitter has, which its code emits itself.
bool isServiceEvent(llvm::StringRef name) {
  return name == "newListener" || name == "removeListener";
}

// What the listeners of newListener and removeListener get: the name of the
// event and the listener.
const Type *serviceEventArgs() { return Type::tuple({Type::string(), Type::unknown()}); }

// "'data', 'end' and 'error'"
std::string eventList(const Type *events) {
  std::string out;
  llvm::ArrayRef<Field> fields = events->fields();
  for (size_t i = 0; i < fields.size(); ++i) {
    out += (i == 0 ? "" : i + 1 == fields.size() ? " and " : ", ") + ("'" + fields[i].name + "'");
  }

  return out;
}

// The name nearest to `name` among the events, for a typo; empty if none
// is near.
std::string nearestEvent(const Type *events, llvm::StringRef name) {
  std::string best;
  unsigned bestDistance = ~0u;
  for (const Field &f : events->fields()) {
    unsigned distance = llvm::StringRef(f.name).edit_distance(name);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = f.name;
    }
  }

  return bestDistance <= std::max<size_t>(2, name.size() / 3) ? best : std::string();
}

} // namespace

EventMaps::EventMaps(Checker &checker) : checker_(checker) {}

bool EventMaps::isEmitterClass(const ClassDecl *cls) const {
  ClassDecl *base = checker_.libraryClass("EventEmitterBase");

  return cls && base && cls->derivesFrom(base);
}

const Type *EventMaps::eventMapOf(const Type *type) const {
  const ClassDecl *emitter = checker_.libraryClass("EventEmitter");
  if (!emitter) {
    return nullptr;
  }
  // The streams of node:stream have theirs from StreamEmitter (see
  // library/stream/stream.tz).
  const ClassDecl *streams = checker_.libraryClass("StreamEmitter");
  for (const ClassDecl *c = ClassDeclarer::classOf(type); c; c = c->parent) {
    if (c->generic && (c->generic == emitter || c->generic == streams) && !c->typeArgs.empty()) {
      return c->typeArgs[0];
    }
  }

  return nullptr;
}

bool EventMaps::checkEventMap(const Type *events, llvm::SMLoc loc) {
  if (events->isError() || mentionsParam(events)) {
    return true;
  }
  auto bad = [&](const std::string &why) {
    checker_.diags_.error(
        loc, "the type argument of 'EventEmitter' is its event map: each event with the "
             "arguments its listeners get, as a tuple, e.g. 'EventEmitter<{ data: "
             "[chunk: string]; end: [] }>'; " +
                 why);

    return false;
  };
  if (events->isRecord()) {
    if (!events->valueType()->isTuple()) {
      return bad("the arguments of each event are '" + events->valueType()->name().str() +
                 "' here, which is not a tuple");
    }
    return true;
  }
  if (!events->isObject() || events->isClass() || events->isTuple() || events->isAnyObject() ||
      events->isBytes() || events->isDate() || events->isRegExp() || events->isMatch()) {
    return bad("'" + events->name().str() + "' is not one");
  }
  for (const Field &f : events->fields()) {
    if (f.type->isOptional()) {
      return bad("event '" + f.name + "' is optional, but an event is in the map or not");
    }
    if (!f.type->isTuple()) {
      return bad("event '" + f.name + "' has '" + f.type->name().str() + "', which is not a tuple");
    }
    if (isServiceEvent(f.name) && f.type != serviceEventArgs()) {
      checker_.diags_.error(
          loc, "every emitter has the event '" + f.name +
                   "', whose listeners get the name of the event and the listener, "
                   "'[eventName: string, listener: unknown]', but the event map gives it '" +
                   f.type->name() + "'; leave it out of the map");
      return false;
    }
  }

  return true;
}

const Type *EventMaps::eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                                 std::string *label) {
  if (events->isRecord()) {
    if (auto *lit = dyn_cast<StringLitExpr>(name); lit && label) {
      *label = utf8(lit->value);
    }
    name = checker_.argumentChecker().checkArg(name, Type::string());
    return name->type->isError() ? nullptr : events->valueType();
  }
  // A constant, after the check: a literal, a constant holding one, a
  // member of a string enum.
  name = checker_.argumentChecker().checkArg(name, Type::string());
  std::optional<std::string> key = checker_.keyedFieldChecker().constantKey(name);
  if (!key) {
    if (!name->type->isError()) {
      checker_.diags_.error(name->loc(),
                            "the name of the event must be known when compiling, as a "
                            "string literal, e.g. '" +
                                method.str() +
                                "(\"data\", ...)'; to "
                                "name events known only when the program runs, give the "
                                "emitter a map that takes any name, e.g. 'new EventEmitter<{ "
                                "[name: string]: [msg: string] }>()'");
    }
    return nullptr;
  }
  if (label) {
    *label = *key;
  }
  llvm::SMLoc loc = name->loc();
  if (isServiceEvent(*key)) {
    return serviceEventArgs();
  }
  // A map that a round looking for maps knows only by what it found so far.
  if (auto open = checker_.openMaps_.find(events); open != checker_.openMaps_.end()) {
    for (const EventInference::Event &e : checker_.inference_->maps[open->second]) {
      if (e.name == *key) {
        return checker_.eventMapInference().fromPortable(e.args, loc);
      }
    }
    return nullptr; // not yet
  }
  if (const Field *f = events->field(*key)) {
    return f->type;
  }
  std::string message;
  if (checker_.inferredMaps_.count(events)) {
    // A map found from emit() calls: an event no code emits is a typo, as a
    // rule (docs/events.md, section 2).
    message = "no code of the program emits '" + *key + "' on this emitter";
    if (events->fields().empty()) {
      message += ", nor any other event: an emitter without an event map has the events that "
                 "emit() calls give it, e.g. 'bus.emit(\"" +
                 *key + "\", ...)'";
    } else {
      message += "; it emits " + eventList(events);
    }
  } else {
    message = "the emitter has no event '" + *key + "'";
    if (events->fields().empty()) {
      message += ": its event map has none";
    } else {
      message += ": its event map has " + eventList(events);
    }
  }
  if (std::string near = nearestEvent(events, *key); !near.empty()) {
    message += "; did you mean '" + near + "'?";
  }
  checker_.diags_.error(loc, message);

  return nullptr;
}

const Type *EventMaps::listenerType(const Type *args, const Type *ret) {
  std::vector<const Type *> params;
  for (const Field &f : args->fields()) {
    params.push_back(f.type);
  }
  bool hasRest = args->tupleRest() != nullptr;
  if (hasRest) {
    params.push_back(args->tupleRest());
  }

  return Type::function(params, args->tupleMin(), hasRest, ret);
}

Expr *EventMaps::checkListener(Expr *arg, const Type *args, llvm::StringRef event,
                               const Type *receiver, Checker::ListenerKind &kind) {
  const Type *sync = listenerType(args, Type::voidTy());
  const Type *async = listenerType(args, Type::promise(Type::voidTy()));
  kind = Checker::ListenerKind::Plain;
  // `function (this: Server, port: int)`: it gets the emitter as `this`,
  // which must be what it says.
  auto takesThis = [&](const Type *self, llvm::SMLoc loc) {
    if (self->isError()) {
      return false;
    }
    if (self->isUnknown() || receiver->isSubtypeOf(self)) {
      return true;
    }
    checker_.diags_.error(loc, "the listener takes 'this' of type '" + self->name() +
                                   "', but it gets the emitter, of type '" + receiver->name() +
                                   "'");

    return false;
  };
  auto withThis = [&](const Type *self) {
    std::vector<const Type *> params{self};
    params.insert(params.end(), sync->params().begin(), sync->params().end());

    return Type::function(params, sync->minArgs() + 1, sync->hasRest(), Type::voidTy(),
                          /*hasThis=*/true);
  };
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    FunctionDecl *fn = fe->fn;
    if (!fn->params.empty() && fn->params[0].name == "this" && fn->params[0].type) {
      const Type *self =
          checker_.typeResolver().resolveType(*fn->params[0].type, /*allowVoid=*/false);
      if (!takesThis(self, fn->params[0].loc)) {
        arg = checker_.exprChecker().checkExpr(arg);
        return arg;
      }
      if (fn->isAsync) {
        checker_.diags_.error(fe->loc(),
                              "an async listener cannot take 'this'; use the emitter by name");
        arg = checker_.exprChecker().checkExpr(arg);
        return arg;
      }
      kind = Checker::ListenerKind::This;
      const Type *wanted = withThis(self);
      Checker::Signature sig = ClosureChecker::signatureOf(wanted);
      sig.voidResult = true;
      arg = checker_.closureChecker().checkFunctionExpr(fe, &sig);
      if (arg->type->isError()) {
        return arg;
      }
      return checker_.typeRelations().convert(checker_.typeRelations().coerce(arg, wanted),
                                              Type::unknown());
    }
    // An arrow written here gets the arguments of the event; an async one
    // gives a Promise, which the emitter lets go (docs/events.md, section 3).
    if (fn->isAsync) {
      kind = Checker::ListenerKind::Async;
    }
    Checker::Signature sig = ClosureChecker::signatureOf(fn->isAsync ? async : sync);
    sig.voidResult = !fn->isAsync;
    arg = checker_.closureChecker().checkFunctionExpr(fe, &sig);
    if (arg->type->isError()) {
      return arg;
    }
    return checker_.typeRelations().convert(
        checker_.typeRelations().coerce(arg, fn->isAsync ? async : sync), Type::unknown());
  }
  arg = checker_.exprChecker().checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isFunction()) {
    checker_.diags_.error(arg->loc(),
                          "a listener is a function, but this is '" + type->name() + "'");
    return arg;
  }
  if (type->hasThis()) {
    if (!takesThis(type->params()[0], arg->loc())) {
      return arg;
    }
    kind = Checker::ListenerKind::This;
    if (type == withThis(type->params()[0])) {
      return checker_.typeRelations().convert(arg, Type::unknown());
    }
    checker_.diags_.error(arg->loc(), "the listeners of '" + event.str() + "' get '" +
                                          args->name().str() + "', but this function is '" +
                                          type->name().str() + "'");
    return arg;
  }
  bool isAsync = type->ret()->isPromise();
  if (isAsync) {
    kind = Checker::ListenerKind::Async;
  }
  const Type *wanted = isAsync ? async : sync;
  if (type == wanted) {
    return checker_.typeRelations().convert(arg, Type::unknown());
  }
  if (isAsync && type->ret() != async->ret()) {
    checker_.diags_.error(arg->loc(),
                          "an async listener returns 'Promise<void>', but this one returns '" +
                              type->ret()->name() +
                              "', which the emitter would drop; wrap it, "
                              "e.g. 'async (...) => { await f(...) }'");
    return arg;
  }
  if (Expr *adapted = checker_.functionValueChecker().adaptFunction(arg, wanted)) {
    return checker_.typeRelations().convert(adapted, Type::unknown());
  }
  checker_.diags_.error(arg->loc(), "the listeners of '" + event.str() + "' get '" +
                                        args->name().str() + "', but this function is '" +
                                        type->name().str() +
                                        "'; pass an arrow that calls it with what it needs");

  return arg;
}

const Type *EventMaps::createdType(const NewExpr *create) {
  if (!create->member.empty()) {
    return nullptr;
  }
  auto *cls = llvm::dyn_cast_or_null<ClassDecl>(checker_.typeDeclarer().lookupType(create->name));
  if (!cls) {
    return nullptr;
  }
  if (!cls->isGeneric()) {
    return cls->type;
  }
  std::vector<const Type *> args;
  for (const TypeRef &ref : create->typeArgs) {
    args.push_back(checker_.typeResolver().resolveType(ref, /*allowVoid=*/false));
    if (args.back()->isError()) {
      return nullptr;
    }
  }
  if (args.empty() && cls->library && cls->name == "EventEmitter") {
    if (const Type *events = checker_.eventMapInference().inferredEventMap(
            "new " + checker_.eventMapInference().placeOf(create->loc()))) {
      args.push_back(events);
    }
  }
  if (args.empty()) {
    return nullptr;
  }
  ClassDecl *inst =
      checker_.genericClassInstantiator().instantiateClass(cls, std::move(args), create->loc());

  return inst ? inst->type : nullptr;
}

} // namespace tz
