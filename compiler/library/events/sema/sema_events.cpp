// Events (docs/events.md): node:events and the events of the web platform,
// which tz has written in tz (src/lib/events.tz and src/lib/web.tz), and
// what the compiler adds to that code: the intrinsics `__tz_*` it uses, and
// the types of the events.
//
// The code of an emitter sees each listener as an `unknown`, as JavaScript
// sees a function. The type of an emitter, `EventEmitter<Events>`, says
// which events it has and what their listeners get, a tuple for each:
// `{ data: [chunk: string]; end: [] }`, or `{ [name: string]: [msg: Msg] }`
// for names known only when the program runs. Each call of the program on
// an emitter (on, emit, listeners...) is checked here against that map and
// becomes a call of the emitter's code: a listener converted to an unknown,
// the arguments of emit those of `emit<A>` with A the tuple of the event,
// whose copy calls each listener as a function of them.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Path.h"

#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
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
      name == "prependOnceListener")
    return EmitterMethod::Listen;
  if (name == "off" || name == "removeListener")
    return EmitterMethod::Remove;
  if (name == "emit")
    return EmitterMethod::Emit;
  if (name == "listeners")
    return EmitterMethod::Listeners;
  if (name == "rawListeners")
    return EmitterMethod::RawListeners;
  if (name == "listenerCount")
    return EmitterMethod::Count;
  if (name == "removeAllListeners")
    return EmitterMethod::RemoveAll;
  if (name == "setMaxListeners")
    return EmitterMethod::Chain;
  return EmitterMethod::None;
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
  for (size_t i = 0; i < fields.size(); ++i)
    out += (i == 0 ? "" : i + 1 == fields.size() ? " and " : ", ") + ("'" + fields[i].name + "'");
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

//===----------------------------------------------------------------------===//
// The built-in modules written in tz
//===----------------------------------------------------------------------===//

void Sema::prepareLibraries(SourceFile &program) {
  for (Module *module : program.modules) {
    if (!module->builtin)
      continue;
    // Their code finds a listener by what it is: every evaluation of a
    // function expression must give a function of its own.
    program.comparesFunctions = true;
    if (!module->file.items.empty()) {
      llvm::SourceMgr &sm = diags_.sourceMgr();
      unsigned id = sm.FindBufferContainingLoc(module->file.items.front()->loc());
      if (id) {
        const llvm::MemoryBuffer *buffer = sm.getMemoryBuffer(id);
        libraryRanges_.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
        // Its warnings are not the program's author's to fix.
        diags_.quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
      }
    }
    for (Stmt *item : module->file.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        libFunctions_[fn->name] = fn;
        continue;
      }
      if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
        libTypes_[alias->name] = alias;
        continue;
      }
      auto *cls = dyn_cast<ClassDecl>(item);
      if (!cls)
        continue;
      cls->library = true;
      libClasses_[cls->name] = cls;
      // `@__tz_hidden x`: a field that is no key.
      for (ClassField &f : cls->fields)
        llvm::erase_if(f.decorators, [&](const Decorator &d) {
          auto *name = dyn_cast<NameExpr>(d.expr);
          if (!name || name->name != "__tz_hidden")
            return false;
          f.hidden = true;
          return true;
        });
    }
  }
}

bool Sema::inLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();
  return llvm::any_of(libraryRanges_, [&](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  });
}

Expr *Sema::checkLibCall(CallExpr *call, NameExpr *name) {
  if (!name->name.starts_with("__tz_"))
    return nullptr;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  // A name of the program's own is that; anywhere else but the built-in
  // modules, these are no names.
  if (ScopeEntry own = lookup(name->name); own.var || own.func || own.cls)
    return nullptr;
  if (!inLibrary(call->loc())) {
    diags_.error(name->loc(), "cannot find name '" + name->name + "'");
    checkArgsLoosely();
    return call;
  }
  if (name->name.starts_with("__tz_subtle_"))
    return checkSubtleIntrinsic(call, name);
  struct Intrinsic {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned args, typeArgs;
  };
  // Those that take a type; the others are declared by the code that
  // calls them (sema_library.cpp).
  static const Intrinsic intrinsics[] = {{"__tz_listener", Builtin::LibListener, 1, 1},
                                         {"__tz_as", Builtin::LibAs, 1, 1},
                                         {"__tz_chunk", Builtin::LibChunk, 1, 1},
                                         {"__tz_isChunk", Builtin::LibIsChunk, 1, 1},
                                         {"__tz_chunkKind", Builtin::LibChunkKind, 0, 1},
                                         {"__tz_typeName", Builtin::LibTypeName, 0, 1},
                                         {"__tz_overrides", Builtin::LibOverrides, 2, 0},
                                         {"__tz_fsOpen", Builtin::LibFsOpen, 3, 0},
                                         {"__tz_fsRead", Builtin::LibFsRead, 5, 0},
                                         {"__tz_fsWrite", Builtin::LibFsWrite, 5, 0},
                                         {"__tz_fsClose", Builtin::LibFsClose, 2, 0},
                                         {"__tz_stdinChunk", Builtin::LibStdinChunk, 1, 0},
                                         {"__tz_stdWrite", Builtin::LibStdWrite, 2, 0},
                                         {"__tz_ahead", Builtin::LibAhead, 1, 0},
                                         {"__tz_order", Builtin::LibOrder, 0, 0}};
  const Intrinsic *intrinsic =
      llvm::find_if(intrinsics, [&](const Intrinsic &i) { return i.name == name->name; });
  if (intrinsic == std::end(intrinsics) || call->args.size() != intrinsic->args ||
      name->typeArgs.size() != intrinsic->typeArgs || call->hasSpread()) {
    diags_.error(name->loc(), "'" + name->name + "' is no intrinsic of tz, or is called wrongly");
    checkArgsLoosely();
    return call;
  }
  const Type *typeArg =
      intrinsic->typeArgs ? resolveType(name->typeArgs[0], /*allowVoid=*/false) : nullptr;
  call->builtin = intrinsic->builtin;
  switch (intrinsic->builtin) {
  case Builtin::LibListener: {
    // A function type whose last parameter is a tuple of arguments, `(...args:
    // A) => void` with A a tuple in a copy, is the function of those
    // parameters: the one a listener was converted from.
    const Type *fn = typeArg;
    std::vector<const Type *> flat;
    size_t minArgs = 0;
    bool hasRest = false;
    if (fn->isFunction() && flattenArguments(fn->params(), minArgs, hasRest, flat))
      fn = Type::function(flat, minArgs, hasRest, fn->ret(), fn->hasThis());
    call->args[0] = checkArg(call->args[0], Type::unknown());
    call->type = fn;
    if (!fn->isFunction() && !fn->isParam() && !fn->isError())
      diags_.error(name->loc(), "'__tz_listener' gives a function");
    break;
  }
  case Builtin::LibAs:
    call->args[0] = checkExpr(call->args[0]);
    if (!typeArg->isClass() && !typeArg->isParam() && !typeArg->isError())
      diags_.error(name->loc(), "'__tz_as' takes a class");
    call->type = Type::optional(typeArg);
    break;
  case Builtin::LibAhead:
    call->args[0] = checkExpr(call->args[0]);
    call->type = call->args[0]->type;
    if (!call->type->isPromise() && !call->type->isError())
      diags_.error(name->loc(), "'__tz_ahead' takes the promise an await waits for");
    if (currentFunction_ && currentFunction_->decl)
      currentFunction_->decl->awaitsAhead = true;
    break;
  case Builtin::LibOrder:
    call->type = Type::voidTy();
    break;
  case Builtin::LibChunk:
  case Builtin::LibIsChunk:
  case Builtin::LibChunkKind:
  case Builtin::LibTypeName:
  case Builtin::LibOverrides:
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk:
  case Builtin::LibStdWrite:
    return checkStreamIntrinsic(call, name, typeArg);
  default:
    break;
  }
  return call;
}

//===----------------------------------------------------------------------===//
// Event maps
//===----------------------------------------------------------------------===//

bool Sema::isEmitterClass(const ClassDecl *cls) const {
  ClassDecl *base = libraryClass("EventEmitterBase");
  return cls && base && cls->derivesFrom(base);
}

const Type *Sema::eventMapOf(const Type *type) const {
  const ClassDecl *emitter = libraryClass("EventEmitter");
  if (!emitter)
    return nullptr;
  // The streams of node:stream have theirs from StreamEmitter (see
  // src/lib/stream.tz).
  const ClassDecl *streams = libraryClass("StreamEmitter");
  for (const ClassDecl *c = classOf(type); c; c = c->parent)
    if (c->generic && (c->generic == emitter || c->generic == streams) && !c->typeArgs.empty())
      return c->typeArgs[0];
  return nullptr;
}

bool Sema::checkEventMap(const Type *events, llvm::SMLoc loc) {
  if (events->isError() || mentionsParam(events))
    return true;
  auto bad = [&](const std::string &why) {
    diags_.error(loc, "the type argument of 'EventEmitter' is its event map: each event with the "
                      "arguments its listeners get, as a tuple, e.g. 'EventEmitter<{ data: "
                      "[chunk: string]; end: [] }>'; " +
                          why);
    return false;
  };
  if (events->isRecord()) {
    if (!events->valueType()->isTuple())
      return bad("the arguments of each event are '" + events->valueType()->name().str() +
                 "' here, which is not a tuple");
    return true;
  }
  if (!events->isObject() || events->isClass() || events->isTuple() || events->isAnyObject() ||
      events->isBytes() || events->isDate() || events->isRegExp() || events->isMatch())
    return bad("'" + events->name().str() + "' is not one");
  for (const Field &f : events->fields()) {
    if (f.type->isOptional())
      return bad("event '" + f.name + "' is optional, but an event is in the map or not");
    if (!f.type->isTuple())
      return bad("event '" + f.name + "' has '" + f.type->name().str() + "', which is not a tuple");
    if (isServiceEvent(f.name) && f.type != serviceEventArgs()) {
      diags_.error(loc,
                   "every emitter has the event '" + f.name +
                       "', whose listeners get the name of the event and the listener, "
                       "'[eventName: string, listener: unknown]', but the event map gives it '" +
                       f.type->name() + "'; leave it out of the map");
      return false;
    }
  }
  return true;
}

const Type *Sema::eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                            std::string *label) {
  if (events->isRecord()) {
    if (auto *lit = dyn_cast<StringLitExpr>(name); lit && label)
      *label = utf8(lit->value);
    name = checkArg(name, Type::string());
    return name->type->isError() ? nullptr : events->valueType();
  }
  // A constant, after the check: a literal, a constant holding one, a
  // member of a string enum.
  name = checkArg(name, Type::string());
  std::optional<std::string> key = constantKey(name);
  if (!key) {
    if (!name->type->isError())
      diags_.error(name->loc(), "the name of the event must be known when compiling, as a "
                                "string literal, e.g. '" + method.str() + "(\"data\", ...)'; to "
                                "name events known only when the program runs, give the "
                                "emitter a map that takes any name, e.g. 'new EventEmitter<{ "
                                "[name: string]: [msg: string] }>()'");
    return nullptr;
  }
  if (label)
    *label = *key;
  llvm::SMLoc loc = name->loc();
  if (isServiceEvent(*key))
    return serviceEventArgs();
  // A map that a round looking for maps knows only by what it found so far.
  if (auto open = openMaps_.find(events); open != openMaps_.end()) {
    for (const EventInference::Event &e : inference_->maps[open->second])
      if (e.name == *key)
        return fromPortable(e.args, loc);
    return nullptr; // not yet
  }
  if (const Field *f = events->field(*key))
    return f->type;
  std::string message;
  if (inferredMaps_.count(events)) {
    // A map found from emit() calls: an event no code emits is a typo, as a
    // rule (docs/events.md, section 2).
    message = "no code of the program emits '" + *key + "' on this emitter";
    if (events->fields().empty())
      message += ", nor any other event: an emitter without an event map has the events that "
                 "emit() calls give it, e.g. 'bus.emit(\"" +
                 *key + "\", ...)'";
    else
      message += "; it emits " + eventList(events);
  } else {
    message = "the emitter has no event '" + *key + "'";
    if (events->fields().empty())
      message += ": its event map has none";
    else
      message += ": its event map has " + eventList(events);
  }
  if (std::string near = nearestEvent(events, *key); !near.empty())
    message += "; did you mean '" + near + "'?";
  diags_.error(loc, message);
  return nullptr;
}

const Type *Sema::listenerType(const Type *args, const Type *ret) {
  std::vector<const Type *> params;
  for (const Field &f : args->fields())
    params.push_back(f.type);
  bool hasRest = args->tupleRest() != nullptr;
  if (hasRest)
    params.push_back(args->tupleRest());
  return Type::function(params, args->tupleMin(), hasRest, ret);
}

Expr *Sema::checkListener(Expr *arg, const Type *args, llvm::StringRef event,
                          const Type *receiver, ListenerKind &kind) {
  const Type *sync = listenerType(args, Type::voidTy());
  const Type *async = listenerType(args, Type::promise(Type::voidTy()));
  kind = ListenerKind::Plain;
  // `function (this: Server, port: int)`: it gets the emitter as `this`,
  // which must be what it says.
  auto takesThis = [&](const Type *self, llvm::SMLoc loc) {
    if (self->isError())
      return false;
    if (self->isUnknown() || receiver->isSubtypeOf(self))
      return true;
    diags_.error(loc, "the listener takes 'this' of type '" + self->name() +
                          "', but it gets the emitter, of type '" + receiver->name() + "'");
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
      const Type *self = resolveType(*fn->params[0].type, /*allowVoid=*/false);
      if (!takesThis(self, fn->params[0].loc)) {
        arg = checkExpr(arg);
        return arg;
      }
      if (fn->isAsync) {
        diags_.error(fe->loc(), "an async listener cannot take 'this'; use the emitter by name");
        arg = checkExpr(arg);
        return arg;
      }
      kind = ListenerKind::This;
      const Type *wanted = withThis(self);
      Signature sig = signatureOf(wanted);
      sig.voidResult = true;
      arg = checkFunctionExpr(fe, &sig);
      if (arg->type->isError())
        return arg;
      return convert(coerce(arg, wanted), Type::unknown());
    }
    // An arrow written here gets the arguments of the event; an async one
    // gives a Promise, which the emitter lets go (docs/events.md, section 3).
    if (fn->isAsync)
      kind = ListenerKind::Async;
    Signature sig = signatureOf(fn->isAsync ? async : sync);
    sig.voidResult = !fn->isAsync;
    arg = checkFunctionExpr(fe, &sig);
    if (arg->type->isError())
      return arg;
    return convert(coerce(arg, fn->isAsync ? async : sync), Type::unknown());
  }
  arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  if (!type->isFunction()) {
    diags_.error(arg->loc(), "a listener is a function, but this is '" + type->name() + "'");
    return arg;
  }
  if (type->hasThis()) {
    if (!takesThis(type->params()[0], arg->loc()))
      return arg;
    kind = ListenerKind::This;
    if (type == withThis(type->params()[0]))
      return convert(arg, Type::unknown());
    diags_.error(arg->loc(), "the listeners of '" + event.str() + "' get '" + args->name().str() +
                                 "', but this function is '" + type->name().str() + "'");
    return arg;
  }
  bool isAsync = type->ret()->isPromise();
  if (isAsync)
    kind = ListenerKind::Async;
  const Type *wanted = isAsync ? async : sync;
  if (type == wanted)
    return convert(arg, Type::unknown());
  if (isAsync && type->ret() != async->ret()) {
    diags_.error(arg->loc(), "an async listener returns 'Promise<void>', but this one returns '" +
                                 type->ret()->name() +
                                 "', which the emitter would drop; wrap it, "
                                 "e.g. 'async (...) => { await f(...) }'");
    return arg;
  }
  if (Expr *adapted = adaptFunction(arg, wanted))
    return convert(adapted, Type::unknown());
  diags_.error(arg->loc(), "the listeners of '" + event.str() + "' get '" + args->name().str() +
                               "', but this function is '" + type->name().str() +
                               "'; pass an arrow that calls it with what it needs");
  return arg;
}

//===----------------------------------------------------------------------===//
// Event maps found from emit() calls
//===----------------------------------------------------------------------===//

std::string Sema::placeOf(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = diags_.sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (!id)
    return "?";
  auto [line, column] = sm.getLineAndColumn(loc, id);
  return sm.getMemoryBuffer(id)->getBufferIdentifier().str() + ":" + std::to_string(line) + ":" +
         std::to_string(column);
}

namespace {

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

// Whether the next round may take `type` as it is: it has no class in it,
// nor a type that its declaration alone makes (a recursive one), which
// each round declares anew.
bool sameInEveryRound(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (!seen.insert(type).second)
    return true;
  if (type->isClass() || type->isClassRef())
    return false;
  if (type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isBytes() &&
      !type->isDate() && !type->isRegExp() && !type->isIntl() && !type->isCrypto() &&
      !type->isRuntimeObject() && !type->isMatch() && !type->isTimer() &&
      Type::object(std::vector<Field>(type->fields().begin(), type->fields().end()), {},
                   std::vector<Overload>(type->overloads().begin(), type->overloads().end())) !=
          type)
    return false;
  auto all = [&](llvm::ArrayRef<const Type *> types) {
    return llvm::all_of(types, [&](const Type *t) { return !t || sameInEveryRound(t, seen); });
  };
  switch (type->kind()) {
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return sameInEveryRound(type->inner(), seen);
  case Type::Kind::Function:
    return all(type->params()) && sameInEveryRound(type->ret(), seen);
  case Type::Kind::Union:
    return all(type->members());
  case Type::Kind::Dict:
    return all({type->keyType(), type->valueType()});
  case Type::Kind::Iterator:
    return all({type->yielded(), type->returned(), type->sent()});
  case Type::Kind::Result:
    return all({type->okType(), type->errType()});
  case Type::Kind::Object:
    for (const Field &f : type->fields())
      if (!sameInEveryRound(f.type, seen))
        return false;
    return !type->tupleRest() || sameInEveryRound(type->tupleRest(), seen);
  default:
    return true;
  }
}

} // namespace

EventInference::Portable Sema::portable(const Type *type,
                                        llvm::SmallPtrSetImpl<const Type *> &seen) {
  using Kind = EventInference::Portable::Kind;
  EventInference::Portable p;
  llvm::SmallPtrSet<const Type *, 8> visited;
  if (sameInEveryRound(type, visited)) {
    p.same = type;
    return p;
  }
  if (!seen.insert(type).second) {
    p.kind = Kind::Bad; // a recursive type
    return p;
  }
  auto part = [&](const Type *t) { p.parts.push_back(portable(t, seen)); };
  if (type->isClass() || type->isClassRef()) {
    const ClassDecl *cls = type->isClass() ? type->classDecl() : type->classRef();
    if (cls->literal || cls->name.empty()) {
      p.kind = Kind::Bad;
      return p;
    }
    p.kind = type->isClass() ? Kind::Class : Kind::ClassRef;
    const ClassDecl *declared = cls->generic ? cls->generic : cls;
    p.name = declared->name.str();
    if (!declared->builtin)
      p.file = fileOfPlace(placeOf(declared->loc()));
    if (type->isClass() && cls->generic)
      for (const Type *arg : cls->typeArgs)
        part(arg);
    return p;
  }
  switch (type->kind()) {
  case Type::Kind::Optional:
    p.kind = Kind::Optional;
    part(type->inner());
    break;
  case Type::Kind::Array:
    p.kind = Kind::Array;
    part(type->inner());
    break;
  case Type::Kind::Promise:
    p.kind = Kind::Promise;
    part(type->inner());
    break;
  case Type::Kind::Function:
    p.kind = Kind::Function;
    for (const Type *t : type->params())
      part(t);
    part(type->ret());
    p.min = type->minArgs();
    p.rest = type->hasRest();
    break;
  case Type::Kind::Union:
    p.kind = Kind::Union;
    for (const Type *t : type->members())
      part(t);
    break;
  case Type::Kind::Dict:
    p.kind = Kind::Dict;
    p.dict = int(type->dictKind());
    part(type->keyType());
    part(type->valueType());
    break;
  case Type::Kind::Iterator:
    p.kind = Kind::Iterator;
    part(type->yielded());
    part(type->returned());
    part(type->sent());
    break;
  case Type::Kind::Result:
    p.kind = Kind::Result;
    part(type->okType());
    part(type->errType());
    break;
  case Type::Kind::Object:
    if (type->isTuple()) {
      p.kind = Kind::Tuple;
      for (const Field &f : type->fields())
        part(f.type);
      p.min = type->tupleMin();
      p.rest = type->tupleRest() != nullptr;
      if (p.rest)
        part(type->tupleRest());
      break;
    }
    p.kind = Kind::Object;
    for (const Field &f : type->fields()) {
      p.names.push_back(f.name);
      p.readonly.push_back(f.readonly);
      part(f.type);
    }
    break;
  default:
    p.kind = Kind::Bad;
    break;
  }
  seen.erase(type);
  for (const EventInference::Portable &q : p.parts)
    if (q.kind == Kind::Bad)
      p.kind = Kind::Bad;
  return p;
}

const Type *Sema::fromPortable(const EventInference::Portable &p, llvm::SMLoc loc) {
  using Kind = EventInference::Portable::Kind;
  std::vector<const Type *> parts;
  for (const EventInference::Portable &q : p.parts) {
    parts.push_back(fromPortable(q, loc));
    if (!parts.back())
      return nullptr;
  }
  switch (p.kind) {
  case Kind::Same:
    return p.same;
  case Kind::Bad:
    return nullptr;
  case Kind::Class:
  case Kind::ClassRef: {
    ClassDecl *cls = nullptr;
    if (p.file.empty()) {
      cls = errorKindClass(p.name);
      if (!cls && systemErrorClass_ && p.name == systemErrorClass_->name)
        cls = systemErrorClass_;
      if (!cls && statsClass_ && p.name == statsClass_->name)
        cls = statsClass_;
      if (!cls && direntClass_ && p.name == direntClass_->name)
        cls = direntClass_;
    } else {
      for (Module *module : file_->modules)
        for (Stmt *item : module->file.items)
          if (auto *c = dyn_cast<ClassDecl>(item);
              c && !cls && c->name == p.name && fileOfPlace(placeOf(c->loc())) == p.file)
            cls = c;
    }
    if (!cls || !cls->type)
      return nullptr;
    if (p.kind == Kind::ClassRef)
      return Type::classRef(cls, cls->name);
    if (cls->isGeneric()) {
      ClassDecl *inst = instantiateClass(cls, parts, loc);
      return inst ? inst->type : nullptr;
    }
    return cls->type;
  }
  case Kind::Optional:
    return Type::optional(parts[0]);
  case Kind::Array:
    return Type::array(parts[0]);
  case Kind::Promise:
    return Type::promise(parts[0]);
  case Kind::Function: {
    const Type *ret = parts.back();
    parts.pop_back();
    return Type::function(parts, p.min, p.rest, ret);
  }
  case Kind::Union:
    return Type::unionOf(parts);
  case Kind::Dict:
    return Type::dict(Type::DictKind(p.dict), parts[0], parts[1]);
  case Kind::Iterator:
    return Type::iterator(parts[0], parts[1], parts[2]);
  case Kind::Result:
    return Type::result(parts[0], parts[1]);
  case Kind::Tuple: {
    const Type *rest = nullptr;
    if (p.rest) {
      rest = parts.back();
      parts.pop_back();
    }
    return Type::argumentList(parts, p.min, rest);
  }
  case Kind::Object: {
    std::vector<Field> fields;
    for (size_t i = 0; i < parts.size(); ++i)
      fields.push_back({p.names[i], parts[i], p.readonly[i]});
    return Type::object(std::move(fields));
  }
  }
  return nullptr;
}

const Type *Sema::inferredEventMap(const std::string &site) {
  if (auto found = siteMaps_.find(site); found != siteMaps_.end())
    return found->second;
  if (!inference_)
    return nullptr;
  inference_->open = true;
  const Type *map;
  if (!inference_->final) {
    // Opaque, so that the calls on its emitters can tell it is this one.
    Type *opaque = Type::declareObject("the events of " + site);
    opaque->defineFields({});
    openMaps_[opaque] = site;
    map = opaque;
  } else {
    std::vector<Field> fields;
    for (const EventInference::Event &e : inference_->maps[site])
      if (const Type *args = fromPortable(e.args, llvm::SMLoc()))
        fields.push_back({e.name, args, false});
    map = Type::object(std::move(fields));
    inferredMaps_[map] = site;
  }
  siteMaps_[site] = map;
  return map;
}

const Type *Sema::createdType(const NewExpr *create) {
  if (!create->member.empty())
    return nullptr;
  auto *cls = llvm::dyn_cast_or_null<ClassDecl>(lookupType(create->name));
  if (!cls)
    return nullptr;
  if (!cls->isGeneric())
    return cls->type;
  std::vector<const Type *> args;
  for (const TypeRef &ref : create->typeArgs) {
    args.push_back(resolveType(ref, /*allowVoid=*/false));
    if (args.back()->isError())
      return nullptr;
  }
  if (args.empty() && cls->library && cls->name == "EventEmitter")
    if (const Type *events = inferredEventMap("new " + placeOf(create->loc())))
      args.push_back(events);
  if (args.empty())
    return nullptr;
  ClassDecl *inst = instantiateClass(cls, std::move(args), create->loc());
  return inst ? inst->type : nullptr;
}

Expr *Sema::checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  };
  const Type *events = nullptr;
  if (!expr->typeArgs.empty()) {
    if (expr->typeArgs.size() == 1)
      events = resolveType(expr->typeArgs[0], /*allowVoid=*/false);
  } else if (const Type *want = expected ? expected->nonOptional() : nullptr;
             want && classOf(want) && classOf(want)->generic == generic) {
    // `const bus: EventEmitter<Events> = new EventEmitter()`: the map of the
    // place it goes.
    events = classOf(want)->typeArgs[0];
  } else {
    events = inferredEventMap("new " + placeOf(expr->loc()));
    if (!events) {
      diags_.error(expr->loc(), "'new EventEmitter()' needs its event map here, e.g. 'new "
                                "EventEmitter<{ data: [chunk: string]; end: [] }>()'");
      return checkArgsLoosely();
    }
  }
  if (!events) // reported by instantiateClass
    return nullptr;
  if (events->isError())
    return checkArgsLoosely();
  ClassDecl *inst = instantiateClass(generic, {events}, expr->loc());
  if (!inst)
    return checkArgsLoosely();
  expr->typeArgs.clear();
  // `{ captureRejections: true }`: the emitter emits 'error' with the Error
  // of each rejected Promise of an async listener, so that is what the
  // listeners of 'error' get.
  if (!expr->args.empty())
    if (auto *options = dyn_cast<ObjectLitExpr>(expr->args[0]))
      for (const ObjectLitExpr::Property &prop : options->props) {
        auto *off = dyn_cast<BoolLitExpr>(prop.value);
        if (prop.name != "captureRejections" || (off && !off->value))
          continue;
        const Type *error = events->isRecord() ? events->valueType()
                            : events->isObject() && events->field("error")
                                ? events->field("error")->type
                                : nullptr;
        const Type *wanted = Type::tuple({errorClass_->type});
        if (error && error != wanted && !mentionsParam(error))
          diags_.error(prop.loc, "an emitter that captures rejections emits 'error' with the Error "
                                 "of a rejected async listener, but its event map gives 'error' '" +
                                     error->name() + "'; give it '[err: Error]'");
      }
  return checkClassNew(expr, inst, expected);
}

Expr *Sema::collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site) {
  call->type = Type::boolTy();
  member->type = Type::error();
  llvm::SMLoc loc = call->args.empty() ? member->loc() : call->args[0]->loc();
  std::vector<const Type *> elements;
  bool known = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    call->args[i] = i == 0 ? checkArg(call->args[i], Type::string()) : checkExpr(call->args[i]);
    const Type *type = call->args[i]->type;
    if (type->isError() || type->isVoid() || mentionsParam(type))
      known = false;
    if (i > 0)
      elements.push_back(type->isLiteral() ? type->literalBase() : type);
  }
  std::optional<std::string> key = call->args.empty() ? std::nullopt : constantKey(call->args[0]);
  if (!key || !known || isServiceEvent(*key))
    return call;
  const Type *found = Type::tuple(elements);
  std::vector<EventInference::Event> &events = inference_->maps[site];
  for (EventInference::Event &e : events) {
    if (e.name != *key)
      continue;
    // An event emitted with other types: those that hold both, if any, as
    // int and double make double; the last round reports others.
    const Type *before = fromPortable(e.args, loc);
    if (!before || before == found || before->fields().size() != elements.size())
      return call;
    std::vector<const Type *> wider;
    for (size_t i = 0; i < elements.size(); ++i) {
      const Type *both = unify(before->fields()[i].type, elements[i]);
      if (!both)
        return call;
      wider.push_back(both);
    }
    if (const Type *now = Type::tuple(wider); now != before) {
      llvm::SmallPtrSet<const Type *, 8> seen;
      e.args = portable(now, seen);
      inference_->changed = true;
    }
    return call;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  events.push_back({*key, portable(found, seen), shortPlace(placeOf(loc))});
  inference_->changed = true;
  return call;
}

//===----------------------------------------------------------------------===//
// Calls on emitters
//===----------------------------------------------------------------------===//

Expr *Sema::callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                              llvm::StringRef name, llvm::ArrayRef<const Type *> bound,
                              const std::vector<bool> *given, bool super) {
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  assert(!methods.empty() && "a method of the emitter's code");
  FuncSymbol *sym = methods[0]->sym;
  resolveParams(sym);
  llvm::SMLoc loc = member->loc();
  std::vector<const Type *> params = sym->params;
  const Type *ret = returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric()) {
    llvm::SMLoc outerLoc = std::exchange(instanceLoc_, loc);
    for (const Type *&p : params)
      p = substitute(p, sym->typeParams, bound);
    ret = ret ? substitute(ret, sym->typeParams, bound) : Type::error();
    instanceLoc_ = outerLoc;
    // Generic code is checked with its types opaque, and not compiled: the
    // copies of the methods are for the types of its instances.
    if (!trial_ && llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); }))
      if (FuncSymbol *instance = instantiate(sym, bound, loc))
        callee = instance;
  }
  call->superCall = super;
  if (sym->isGeneric() || super) {
    call->method = callee;
  } else if (FunctionDecl *impl = soleImplementation(cls, sym->decl)) {
    call->method = impl->sym;
  } else {
    call->method = sym;
    call->virtualCall = true;
  }
  member->type = functionType(callee);
  call->type = ret ? ret : Type::error();
  // A copy takes the elements of an argument list as parameters of their
  // own (see expandArgumentList).
  if (callee != sym) {
    resolveParams(callee);
    return checkArguments(call, callee->params, callee->minArgs, callee->hasRest, loc, given);
  }
  return checkArguments(call, params, sym->minArgs, sym->hasRest, loc, given);
}

Expr *Sema::checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super) {
  EmitterMethod kind = emitterMethod(member->name);
  if (kind == EmitterMethod::None || inLibrary(call->loc()))
    return nullptr;
  // A class down the chain may declare a method of that name itself.
  std::vector<FunctionDecl *> methods = methodsNamed(cls, member->name, /*isStatic=*/false);
  if (!methods.empty() && !methods[0]->cls->library)
    return nullptr;
  const Type *receiver = member->object->type; // `this` for super.m()
  const Type *events = eventMapOf(receiver);
  if (!events)
    return nullptr;
  call->type = Type::error();
  member->type = Type::error();
  if (!member->typeArgs.empty()) {
    diags_.error(member->loc(), "'" + member->name +
                                    "' takes no type arguments: the event map of "
                                    "the emitter gives the types");
    member->typeArgs.clear();
  }
  if (rejectSpread(call))
    return call;
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i)
      call->args[i] = checkExpr(call->args[i]);
  };
  auto flag = [&](bool value) { return checkExpr(ctx_.make<BoolLitExpr>(call->loc(), value)); };
  llvm::StringRef name = member->name;
  std::string event;
  switch (kind) {
  case EmitterMethod::Emit: {
    if (call->args.empty()) {
      diags_.error(member->loc(), "'emit' needs the name of the event, e.g. 'emit(\"data\", ...)'");
      return call;
    }
    // A round looking for event maps records what the emits pass.
    if (auto open = openMaps_.find(events); open != openMaps_.end())
      return collectEmitterCall(call, member, open->second);
    const Type *args = eventArgs(events, call->args[0], name, &event);
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
    if (auto inferred = inferredMaps_.find(events); inferred != inferredMaps_.end())
      for (const EventInference::Event &e : inference_->maps[inferred->second])
        if (e.name == event)
          first = e.where;
    if (count < min || count > max) {
      if (!first.empty())
        diags_.error(member->loc(), "'" + event + "' is emitted with " + args->name().str() +
                                        " at " + first + ", but with " + std::to_string(count) +
                                        " argument" + (count == 1 ? "" : "s") + " here");
      else
        diags_.error(member->loc(), "the listeners of '" + event + "' get '" + args->name().str() +
                                        "': " + argumentCount(unsigned(min), unsigned(max)) +
                                        ", but got " + std::to_string(count));
      checkArgsLoosely(1);
      return call;
    }
    std::vector<bool> given(call->args.size(), false);
    given[0] = true;
    if (!first.empty()) {
      // Both places in the message (docs/events.md, section 2).
      for (size_t i = 1; i < call->args.size() && i - 1 < args->fields().size(); ++i) {
        const Type *wanted = args->fields()[i - 1].type;
        call->args[i] = checkExpr(call->args[i], wanted);
        const Type *type = call->args[i]->type;
        if (!type->isError() && !isImplicitlyConvertible(type, wanted)) {
          diags_.error(call->args[i]->loc(), "'" + event + "' is emitted with " +
                                                 args->name().str() + " at " + first +
                                                 ", but with '" + type->name().str() +
                                                 "' here; an event has one type of arguments");
          checkArgsLoosely(i + 1);
          return call;
        }
        call->args[i] = coerce(call->args[i], wanted);
        given[i] = true;
      }
    }
    return callLibraryMethod(call, member, cls, "emit", {args}, &given, super);
  }
  case EmitterMethod::Listen:
  case EmitterMethod::Remove: {
    if (!checkArity(call, member->loc(), 2, 2))
      return call;
    const Type *args = eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    ListenerKind listener = ListenerKind::Plain;
    call->args[1] = checkListener(call->args[1], args, event, receiver, listener);
    if (call->args[1]->type->isError())
      return call;
    if (kind == EmitterMethod::Listen)
      call->args.push_back(
          checkExpr(ctx_.make<IntLitExpr>(call->loc(), uint64_t(listener)), Type::intTy()));
    else
      file_->comparesFunctions = true; // the listener is found by what it is
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver; // `this`
    return call;
  }
  case EmitterMethod::Listeners:
  case EmitterMethod::RawListeners: {
    if (!checkArity(call, member->loc(), 1, 1))
      return call;
    const Type *args = eventArgs(events, call->args[0], name, &event);
    if (!args)
      return call;
    call->args.push_back(flag(kind == EmitterMethod::RawListeners));
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, "#listenersOf", {args}, &given, super);
    call->type = Type::array(listenerType(args, Type::voidTy()));
    return call;
  }
  case EmitterMethod::Count: {
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    const Type *args = eventArgs(events, call->args[0], name, &event);
    if (!args) {
      checkArgsLoosely(1);
      return call;
    }
    if (call->args.size() == 2) {
      ListenerKind listener = ListenerKind::Plain;
      call->args[1] = checkListener(call->args[1], args, event, receiver, listener);
      if (call->args[1]->type->isError())
        return call;
      file_->comparesFunctions = true;
    }
    std::vector<bool> given(call->args.size(), true);
    return callLibraryMethod(call, member, cls, name, {}, &given, super);
  }
  case EmitterMethod::RemoveAll: {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    if (!call->args.empty() && !eventArgs(events, call->args[0], name, &event))
      return call;
    std::vector<bool> given(call->args.size(), true);
    callLibraryMethod(call, member, cls, name, {}, &given, super);
    call->type = receiver;
    return call;
  }
  case EmitterMethod::Chain:
    callLibraryMethod(call, member, cls, name, {}, nullptr, super);
    if (!call->type->isError())
      call->type = receiver;
    return call;
  case EmitterMethod::None:
    break;
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// EventTarget
//===----------------------------------------------------------------------===//

Expr *Sema::onabortAssign(AssignExpr *assign, MemberExpr *member, bool logical) {
  ClassDecl *signal = libraryClass("AbortSignal");
  if (!signal || inLibrary(assign->loc()))
    return nullptr;
  ClassDecl *cls = classOf(member->object->type);
  if (!cls || !cls->derivesFrom(signal) || member->object->type->isOptional())
    return nullptr;
  // What node has as getters only.
  if (member->name == "aborted" || member->name == "reason") {
    diags_.error(member->loc(), "'" + member->name +
                                    "' of an AbortSignal is read-only; abort it with its "
                                    "controller, 'controller.abort(reason)'");
    assign->type = Type::error();
    return assign;
  }
  if (member->name != "onabort")
    return nullptr;
  if (assign->op || logical) {
    diags_.error(assign->loc(), "only '=' can set 'onabort'");
    assign->type = Type::error();
    return assign;
  }
  auto *callee = ctx_.make<MemberExpr>(member->loc(), member->object, "setOnabort");
  auto *call = ctx_.make<CallExpr>(assign->loc(), callee, std::vector<Expr *>{assign->value});
  call->type = Type::error();
  std::vector<bool> given{true};
  return callLibraryMethod(call, callee, cls, "setOnabort", {}, &given, false);
}

Expr *Sema::checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  bool add = member->name == "addEventListener";
  if ((!add && member->name != "removeEventListener") || inLibrary(call->loc()))
    return nullptr;
  ClassDecl *eventClass = libraryClass("Event");
  TypeAliasDecl *objectListener = libTypes_.lookup("EventListenerObject");
  if (!eventClass || !objectListener)
    return nullptr;
  call->type = Type::error();
  member->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, member->loc(), 2, 3))
    return call;
  call->args[0] = checkArg(call->args[0], Type::string());
  const Type *event = eventClass->type;
  const Type *object = resolveAlias(objectListener);
  // The kind of event a function takes: Event, or a class down its chain.
  auto eventKind = [&](const Type *type, llvm::SMLoc loc) -> const Type * {
    if (type->isError())
      return nullptr;
    if (ClassDecl *c = classOf(type); c && c->derivesFrom(eventClass) && !type->isOptional())
      return type;
    diags_.error(loc, "a listener of an EventTarget takes an Event, or a kind of it such as "
                      "'CustomEvent<int>', but this one takes '" +
                          type->name() + "'");
    return nullptr;
  };
  Expr *&listener = call->args[1];
  const Type *kind = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(listener)) {
    kind = event;
    if (!fe->fn->params.empty() && fe->fn->params[0].type && !fe->fn->params[0].rest)
      kind = eventKind(resolveType(*fe->fn->params[0].type, /*allowVoid=*/false),
                       fe->fn->params[0].loc);
    if (!kind) {
      listener = checkExpr(listener);
      return call;
    }
    Signature sig;
    sig.params = {kind};
    sig.minArgs = 1;
    sig.ret = Type::voidTy();
    sig.voidResult = true;
    sig.asyncVoid = true;
    listener = checkFunctionExpr(fe, &sig);
  } else if (isa<ObjectLitExpr>(listener)) {
    listener = checkArg(listener, object);
  } else {
    listener = checkExpr(listener);
    const Type *type = listener->type;
    if (type->isFunction()) {
      kind = type->params().empty() ? event : eventKind(type->params()[0], listener->loc());
      if (kind && type->params().empty())
        listener = coerce(listener, Type::function({event}, 1, false, Type::voidTy()));
      else if (kind)
        listener = coerce(listener, Type::function({kind}, 1, false, Type::voidTy()));
    } else {
      listener = coerce(listener, object, "the listener");
    }
  }
  if (listener->type->isError())
    return call;
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (!add) {
    listener = convert(listener, Type::unknown());
    file_->comparesFunctions = true; // the listener is found by what it is
    return callLibraryMethod(call, member, cls, "removeListenerWith", {}, &given, false);
  }
  if (kind)
    return callLibraryMethod(call, member, cls, "addFunctionListener", {kind}, &given, false);
  return callLibraryMethod(call, member, cls, "addObjectListener", {}, &given, false);
}

//===----------------------------------------------------------------------===//
// The functions of node:events
//===----------------------------------------------------------------------===//

Expr *Sema::callLibraryFunction(CallExpr *call, llvm::StringRef name,
                                llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                                llvm::SMLoc loc) {
  FunctionDecl *fn = libFunctions_.lookup(name);
  assert(fn && fn->sym && "a function of the emitter's code");
  FuncSymbol *sym = fn->sym;
  resolveParams(sym);
  // Of a function that does not write it, inferred before its copies are
  // made (on() gives what its async generator does).
  const Type *ret = returnType(sym);
  FuncSymbol *callee = sym;
  if (sym->isGeneric() && !trial_ &&
      llvm::none_of(bound, [](const Type *t) { return mentionsParam(t); }))
    if (FuncSymbol *instance = instantiate(sym, bound, loc))
      callee = instance;
  resolveParams(callee);
  call->func = callee;
  call->type = ret ? substitute(ret, sym->typeParams, bound) : Type::error();
  std::vector<const Type *> params = callee->params;
  if (callee == sym)
    for (const Type *&p : params)
      p = substitute(p, sym->typeParams, bound);
  return checkArguments(call, params, callee->minArgs, callee->hasRest, loc, &given);
}

Expr *Sema::checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  if (inLibrary(call->loc()))
    return nullptr;
  call->type = Type::error();
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i)
      call->args[i] = checkExpr(call->args[i]);
  };
  // The functions that are methods of the emitter they get first.
  if (fn == "getEventListeners" || fn == "listenerCount") {
    if (rejectSpread(call) || !checkArity(call, loc, 2, 2))
      return call;
    Expr *object = checkExpr(call->args[0]);
    if (ClassDecl *target = libraryClass("EventTarget");
        fn == "getEventListeners" && target && classOf(object->type) &&
        classOf(object->type)->derivesFrom(target)) {
      // The listeners of an EventTarget, of the kinds of events they take.
      auto *member = ctx_.make<MemberExpr>(call->loc(), object, "listenersOf");
      auto *method = ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
      method->type = Type::error();
      std::vector<bool> given{false};
      callLibraryMethod(method, member, classOf(object->type), "listenersOf", {}, &given, false);
      return method;
    }
    if (!object->type->isError() && !eventMapOf(object->type)) {
      diags_.error(object->loc(), "'" + fn.str() + "' takes an emitter of node:events" +
                                      (fn == "getEventListeners" ? " or an EventTarget" : "") +
                                      ", but this is '" + object->type->name().str() + "'");
      checkArgsLoosely(1);
      return call;
    }
    if (object->type->isError()) {
      checkArgsLoosely(1);
      return call;
    }
    auto *member = ctx_.make<MemberExpr>(call->loc(), object,
                                         fn == "getEventListeners" ? "listeners" : "listenerCount");
    member->type = Type::error();
    auto *method = ctx_.make<CallExpr>(call->loc(), member, std::vector<Expr *>{call->args[1]});
    Expr *checked = checkEmitterCall(method, member, classOf(object->type), /*super=*/false);
    return checked ? checked : method;
  }
  if (fn != "once" && fn != "on")
    return nullptr;
  // once(emitter, name, { signal }): a Promise of the arguments of the next
  // event, and of its 'error' events, if it has them. on(emitter, name, {
  // signal, close }): an async iterator of the arguments of each event, which
  // its 'error' events end.
  if (rejectSpread(call) || !checkArity(call, loc, 2, 3))
    return call;
  call->args[0] = checkExpr(call->args[0]);
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    checkArgsLoosely(1);
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = given[1] = true;
  if (ClassDecl *target = libraryClass("EventTarget");
      target && classOf(type) && classOf(type)->derivesFrom(target)) {
    if (fn == "on") {
      diags_.error(call->args[0]->loc(), "'on' of node:events takes an emitter of node:events; "
                                         "the events of an EventTarget are not supported yet: "
                                         "listen with 'addEventListener', or wait for one with "
                                         "'await once(target, name)'");
      checkArgsLoosely(1);
      return call;
    }
    call->args[1] = checkArg(call->args[1], Type::string());
    return callLibraryFunction(call, "onceTarget", {}, given, loc);
  }
  const Type *events = eventMapOf(type);
  if (!events) {
    diags_.error(call->args[0]->loc(), "'" + fn.str() +
                                           "' takes an emitter of node:events" +
                                           (fn == "once" ? " or an EventTarget" : "") +
                                           ", but this is '" + type->name().str() + "'");
    checkArgsLoosely(1);
    return call;
  }
  const Type *args = eventArgs(events, call->args[1], fn);
  if (!args) {
    checkArgsLoosely(2);
    return call;
  }
  // Its 'error' events reject the Promise.
  const Type *errors = events->isRecord()       ? events->valueType()
                       : events->field("error") ? events->field("error")->type
                                                : nullptr;
  while (call->args.size() < 3)
    call->args.push_back(ctx_.make<UndefinedExpr>(call->loc()));
  call->args.push_back(checkExpr(ctx_.make<BoolLitExpr>(call->loc(), errors != nullptr)));
  given.resize(call->args.size(), false);
  given.back() = true;
  return callLibraryFunction(call, fn, {args, errors ? errors : Type::tuple({})}, given, loc);
}

std::string Sema::libraryExportHint(llvm::StringRef name) {
  if (name == "errorMonitor" || name == "captureRejectionSymbol")
    return ": in node it is a symbol, and tz has no symbols of its own; the names of events are "
           "strings (docs/events.md, section 4)";
  if (name == "defaultMaxListeners")
    return "; it is 'EventEmitter.defaultMaxListeners'";
  if (name == "captureRejections")
    return "; capture the rejections of each emitter: 'new EventEmitter({ captureRejections: true "
           "})'";
  if (name == "addAbortListener")
    return " yet; listen to the signal: 'signal.addEventListener(\"abort\", listener, { once: "
           "true })'";
  if (name == "EventEmitterAsyncResource" || name == "usingDomains")
    return ": tz has neither async_hooks nor domains";
  if (name == "EventTarget" || name == "Event" || name == "CustomEvent")
    return "; '" + name.str() + "' is global, as in node: use it without an import";
  return "";
}

void Sema::reportEmitterMethodValue(MemberExpr *member) {
  diags_.error(member->loc(), "'" + member->name +
                                  "' of an emitter can only be called: the event "
                                  "map checks each call by the name of its event; wrap it in an "
                                  "arrow, e.g. '(listener) => bus." +
                                  member->name + "(\"data\", listener)'");
}

} // namespace tz
