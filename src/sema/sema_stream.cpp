// Streams (docs/streams.md): node:stream, which tz has written in tz
// (src/lib/stream.ts), and what the compiler adds to that code.
//
// The code of Stream sees a chunk as an unknown; the classes of the program,
// Readable<T>, Writable<T>, Duplex<W, R>, Transform<W, R> and PassThrough<T>,
// give the chunks their types. What their types cannot say in tz is said
// here: a stream of bytes (of Buffers, or of strings once it has an
// encoding) takes a string, a Buffer or a Uint8Array in push() and write();
// write() and end() take a callback or an encoding in the same place;
// pipeline() takes streams and functions of any types, each of which gives
// the chunks the next one takes; and a Duplex works where a Writable is
// expected.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

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

// Whether `place` (as Sema::placeOf gives it) is in the module named
// `module`: "node:stream:12:3" is in "node:stream", not in
// "node:stream/promises".
bool inModule(const std::string &place, llvm::StringRef module) {
  llvm::StringRef at(place);
  return at.consume_front(module) && at.consume_front(":");
}

// A chunk type of a stream of bytes: Buffer, or string.
bool isByteChunk(const Type *type) { return type->isBuffer() || type->isString(); }

} // namespace

//===----------------------------------------------------------------------===//
// The classes
//===----------------------------------------------------------------------===//

bool Sema::isStreamClass(const ClassDecl *cls) const {
  ClassDecl *stream = libraryClass("Stream");
  return cls && stream && cls->derivesFrom(stream);
}

Sema::StreamChunks Sema::streamChunksOf(const ClassDecl *cls) const {
  StreamChunks chunks;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library || !c->generic || c->typeArgs.empty())
      continue;
    llvm::StringRef name = c->generic->name;
    if (name == "Readable" && !chunks.readable)
      chunks.readable = c->typeArgs[0];
    else if (name == "Writable" && !chunks.writable)
      chunks.writable = c->typeArgs[0];
    else if (name == "Duplex" && c->typeArgs.size() == 2 && !chunks.writable)
      chunks.writable = c->typeArgs[0];
  }
  return chunks;
}

void Sema::addStreamBases(ClassDecl *inst, llvm::SMLoc loc) {
  // A Duplex<W, R> is a Writable<W> too, as node's Duplex is (Symbol.hasInstance
  // of Writable): their objects lie in memory as Stream's do (see
  // src/lib/stream.ts).
  ClassDecl *generic = inst->generic;
  ClassDecl *writable = libraryClass("Writable");
  if (!generic || !generic->library || generic->name != "Duplex" || !writable ||
      inst->typeArgs.size() != 2)
    return;
  if (ClassDecl *side = instantiateClass(writable, {inst->typeArgs[0]}, loc)) {
    addBase(inst->type, side->type);
    if (inst->isEmitted() && !llvm::is_contained(file_->subtypes, inst->type))
      file_->subtypes.push_back(inst->type);
  }
}

// `class Upper extends Transform { _transform(chunk, encoding, callback) {
// ... } }`: the method replaces the one the stream calls, as in node, so it
// takes its parameters. They are known before the methods of the library's
// classes are (an instance of a generic class gets its methods once all
// classes have theirs), from the chunk types of the class it extends.
std::optional<std::vector<const Type *>> Sema::streamHookParams(const FunctionDecl *fn) {
  if (!isStreamHook(fn, /*linked=*/false))
    return std::nullopt;
  // The class of node:stream the class extends, and its chunk types.
  const ClassDecl *lib = fn->cls->parent;
  while (lib && !lib->library)
    lib = lib->parent;
  if (!lib || !lib->generic)
    return std::nullopt;
  llvm::StringRef kind = lib->generic->name;
  bool transform = kind == "Transform" || kind == "PassThrough";
  const Type *readable = nullptr, *writable = nullptr;
  if (kind == "Readable" && lib->typeArgs.size() == 1) {
    readable = lib->typeArgs[0];
  } else if (kind == "Writable" && lib->typeArgs.size() == 1) {
    writable = lib->typeArgs[0];
  } else if ((kind == "Duplex" || transform) && lib->typeArgs.size() == 2) {
    writable = lib->typeArgs[0];
    readable = lib->typeArgs[1];
  } else {
    return std::nullopt;
  }
  auto alias = [&](llvm::StringRef name) -> const Type * {
    TypeAliasDecl *decl = libTypes_.lookup(name);
    return decl ? decl->resolved : nullptr;
  };
  const Type *callback = alias("StreamCallback");
  llvm::StringRef name = fn->name;
  std::vector<const Type *> params;
  if (name == "_read" && readable) {
    params = {Type::intTy()};
  } else if (name == "_final" || name == "_construct") {
    params = {callback};
  } else if (name == "_destroy") {
    params = {Type::optional(errorClass_->type), callback};
  } else if (name == "_flush" && transform) {
    params = {alias("TransformCallback")};
  } else if (name == "_transform" && transform) {
    params = {writable, Type::string(), alias("TransformCallback")};
  } else if (name == "_write" && writable) {
    params = {writable, Type::string(), callback};
  } else if (name == "_writev" && writable) {
    TypeAliasDecl *entry = libTypes_.lookup("WriteChunk");
    if (const Type *type = entry ? instantiateAlias(entry, {writable}, fn->loc()) : nullptr)
      params = {Type::array(type), callback};
  }
  if (params.empty() || llvm::is_contained(params, nullptr))
    return std::nullopt;
  return params;
}

bool Sema::isStreamHook(const FunctionDecl *fn, bool linked) const {
  static const llvm::StringLiteral hooks[] = {"_read",  "_write", "_writev",  "_transform",
                                              "_flush", "_final", "_destroy", "_construct"};
  const ClassDecl *cls = fn->cls;
  return cls && !fn->isStatic && !cls->library && !cls->literal && cls->parent &&
         llvm::is_contained(hooks, fn->name) && (!linked || isStreamClass(cls->parent));
}

void Sema::reportStreamHook(const FunctionDecl *fn, llvm::ArrayRef<FunctionDecl *> inherited) {
  const FuncSymbol *hook = nullptr;
  for (const FunctionDecl *v : inherited)
    if (v->sym && v->cls && v->cls->library)
      hook = v->sym;
  if (!hook)
    return;
  for (const Type *t : fn->sym->params)
    if (!t || t->isError())
      return;
  std::string params;
  for (size_t i = 0; i < hook->params.size(); ++i) {
    if (i)
      params += ", ";
    llvm::StringRef name = hook->decl && i < hook->decl->params.size()
                               ? hook->decl->params[i].name
                               : llvm::StringRef("arg");
    params += name.str() + ": " + hook->params[i]->name().str();
  }
  std::string chunks;
  if (fn->name == "_write" || fn->name == "_writev" || fn->name == "_transform")
    chunks = "; for chunks of another type, give the class it extends type arguments, e.g. "
             "'extends Writable<string>'";
  diags_.error(fn->loc(), "'" + fn->name + "' is what node:stream calls, as '" + fn->name + "(" +
                              params + ")', and a method with other parameters is another "
                              "variant of the name, which it never calls; take those parameters, "
                              "or leave out their types" + chunks);
}

//===----------------------------------------------------------------------===//
// The chunk types of object-mode streams, found from the program
//===----------------------------------------------------------------------===//

// An object-mode side without a type gets the type of what the program puts
// into it, as an emitter without a map gets the events emit() gives it
// (docs/streams.md, section 2): push() for a readable side, write(), end()
// and what is piped in for a writable one. While a round looks for them the
// side has an opaque type of its own, an open map of sema_events.cpp whose
// one event "chunk" the calls give the types of; in the last round the type
// found.

const Type *Sema::inferredChunkType(const std::string &site, llvm::SMLoc loc) {
  const Type *map = inferredEventMap(site);
  if (!map)
    return nullptr;
  if (openMaps_.count(map))
    return map; // the placeholder of this round
  const Field *chunk = map->field("chunk");
  if (!chunk || chunk->type->fields().empty())
    return nullptr;
  (void)loc;
  return chunk->type->fields()[0].type;
}

void Sema::noteStreamCallback(CallExpr *call, const VarSymbol *var) {
  for (const StreamCallbackSite &site : streamCallbacks_) {
    if (site.fn != var->owner || site.param != var->name || call->args.size() < 2)
      continue;
    const Expr *data = call->args[1];
    while (auto *conv = dyn_cast<CastExpr>(data)) {
      if (!conv->isImplicit())
        break;
      data = conv->operand;
    }
    if (!isa<UndefinedExpr>(data))
      recordStreamChunk(site.chunk, data->type, data->loc());
  }
}

bool Sema::recordStreamChunk(const Type *chunk, const Type *value, llvm::SMLoc loc) {
  auto open = openMaps_.find(chunk);
  if (open == openMaps_.end())
    return false;
  if (value->isError() || value->isVoid() || value->isUndefined() || mentionsParam(value))
    return true;
  if (value->isLiteral())
    value = value->literalBase();
  // `push(more ? x : undefined)`: undefined ends the stream.
  value = value->nonOptional();
  const Type *found = Type::tuple({value});
  std::vector<EventInference::Event> &events = inference_->maps[open->second];
  for (EventInference::Event &e : events) {
    if (e.name != "chunk")
      continue;
    // Chunks of other types: one that holds both, as int and double make
    // double; the last round reports others.
    const Type *before = fromPortable(e.args, loc);
    if (!before || before == found || before->fields().size() != 1)
      return true;
    const Type *both = unify(before->fields()[0].type, value);
    if (!both)
      return true;
    if (const Type *now = Type::tuple({both}); now != before) {
      llvm::SmallPtrSet<const Type *, 8> seen;
      e.args = portable(now, seen);
      inference_->changed = true;
    }
    return true;
  }
  llvm::SmallPtrSet<const Type *, 8> seen;
  events.push_back({"chunk", portable(found, seen), placeOf(loc)});
  inference_->changed = true;
  return true;
}

//===----------------------------------------------------------------------===//
// new Readable() and the others
//===----------------------------------------------------------------------===//

namespace {

// What the options literal of `new Readable({...})` says of the chunk
// types: an option written as a literal, `true`, `false` or a string.
struct StreamOptionFlags {
  const ObjectLitExpr *lit = nullptr;
  // -1 when the option is not written, 0 or 1 for false or true.
  int flag(llvm::StringRef name) const {
    if (!lit)
      return -1;
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (prop.spread || prop.name != name)
        continue;
      if (auto *b = dyn_cast<BoolLitExpr>(prop.value))
        return b->value ? 1 : 0;
      return 1; // a value known only when the program runs
    }
    return -1;
  }
  // Whether a string option is written (not undefined).
  bool has(llvm::StringRef name) const {
    if (!lit)
      return false;
    for (const ObjectLitExpr::Property &prop : lit->props)
      if (!prop.spread && prop.name == name && !isa<UndefinedExpr>(prop.value))
        return true;
    return false;
  }
};

} // namespace

Expr *Sema::checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  llvm::StringRef name = generic->name;
  bool readable = name == "Readable";
  bool writable = name == "Writable";
  bool duplex = name == "Duplex" || name == "Transform" || name == "PassThrough";
  if (!readable && !writable && !duplex)
    return nullptr;
  if (!expr->typeArgs.empty() || inLibrary(expr->loc()))
    return nullptr;
  // `const r: Readable<string> = new Readable({...})`: the types of the place
  // it goes.
  if (const Type *want = expected ? expected->nonOptional() : nullptr;
      want && classOf(want) && classOf(want)->generic == generic)
    return nullptr;
  StreamOptionFlags options;
  if (!expr->args.empty())
    options.lit = dyn_cast<ObjectLitExpr>(expr->args[0]);
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  // The type the chunk parameter of a function of the literal is declared
  // with: `write(chunk: User, ...)`, `transform(chunk: Row, ...)`.
  auto annotated = [&](llvm::StringRef fn) -> const Type * {
    if (!options.lit)
      return nullptr;
    for (const ObjectLitExpr::Property &prop : options.lit->props) {
      auto *fe = prop.name == fn ? dyn_cast<FunctionExpr>(prop.value) : nullptr;
      if (!fe || fe->fn->params.empty())
        continue;
      const Param &param = fe->fn->params[0].name == "this" && fe->fn->params.size() > 1
                               ? fe->fn->params[1]
                               : fe->fn->params[0];
      if (param.type)
        return resolveType(*param.type, /*allowVoid=*/false);
    }
    return nullptr;
  };
  // The chunk type of a side: of an object-mode side what the program puts
  // into it (see inferredChunkType); a string where it decodes text, a
  // Buffer otherwise.
  std::string site = "stream " + placeOf(expr->loc());
  auto side = [&](bool objectMode, bool text, llvm::StringRef key, const Type *fallback,
                  llvm::StringRef what) -> const Type * {
    if (!objectMode)
      return text ? Type::string() : buffer;
    if (const Type *found = inferredChunkType(site + " " + key.str(), expr->loc()))
      return found;
    if (fallback)
      return fallback;
    diags_.error(expr->loc(), "cannot infer the type of the " + what.str() + " of this object-mode " +
                                  name.str() + ": no code of the program " +
                                  (key == "R" ? "pushes into it" : "writes to it") +
                                  "; give the type, e.g. 'new " + name.str() + "<User>({ " +
                                  "objectMode: true, ... })'");
    return nullptr;
  };
  bool objectMode = options.flag("objectMode") == 1;
  auto failed = [&] {
    expr->type = Type::error();
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    return expr;
  };
  std::vector<const Type *> args;
  if (readable) {
    const Type *t = side(objectMode, options.has("encoding"), "R", nullptr, "chunks");
    if (!t)
      return failed();
    args.push_back(t);
  } else if (writable) {
    const Type *t = objectMode ? annotated("write") : nullptr;
    if (!t)
      t = side(objectMode, options.flag("decodeStrings") == 0, "W", nullptr, "chunks");
    if (!t)
      return failed();
    args.push_back(t);
  } else {
    bool writableObjects = objectMode || options.flag("writableObjectMode") == 1;
    bool readableObjects = objectMode || options.flag("readableObjectMode") == 1;
    const Type *w = writableObjects ? annotated(name == "Duplex" ? "write" : "transform") : nullptr;
    if (!w)
      w = side(writableObjects, options.flag("decodeStrings") == 0, "W", nullptr,
               "written chunks");
    if (!w)
      return failed();
    // What a Transform gives is what it takes, unless it pushes others or
    // gives others to the callbacks of transform() and flush().
    const Type *r = side(readableObjects, options.has("encoding"), "R",
                         name == "Duplex" || !writableObjects ? nullptr : w, "chunks it gives");
    if (!r)
      return failed();
    if (openMaps_.count(r) && options.lit)
      for (const ObjectLitExpr::Property &prop : options.lit->props) {
        auto *fe = dyn_cast<FunctionExpr>(prop.value);
        size_t at = prop.name == "transform" ? 2 : prop.name == "flush" ? 0 : ~size_t(0);
        if (fe && at < fe->fn->params.size())
          streamCallbacks_.push_back({fe->fn, fe->fn->params[at].name, r});
      }
    args = {w, r};
  }
  ClassDecl *inst = instantiateClass(generic, std::move(args), expr->loc());
  if (!inst)
    return nullptr;
  return checkClassNew(expr, inst, expected);
}

//===----------------------------------------------------------------------===//
// The intrinsics of src/lib/stream.ts
//===----------------------------------------------------------------------===//

Expr *Sema::checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg) {
  switch (call->builtin) {
  case Builtin::LibChunk:
  case Builtin::LibIsChunk:
    // `__tz_chunk<T>(u)`: the T an unknown holds; `__tz_isChunk<T>(u)`:
    // whether it holds one.
    call->args[0] = checkArg(call->args[0], Type::unknown());
    call->type = call->builtin == Builtin::LibChunk ? typeArg : Type::boolTy();
    if (call->builtin == Builtin::LibIsChunk) {
      // The type to test for, which CodeGen reads off an argument it does
      // not evaluate.
      Expr *witness = ctx_.make<UndefinedExpr>(call->loc());
      witness->type = typeArg;
      call->args.push_back(witness);
    }
    return call;
  case Builtin::LibChunkKind: {
    // `__tz_chunkKind<T>()`: 0 for Buffer, 1 for string, 2 for anything else;
    // known in each copy.
    unsigned kind = typeArg->isBuffer() ? 0 : typeArg->isString() ? 1 : 2;
    return checkExpr(ctx_.make<IntLitExpr>(call->loc(), uint64_t(kind)), Type::intTy());
  }
  case Builtin::LibTypeName: {
    // `__tz_typeName<T>()`: the name of T, for messages.
    llvm::SmallVector<llvm::UTF16, 32> units;
    llvm::convertUTF8ToUTF16String(typeArg->name(), units);
    return checkExpr(ctx_.make<StringLitExpr>(call->loc(), std::u16string(units.begin(), units.end())),
                     Type::string());
  }
  case Builtin::LibOverrides: {
    // `__tz_overrides(this, "m")`: whether the class of the object replaces
    // the method m that the class the code knows it by has.
    call->args[0] = checkExpr(call->args[0]);
    call->type = Type::boolTy();
    ClassDecl *cls = classOf(call->args[0]->type);
    auto *lit = dyn_cast<StringLitExpr>(call->args[1]);
    if (!cls || !lit) {
      diags_.error(name->loc(), "'__tz_overrides' takes an object of a class and the name of a "
                                "method, as a string literal");
      return call;
    }
    std::string method = utf8(lit->value);
    std::vector<FunctionDecl *> methods = methodsNamed(cls, method, /*isStatic=*/false);
    if (methods.empty() || !methods[0]->sym) {
      diags_.error(lit->loc(), "class '" + cls->name + "' has no method '" + method + "'");
      return call;
    }
    call->args[1] = checkExpr(call->args[1]);
    call->method = methods[0]->sym;
    return call;
  }
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk:
  case Builtin::LibStdWrite: {
    const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
    std::vector<const Type *> params;
    const Type *result = Type::promise(Type::intTy());
    switch (call->builtin) {
    case Builtin::LibFsOpen:
      params = {Type::string(), Type::string(), Type::intTy()};
      break;
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
      params = {Type::intTy(), buffer, Type::intTy(), Type::intTy(), Type::doubleTy()};
      break;
    case Builtin::LibFsClose:
      params = {Type::intTy(), Type::boolTy()};
      break;
    case Builtin::LibStdinChunk:
      params = {Type::intTy()};
      result = Type::promise(Type::optional(buffer));
      break;
    default:
      params = {Type::intTy(), buffer};
      result = Type::voidTy();
      break;
    }
    for (size_t i = 0; i < params.size(); ++i)
      call->args[i] = checkArg(call->args[i], params[i]);
    call->type = result;
    file_->usesAsync = true;
    calledFunction(); // the runtime settles promises, which run code
    return call;
  }
  default:
    return call;
  }
}

//===----------------------------------------------------------------------===//
// push(), unshift(), write() and end() of the program
//===----------------------------------------------------------------------===//

const Type *Sema::streamCallbackType() {
  return Type::function({Type::optional(errorClass_->type)}, 0, false, Type::voidTy());
}

Expr *Sema::streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing,
                           llvm::StringRef method, bool checked) {
  // undefined ends the stream, as null does in node.
  if (eof && isa<UndefinedExpr>(arg))
    return convert(checked ? arg : checkExpr(arg), Type::unknown());
  if (!isByteChunk(chunk)) {
    // An object-mode stream takes its chunk type.
    const Type *want = eof ? Type::optional(chunk) : chunk;
    arg = checked ? coerce(arg, want) : checkArg(arg, want);
    return convert(arg, Type::unknown());
  }
  if (!checked)
    arg = checkExpr(arg);
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  // A string, a Buffer or a Uint8Array, as node takes; a Writable<string>
  // gives its _write() the strings written, and takes only strings.
  bool onlyStrings = writing && chunk->isString();
  auto fits = [&](const Type *t) {
    if (t->isLiteral())
      t = t->literalBase();
    if (t->isString())
      return true;
    return !onlyStrings && (t->isBuffer() || t->bytes() == Type::Bytes::Uint8);
  };
  const Type *value = eof ? type->nonOptional() : type;
  bool ok = value->isUnion() ? llvm::all_of(value->members(), fits) : fits(value);
  if (!ok) {
    std::string takes = onlyStrings ? "a string" : "a string, a Buffer or a Uint8Array";
    diags_.error(arg->loc(), "'" + method.str() + "' of a stream of " + chunk->name().str() +
                                 " takes " + takes + ", but this is '" + type->name().str() + "'" +
                                 (onlyStrings ? "; a Writable<string> gives its _write() the "
                                                "strings written; make it a Writable<Buffer> to "
                                                "write bytes"
                                              : "; to write objects, make the stream an "
                                                "object-mode one of their type, e.g. 'new "
                                                "Readable<User>({ objectMode: true, ... })'"));
    return arg;
  }
  return convert(arg, Type::unknown());
}

Expr *Sema::checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  llvm::StringRef name = member->name;
  if (name != "push" && name != "unshift" && name != "write" && name != "end" &&
      name != "setEncoding" && name != "pipe")
    return nullptr;
  if (inLibrary(call->loc()) || !isStreamClass(cls))
    return nullptr;
  // A class down the chain may declare a method of that name itself.
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  if (!methods.empty() && !methods[0]->cls->library)
    return nullptr;
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  if (rejectSpread(call))
    return call;
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i)
      call->args[i] = checkExpr(call->args[i]);
  };
  if (name == "setEncoding")
    return checkSetEncoding(call, member, cls);
  if (name == "pipe")
    return checkStreamPipe(call, member, cls);
  StreamChunks chunks = streamChunksOf(cls);
  bool reading = name == "push" || name == "unshift";
  const Type *chunk = reading ? chunks.readable : chunks.writable;
  if (!chunk) {
    diags_.error(member->loc(), "'" + receiver->name() + "' has no " + name.str() + "(): " +
                                    (reading ? "it is not readable" : "it is not writable"));
    checkArgsLoosely(0);
    return call;
  }
  llvm::SMLoc loc = call->loc();
  auto undefinedOf = [&](const Type *type) {
    return convert(checkExpr(ctx_.make<UndefinedExpr>(loc)), type);
  };
  const Type *callback = streamCallbackType();
  const Type *encodingType = Type::optional(Type::string());
  // The second argument of write() and end(): a callback, or an encoding;
  // to tell, the argument is checked, unless it is a function written there.
  llvm::SmallPtrSet<const Expr *, 4> checked;
  auto isCallback = [&](Expr *&arg) {
    if (isa<FunctionExpr>(arg))
      return true;
    arg = checkExpr(arg);
    checked.insert(arg);
    return arg->type->nonOptional()->isFunction();
  };
  auto checkedAs = [&](Expr *arg, const Type *type) {
    return checked.contains(arg) ? coerce(arg, type) : checkArg(arg, type);
  };
  std::vector<Expr *> args;
  llvm::StringRef impl;
  // A round that looks for the chunk type of an object-mode side.
  if (openMaps_.count(chunk) && !call->args.empty()) {
    size_t at = name == "end" && call->args.size() == 1 && isa<FunctionExpr>(call->args[0])
                    ? call->args.size()
                    : 0;
    for (size_t i = 0; i < call->args.size(); ++i) {
      call->args[i] = checkExpr(call->args[i]);
      if (i == at && !isa<UndefinedExpr>(call->args[i]))
        recordStreamChunk(chunk, call->args[i]->type, call->args[i]->loc());
    }
    call->type = name == "end" ? receiver : Type::boolTy();
    return call;
  }
  if (reading) {
    // push(chunk, encoding?) and unshift(chunk, encoding?).
    if (!checkArity(call, member->loc(), 1, 2))
      return call;
    args.push_back(
        streamChunkArg(call->args[0], chunk, /*eof=*/true, /*writing=*/false, name, false));
    args.push_back(call->args.size() > 1 ? coerce(checkExpr(call->args[1], encodingType),
                                                  encodingType)
                                         : undefinedOf(encodingType));
    impl = name == "push" ? "_pushChunk" : "_unshiftChunk";
  } else if (name == "write") {
    // write(chunk), write(chunk, cb), write(chunk, encoding), write(chunk,
    // encoding, cb).
    if (!checkArity(call, member->loc(), 1, 3))
      return call;
    args.push_back(
        streamChunkArg(call->args[0], chunk, /*eof=*/false, /*writing=*/true, name, false));
    Expr *encoding = nullptr;
    Expr *cb = nullptr;
    if (call->args.size() == 2) {
      if (isCallback(call->args[1]))
        cb = call->args[1];
      else
        encoding = call->args[1];
    } else if (call->args.size() == 3) {
      encoding = call->args[1];
      cb = call->args[2];
    }
    args.push_back(encoding ? checkedAs(encoding, encodingType) : undefinedOf(encodingType));
    args.push_back(cb ? checkedAs(cb, Type::optional(callback))
                      : undefinedOf(Type::optional(callback)));
    impl = "_writeBool";
  } else {
    // end(), end(cb), end(chunk), end(chunk, cb), end(chunk, encoding),
    // end(chunk, encoding, cb).
    if (!checkArity(call, member->loc(), 0, 3))
      return call;
    Expr *chunkArg = nullptr;
    Expr *encoding = nullptr;
    Expr *cb = nullptr;
    size_t count = call->args.size();
    if (count >= 1 && isCallback(call->args[0])) {
      cb = call->args[0];
      if (count > 1) {
        diags_.error(call->args[1]->loc(), "end(callback) takes nothing after the callback");
        checkArgsLoosely(1);
        return call;
      }
    } else if (count >= 1) {
      chunkArg = call->args[0];
      if (count == 2) {
        if (isCallback(call->args[1]))
          cb = call->args[1];
        else
          encoding = call->args[1];
      } else if (count == 3) {
        encoding = call->args[1];
        cb = call->args[2];
      }
    }
    args.push_back(checkExpr(ctx_.make<BoolLitExpr>(loc, chunkArg != nullptr)));
    args.push_back(chunkArg ? streamChunkArg(chunkArg, chunk, /*eof=*/false, /*writing=*/true,
                                             name, checked.contains(chunkArg))
                            : undefinedOf(Type::unknown()));
    args.push_back(encoding ? checkedAs(encoding, encodingType) : undefinedOf(encodingType));
    args.push_back(cb ? checkedAs(cb, Type::optional(callback))
                      : undefinedOf(Type::optional(callback)));
    impl = "_endWith";
  }
  for (Expr *arg : args)
    if (arg->type->isError())
      return call;
  call->args = std::move(args);
  member->name = impl;
  std::vector<bool> given(call->args.size(), true);
  callLibraryMethod(call, member, cls, impl, {}, &given, false);
  if (impl == "_endWith" && !call->type->isError())
    call->type = receiver; // `this`
  return call;
}

//===----------------------------------------------------------------------===//
// setEncoding()
//===----------------------------------------------------------------------===//

namespace {

// How the code before a setEncoding() used the stream: it escaped there,
// where code that sees its chunks as Buffers may get the strings.
struct StreamEscape {
  llvm::SMLoc loc;
  std::string how;
};

} // namespace

const Type *Sema::streamViewOf(const FieldPath &path, llvm::SMLoc loc) const {
  for (auto it = streamViews_.rbegin(); it != streamViews_.rend(); ++it)
    if (it->path == path && it->from.getPointer() < loc.getPointer())
      return it->type;
  return nullptr;
}

const Type *Sema::encodedStreamType(ClassDecl *cls, llvm::SMLoc loc) {
  // The class of the program's own stream, if it is one, has its chunk
  // types fixed: the view is the library's class it extends.
  for (ClassDecl *c = cls; c; c = c->parent) {
    if (!c->library || !c->generic || c->typeArgs.empty())
      continue;
    llvm::StringRef name = c->generic->name;
    std::vector<const Type *> args;
    ClassDecl *generic = c->generic;
    if (name == "Readable") {
      args = {Type::string()};
    } else if (name == "Duplex" || name == "Transform") {
      args = {c->typeArgs[0], Type::string()};
    } else if (name == "PassThrough") {
      args = {c->typeArgs[0], Type::string()};
    } else {
      continue;
    }
    if (ClassDecl *inst = generic ? instantiateClass(generic, args, loc) : nullptr)
      return inst->type;
    return nullptr;
  }
  return nullptr;
}

Expr *Sema::checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  const Type *receiver = member->object->type;
  StreamChunks chunks = streamChunksOf(cls);
  llvm::SMLoc loc = member->loc();
  auto plainCall = [&]() -> Expr * {
    callLibraryMethod(call, member, cls, "setEncoding", {}, nullptr, false);
    if (!call->type->isError())
      call->type = receiver;
    return call;
  };
  if (!chunks.readable) {
    diags_.error(loc, "'" + receiver->name() + "' has no setEncoding(): it is not readable");
    return call;
  }
  // A stream of strings already: another encoding, the same type.
  if (chunks.readable->isString())
    return plainCall();
  if (!chunks.readable->isBuffer()) {
    diags_.error(loc, "setEncoding() decodes the Buffers of a stream of bytes, and this is an "
                      "object-mode stream of " + chunks.readable->name().str() +
                      "; push strings into it instead");
    return call;
  }
  // The stream must be known by a constant, or a read-only field of one,
  // whose type changes from here on (docs/streams.md, section 2).
  std::optional<FieldPath> path;
  const Expr *object = member->object;
  while (auto *conv = dyn_cast<CastExpr>(object)) {
    if (!conv->isImplicit())
      break;
    object = conv->operand;
  }
  // process.stdin: a stream of the whole program, which the entry's
  // top-level code may set the encoding of.
  bool stdinStream = false;
  if (auto *c = dyn_cast<CallExpr>(object); c && c->func && c->func->name == "processStdin") {
    if (!module_ || !module_->isEntry || !functions_.empty() || scopes_.size() != 1) {
      diags_.error(loc, "'process.stdin.setEncoding()' changes the type of the chunks of stdin for "
                        "the code after it, so it goes in the top-level code of the program's "
                        "main file, before any other use of process.stdin");
      return call;
    }
    stdinStream = true;
    path = FieldPath{};
    path->var = processStdinVar();
  } else if (auto *name = dyn_cast<NameExpr>(object); name && name->var) {
    path = FieldPath{};
    path->var = name->var;
  } else {
    path = fieldPath(object);
  }
  auto reject = [&](const std::string &why) {
    diags_.error(loc, "setEncoding() makes the chunks of the stream strings from here on, so its "
                      "type changes here; " + why + ": give the stream its encoding where it is "
                      "created, e.g. 'createReadStream(path, \"utf8\")' or 'new Readable({ "
                      "encoding: \"utf8\", ... })'");
    return call;
  };
  if (!path || !path->var)
    return reject("but this stream is not known by a constant");
  const VarSymbol *root = path->var;
  if (!root->isConst)
    return reject("but '" + root->name.str() +
                  "' may hold another stream later; declare it with 'const'");
  // The fields on the way must be read-only.
  {
    const Type *at = root->type;
    for (llvm::StringRef field : path->names) {
      const Field *f = at ? at->nonOptional()->field(field) : nullptr;
      if (!f || !f->readonly)
        return reject("but '" + field.str() + "' is a field that code may set to another stream");
      at = f->type;
    }
  }
  // In the block that declares it, not in an if or a loop: after a
  // setEncoding() that may not run, the type of the chunks is not known.
  size_t declared = stdinStream ? 0 : scopes_.size();
  for (size_t i = scopes_.size(); i-- > 0 && !stdinStream;) {
    auto found = scopes_[i].find(root->name);
    if (found != scopes_[i].end() && found->second.var == root) {
      declared = i;
      break;
    }
  }
  bool branched = stdinStream ? branchDepth_ != 0
                              : declared < scopeBranches_.size() &&
                                    scopeBranches_[declared] != branchDepth_;
  if (declared + 1 != scopes_.size() || branched)
    return reject("but this setEncoding() is in a block of its own (an if, a loop, a function) "
                  "and '" + root->name.str() + "' is declared outside it, so the type would "
                  "depend on whether it runs");
  // What the code before did with the stream.
  std::vector<StreamEscape> escapes;
  // Before the stream this call is on, which starts with its root.
  const Expr *base = member->object;
  for (;;) {
    if (auto *conv = dyn_cast<CastExpr>(base))
      base = conv->operand;
    else if (auto *m = dyn_cast<MemberExpr>(base))
      base = m->object;
    else
      break;
  }
  const char *here = base->loc().getPointer();
  const char *start = stdinStream ? nullptr : root->loc.getPointer();
  auto before = [&](const Expr *e) {
    const char *at = e->loc().getPointer();
    return at > start && at < here;
  };
  // A use of the path: a name of the variable, or `root.f.g`.
  auto isPath = [&](const Expr *e) -> bool {
    while (auto *conv = dyn_cast<CastExpr>(e)) {
      if (!conv->isImplicit())
        break;
      e = conv->operand;
    }
    if (stdinStream) {
      if (auto *c = dyn_cast<CallExpr>(e))
        return c->func && c->func->name == "processStdin";
      return isProcessMember(e, "stdin");
    }
    if (path->names.empty()) {
      auto *name = dyn_cast<NameExpr>(e);
      return name && (name->var == root || (!name->var && name->name == root->name));
    }
    std::optional<FieldPath> p = fieldPath(e);
    return p && *p == *path;
  };
  llvm::SmallPtrSet<const Expr *, 16> safe;
  static const llvm::StringLiteral listening[] = {
      "on", "once", "addListener", "prependListener", "prependOnceListener", "pipe"};
  auto unwrap = [](const Expr *e) {
    while (auto *conv = dyn_cast<CastExpr>(e)) {
      if (!conv->isImplicit())
        break;
      e = conv->operand;
    }
    return e;
  };
  auto classify = [&](const Expr *e) {
    if (!before(e))
      return;
    if (auto *c = dyn_cast<CallExpr>(e)) {
      auto *m = dyn_cast<MemberExpr>(c->callee);
      if (m && isPath(m->object)) {
        if (llvm::is_contained(listening, m->name))
          escapes.push_back({m->loc(), "it got listeners (" + m->name.str() + "())"});
        else
          safe.insert(unwrap(m->object));
      }
    } else if (auto *m = dyn_cast<MemberExpr>(e)) {
      if (isPath(m->object))
        safe.insert(unwrap(m->object));
    }
  };
  std::vector<const Stmt *> body;
  if (FunctionDecl *owner = stdinStream ? nullptr : root->owner; owner && owner->body)
    body.push_back(owner->body);
  else if (module_)
    for (const Stmt *item : module_->file.items)
      body.push_back(item);
  for (const Stmt *stmt : body)
    visitExprs(stmt, classify);
  // The uses in functions written before: they may run after this.
  llvm::SmallPtrSet<const Expr *, 32> direct;
  for (const Stmt *stmt : body)
    visitExprs(stmt, [&](const Expr *e) { direct.insert(e); }, /*intoFunctions=*/false);
  for (const Stmt *stmt : body)
    visitExprs(stmt, [&](const Expr *e) {
      if (!before(e) || isa<CastExpr>(e) || !isPath(e))
        return;
      if (!direct.contains(e))
        escapes.push_back({e->loc(), "a function written before uses it"});
      else if (!safe.contains(e))
        escapes.push_back({e->loc(), "it went elsewhere (passed, stored or returned)"});
    });
  if (!escapes.empty()) {
    const StreamEscape *first = &escapes[0];
    for (const StreamEscape &e : escapes)
      if (e.loc.getPointer() < first->loc.getPointer())
        first = &e;
    diags_.error(loc, "setEncoding() on a stream that is used elsewhere already, which sees its "
                      "chunks as Buffers, but strings would come: " + first->how +
                      " before this; give the stream its encoding where it is created, e.g. "
                      "'createReadStream(path, \"utf8\")', or call setEncoding() first");
    diags_.note(first->loc, "the stream is used here");
    return call;
  }
  const Type *view = encodedStreamType(cls, loc);
  plainCall();
  if (!view || call->type->isError())
    return call;
  call->type = view;
  // From this call on: the constant is not seen outside its block.
  streamViews_.push_back({*path, llvm::SMLoc::getFromPointer(here), view});
  return call;
}

Expr *Sema::checkStreamPipe(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (!checkArity(call, member->loc(), 1, 2))
    return call;
  call->args[0] = checkExpr(call->args[0]);
  Expr *dest = call->args[0];
  if (dest->type->isError())
    return call;
  ClassDecl *destClass = classOf(dest->type);
  if (!destClass || !isStreamClass(destClass) || dest->type->isOptional()) {
    diags_.error(dest->loc(), "pipe() takes the stream to write to, and this is '" +
                                  dest->type->name() + "'");
    return call;
  }
  StreamChunks from = streamChunksOf(cls);
  StreamChunks to = streamChunksOf(destClass);
  if (!from.readable) {
    diags_.error(member->loc(), "'" + member->object->type->name() +
                                    "' has no pipe(): it is not readable");
    return call;
  }
  if (!to.writable) {
    diags_.error(dest->loc(), "pipe() writes to the stream it takes, but '" +
                                  dest->type->name() + "' is not writable");
    return call;
  }
  // A writable side whose type a round looks for gets these chunks.
  if (!recordStreamChunk(to.writable, from.readable, dest->loc()) &&
      !openMaps_.count(from.readable) && !streamTakes(to.writable, from.readable)) {
    diags_.error(dest->loc(), "pipe() writes chunks of " + from.readable->name().str() + " to '" +
                                  dest->type->name().str() + "', which takes " +
                                  to.writable->name().str());
    return call;
  }
  std::vector<bool> given(call->args.size(), false);
  given[0] = true;
  callLibraryMethod(call, member, cls, "pipe", {dest->type}, &given, false);
  if (!call->type->isError())
    call->type = dest->type;
  return call;
}

//===----------------------------------------------------------------------===//
// fs.createReadStream() and fs.createWriteStream()
//===----------------------------------------------------------------------===//

Expr *Sema::checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  llvm::StringRef fn = decl->name;
  bool reading = fn == "createReadStream";
  if (!reading && fn != "createWriteStream")
    return nullptr;
  if (inLibrary(call->loc()))
    return nullptr;
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, loc, 1, 2))
    return call;
  TypeAliasDecl *alias = libTypes_.lookup(reading ? "ReadStreamOptions" : "WriteStreamOptions");
  if (!alias)
    return call;
  const Type *options = Type::optional(resolveAlias(alias));
  const Type *encodingType = Type::optional(Type::string());
  auto undefinedOf = [&](const Type *type) {
    return convert(checkExpr(ctx_.make<UndefinedExpr>(loc)), type);
  };
  Expr *path = checkArg(call->args[0], Type::string());
  Expr *opts = nullptr;
  Expr *encoding = nullptr;
  // The encoding gives strings: written in place of the options, or among
  // them, as a literal (docs/streams.md, section 2).
  bool text = false;
  if (call->args.size() == 2) {
    Expr *arg = call->args[1];
    if (isa<ObjectLitExpr>(arg)) {
      auto *lit = cast<ObjectLitExpr>(arg);
      for (const ObjectLitExpr::Property &prop : lit->props)
        if (!prop.spread && prop.name == "encoding" && !isa<UndefinedExpr>(prop.value))
          text = true;
      opts = checkArg(arg, options);
    } else {
      arg = checkExpr(arg);
      if (arg->type->isString() || arg->type->isStringLiteral()) {
        encoding = coerce(arg, encodingType);
        text = true;
      } else {
        opts = coerce(arg, options);
      }
    }
  }
  if (!opts)
    opts = undefinedOf(options);
  if (!encoding)
    encoding = undefinedOf(encodingType);
  if (path->type->isError() || opts->type->isError() || encoding->type->isError())
    return call;
  call->args = {path, opts, encoding};
  std::vector<bool> given(3, true);
  if (!reading)
    return callLibraryFunction(call, "streamWriteFile", {}, given, loc);
  const Type *chunk = text ? Type::string() : Type::bytesType(Type::Bytes::Buffer);
  return callLibraryFunction(call, "streamReadFile", {chunk}, given, loc);
}

//===----------------------------------------------------------------------===//
// process.stdin, process.stdout and process.stderr
//===----------------------------------------------------------------------===//

Expr *Sema::processStream(MemberExpr *member) {
  llvm::StringRef name = member->name;
  bool in = name == "stdin";
  llvm::StringRef fn = in ? "processStdin" : "processStdout";
  if (!libFunctions_.lookup(fn))
    return nullptr; // not used as a stream (see the loader)
  llvm::SMLoc loc = member->loc();
  std::vector<Expr *> args;
  if (!in)
    args.push_back(checkExpr(ctx_.make<IntLitExpr>(loc, name == "stdout" ? 1 : 2), Type::intTy()));
  auto *callee = ctx_.make<NameExpr>(loc, fn);
  auto *call = ctx_.make<CallExpr>(loc, callee, std::move(args));
  call->type = Type::error();
  std::vector<bool> given(call->args.size(), true);
  Expr *stream = callLibraryFunction(call, fn, {}, given, loc);
  // After process.stdin.setEncoding().
  if (in && !streamViews_.empty()) {
    FieldPath path;
    path.var = processStdinVar();
    if (const Type *view = streamViewOf(path, loc))
      return convert(stream, view);
  }
  return stream;
}

VarSymbol *Sema::processStdinVar() {
  if (!processStdin_) {
    processStdin_ = ctx_.newVar();
    processStdin_->name = "process.stdin";
    processStdin_->isConst = true;
  }
  return processStdin_;
}

//===----------------------------------------------------------------------===//
// Readable.from()
//===----------------------------------------------------------------------===//

const Type *Sema::streamFromOptionsType() {
  TypeAliasDecl *alias = libTypes_.lookup("ReadableFromOptions");
  return alias ? resolveAlias(alias) : nullptr;
}

Expr *Sema::checkStreamFrom(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (rejectSpread(call) || !checkArity(call, member->loc(), 1, 2))
    return call;
  llvm::SMLoc loc = member->loc();
  Expr *source = checkExpr(call->args[0]);
  const Type *type = source->type;
  if (type->isError())
    return call;
  const Type *optionsType = streamFromOptionsType();
  if (!optionsType)
    return call;
  Expr *options = call->args.size() > 1 ? checkArg(call->args[1], Type::optional(optionsType))
                                        : convert(checkExpr(ctx_.make<UndefinedExpr>(loc)),
                                                  Type::optional(optionsType));
  if (options->type->isError())
    return call;
  std::vector<bool> given{true, true};
  // A string or a Buffer: one chunk.
  if (type->isString() || type->isStringLiteral() || type->isBuffer()) {
    const Type *chunk = type->isBuffer() ? type : Type::string();
    call->args = {coerce(source, chunk), options};
    return callLibraryFunction(call, "streamFromValue", {chunk}, given, loc);
  }
  // An async iterable: an async generator, or an object with
  // `[Symbol.asyncIterator]()` (a stream among them).
  if (type->isAsyncIterator()) {
    call->args = {source, options};
    return callLibraryFunction(call, "streamFromAsyncIterator", {type->yielded()}, given, loc);
  }
  if (Expr *async = asyncUserIterator(source); async && async->type->isAsyncIterator()) {
    call->args = {async, options};
    return callLibraryFunction(call, "streamFromAsyncIterator", {async->type->yielded()}, given,
                               loc);
  }
  // Any other iterable: an array, a Set, a generator...
  Expr *iterator = valuesIteratorOf(source);
  if (!iterator) {
    diags_.error(source->loc(), "'Readable.from' takes an iterable, an async iterable, a string "
                                "or a Buffer, but this is '" + type->name() + "'");
    return call;
  }
  call->args = {iterator, options};
  return callLibraryFunction(call, "streamFromIterator", {iterator->type->yielded()}, given, loc);
}

//===----------------------------------------------------------------------===//
// pipeline(), finished() and compose()
//===----------------------------------------------------------------------===//

const Type *Sema::streamSignalOptionsType() {
  ClassDecl *signal = libraryClass("AbortSignal");
  return Type::object({Field{"signal", signal ? signal->type : Type::error(), false}});
}

Expr *Sema::streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                        const Type **sinkValue) {
  llvm::SMLoc loc = arg->loc();
  ClassDecl *stageClass = libraryClass("PipelineStage");
  const Type *stageType = stageClass ? stageClass->type : Type::error();
  const Type *options = streamSignalOptionsType();
  stream = false;
  // A function: the source (the first), a transform of the chunks before it
  // (an async generator), or the sink, whose Promise ends the pipeline.
  auto stageCall = [&](llvm::StringRef fn, std::vector<const Type *> bound, Expr *value) -> Expr * {
    auto *callee = ctx_.make<NameExpr>(loc, fn);
    auto *call = ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{value});
    call->type = Type::error();
    std::vector<bool> given{true};
    Expr *made = callLibraryFunction(call, fn, bound, given, loc);
    return made->type->isError() ? made : convert(made, stageType);
  };
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    Signature sig;
    if (first) {
      sig.params = {options};
      sig.minArgs = 1;
    } else {
      sig.params = {Type::asyncIterator(chunk), options};
      sig.minArgs = 2;
    }
    arg = checkFunctionExpr(fe, &sig);
  } else {
    arg = checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError())
    return arg;
  if (ClassDecl *cls = classOf(type); cls && isStreamClass(cls) && !type->isOptional()) {
    stream = true;
    StreamChunks chunks = streamChunksOf(cls);
    if (!first) {
      if (!chunks.writable) {
        diags_.error(loc, "pipeline() writes the chunks before it to each stream after the first, "
                          "but '" + type->name() + "' is not writable");
        return arg;
      }
      if (chunk && recordStreamChunk(chunks.writable, chunk, loc)) {
        // A round that looks for its type.
      } else if (chunk && !openMaps_.count(chunk) && !streamTakes(chunks.writable, chunk)) {
        diags_.error(loc, "pipeline() writes chunks of " + chunk->name().str() + " to '" +
                              type->name().str() + "', which takes " +
                              chunks.writable->name().str());
        return arg;
      }
    }
    if (!last) {
      if (!chunks.readable) {
        diags_.error(loc, "pipeline() reads each stream but the last, and '" + type->name() +
                              "' is not readable");
        return arg;
      }
      chunk = chunks.readable;
    }
    return convert(arg, Type::unionOf({libraryClass("Stream")->type, stageType}));
  }
  if (type->isFunction()) {
    const Type *ret = type->ret();
    if (first) {
      if (!ret->isAsyncIterator()) {
        diags_.error(loc, "the first function of pipeline() is the source of the chunks: an async "
                          "generator, e.g. 'async function* () { yield \"a\" }'");
        return arg;
      }
      chunk = ret->yielded();
      Expr *stage = stageCall("streamSourceStage", {chunk}, coerce(arg, Type::function({options}, 1, false, ret)));
      return stage->type->isError() ? stage
                                    : convert(stage, Type::unionOf({libraryClass("Stream")->type, stageType}));
    }
    const Type *source = Type::asyncIterator(chunk);
    if (ret->isAsyncIterator()) {
      const Type *fn = Type::function({source, options}, 2, false, ret);
      Expr *stage = stageCall("streamTransformStage", {chunk, ret->yielded()}, coerce(arg, fn));
      chunk = ret->yielded();
      return stage->type->isError() ? stage
                                    : convert(stage, Type::unionOf({libraryClass("Stream")->type, stageType}));
    }
    if (ret->isPromise() && last) {
      const Type *fn = Type::function({source, options}, 2, false, ret);
      const Type *value = ret->promised();
      if (sinkValue)
        *sinkValue = value;
      Expr *stage = value->isVoid()
                        ? stageCall("streamVoidSinkStage", {chunk}, coerce(arg, fn))
                        : stageCall("streamSinkStage", {chunk, value}, coerce(arg, fn));
      return stage->type->isError() ? stage
                                    : convert(stage, Type::unionOf({libraryClass("Stream")->type, stageType}));
    }
    diags_.error(loc, last ? "the last function of pipeline() is an async generator over the "
                             "chunks before it, or an async function that takes them and gives a "
                             "Promise"
                           : "a function in pipeline() is an async generator over the chunks "
                             "before it, e.g. 'async function* (source) { for await (const chunk "
                             "of source) yield chunk }'");
    return arg;
  }
  if (first) {
    // An iterable, or an async one.
    if (type->isAsyncIterator()) {
      chunk = type->yielded();
      Expr *stage = stageCall("streamAsyncIterableStage", {chunk}, arg);
      return stage->type->isError() ? stage
                                    : convert(stage, Type::unionOf({libraryClass("Stream")->type, stageType}));
    }
    if (Expr *iterator = valuesIteratorOf(arg)) {
      chunk = iterator->type->yielded();
      Expr *stage = stageCall("streamIterableStage", {chunk}, iterator);
      return stage->type->isError()
                 ? stage
                 : convert(stage, Type::unionOf({libraryClass("Stream")->type, stageType}));
    }
  }
  diags_.error(loc, "pipeline() takes streams, async generator functions over the chunks and, "
                    "first, an iterable; this is '" + type->name() + "'");
  arg->type = Type::error();
  return arg;
}

Expr *Sema::valuesIteratorOf(Expr *collection) {
  const Type *type = collection->type;
  if (type->isIterator())
    return collection;
  llvm::SMLoc loc = collection->loc();
  if (type->isArray()) {
    const Type *iterator = Type::iterator(type->inner());
    auto *member = ctx_.make<MemberExpr>(loc, collection, "values");
    member->type = Type::function({}, 0, false, iterator);
    return builtinCall(loc, member, Builtin::ArrValues, {}, iterator);
  }
  // A Map, a Set, an object with `[Symbol.iterator]()`.
  Expr *iterator = iterableOf(collection);
  return iterator != collection && iterator->type->isIterator() ? iterator : nullptr;
}

bool Sema::streamTakes(const Type *writable, const Type *chunk) {
  if (writable == chunk)
    return true;
  // A stream of Buffers takes strings too.
  if (writable->isBuffer() && chunk->isString())
    return true;
  return isImplicitlyConvertible(chunk, writable);
}

Expr *Sema::checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  std::string place = placeOf(decl->loc());
  if (inModule(place, "node:internal/fs/streams"))
    return checkFsStreamFunction(call, decl, loc);
  bool promises = inModule(place, "node:stream/promises");
  if (!promises && !inModule(place, "node:stream"))
    return nullptr;
  llvm::StringRef fn = decl->name;
  if (fn != "pipeline" && fn != "finished" && fn != "compose")
    return nullptr;
  if (fn == "finished" && promises)
    return nullptr; // a plain function
  if (inLibrary(call->loc()))
    return nullptr;
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i)
      call->args[i] = checkExpr(call->args[i]);
  };
  ClassDecl *streamClass = libraryClass("Stream");
  if (!streamClass)
    return nullptr;
  const Type *streamType = streamClass->type;
  const Type *callback = streamCallbackType();
  auto undefinedOf = [&](const Type *type) {
    return convert(checkExpr(ctx_.make<UndefinedExpr>(loc)), type);
  };
  if (fn == "finished") {
    // finished(stream, callback) and finished(stream, options, callback).
    if (!checkArity(call, loc, 2, 3))
      return call;
    TypeAliasDecl *alias = libTypes_.lookup("FinishedOptions");
    const Type *options = alias ? Type::optional(resolveAlias(alias)) : Type::error();
    Expr *stream = checkArg(call->args[0], streamType);
    Expr *opts = call->args.size() == 3 ? checkArg(call->args[1], options) : undefinedOf(options);
    Expr *cb = checkArg(call->args.back(), callback);
    call->args = {stream, opts, cb};
    std::vector<bool> given(3, true);
    return callLibraryFunction(call, "streamFinished", {}, given, loc);
  }
  if (fn == "compose") {
    if (call->args.empty()) {
      diags_.error(loc, "compose() takes the streams to make one Duplex of, e.g. 'compose(a, b)'");
      return call;
    }
    std::vector<Expr *> streams;
    const Type *chunk = nullptr;
    StreamChunks head, tail;
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *arg = checkExpr(call->args[i]);
      ClassDecl *cls = classOf(arg->type);
      if (arg->type->isError())
        return call;
      if (!cls || !isStreamClass(cls) || arg->type->isOptional()) {
        diags_.error(arg->loc(), "compose() takes streams, and this is '" + arg->type->name() +
                                     "'; to put a function between them, use pipeline()");
        checkArgsLoosely(i + 1);
        return call;
      }
      StreamChunks chunks = streamChunksOf(cls);
      if (i == 0)
        head = chunks;
      if (i + 1 == call->args.size())
        tail = chunks;
      if (i > 0 && chunk && chunks.writable && !streamTakes(chunks.writable, chunk)) {
        diags_.error(arg->loc(), "compose() writes chunks of " + chunk->name().str() + " to '" +
                                     arg->type->name().str() + "', which takes " +
                                     chunks.writable->name().str());
        return call;
      }
      chunk = chunks.readable;
      streams.push_back(convert(arg, streamType));
    }
    auto *list = ctx_.make<ArrayLitExpr>(loc);
    for (Expr *s : streams)
      list->elements.push_back({s, false});
    list->type = Type::array(streamType);
    const Type *w = head.writable ? head.writable : Type::bytesType(Type::Bytes::Buffer);
    const Type *r = tail.readable ? tail.readable : Type::bytesType(Type::Bytes::Buffer);
    call->args = {list};
    std::vector<bool> given{true};
    return callLibraryFunction(call, "streamCompose", {w, r}, given, loc);
  }
  // pipeline(...stages, callback) of node:stream; pipeline(...stages,
  // options?) of node:stream/promises.
  size_t count = call->args.size();
  Expr *callbackArg = nullptr;
  Expr *optionsArg = nullptr;
  if (!promises) {
    if (count < 3) {
      diags_.error(loc, "pipeline() takes two streams or more and a callback, e.g. "
                        "'pipeline(source, destination, (err) => ...)'");
      checkArgsLoosely(0);
      return call;
    }
    callbackArg = call->args.back();
    --count;
  } else if (count > 0 && isa<ObjectLitExpr>(call->args.back())) {
    optionsArg = call->args.back();
    --count;
  }
  if (count < 2) {
    diags_.error(loc, "pipeline() takes two streams or more, e.g. 'pipeline(source, destination)'");
    checkArgsLoosely(0);
    return call;
  }
  const Type *chunk = nullptr;
  const Type *sinkValue = nullptr;
  std::vector<Expr *> stages;
  bool lastIsStream = false;
  for (size_t i = 0; i < count; ++i) {
    bool stream = false;
    Expr *stage = streamStage(call->args[i], chunk, i == 0, i + 1 == count, stream, &sinkValue);
    if (stage->type->isError()) {
      checkArgsLoosely(i + 1);
      return call;
    }
    stages.push_back(stage);
    if (i + 1 == count)
      lastIsStream = stream;
  }
  ClassDecl *stageClass = libraryClass("PipelineStage");
  const Type *element = Type::unionOf({streamType, stageClass->type});
  auto *list = ctx_.make<ArrayLitExpr>(loc);
  Expr *lastStream = nullptr;
  for (size_t i = 0; i < stages.size(); ++i) {
    if (i + 1 == stages.size() && lastIsStream && !promises) {
      // The last stream goes on its own, as the result has its type.
      auto *conv = dyn_cast<CastExpr>(stages[i]);
      lastStream = conv ? conv->operand : stages[i];
      continue;
    }
    list->elements.push_back({stages[i], false});
  }
  list->type = Type::array(element);
  if (!promises) {
    Expr *cb = checkArg(callbackArg, callback);
    if (cb->type->isError())
      return call;
    std::vector<bool> given;
    if (lastStream) {
      call->args = {list, lastStream, cb};
      given.assign(3, true);
      return callLibraryFunction(call, "streamPipeline", {lastStream->type}, given, loc);
    }
    call->args = {list, cb};
    given.assign(2, true);
    return callLibraryFunction(call, "streamPipelineToStage", {}, given, loc);
  }
  const Type *optionsType = Type::optional(Type::object(
      {Field{"signal", Type::optional(libraryClass("AbortSignal")->type), false},
       Field{"end", Type::optional(Type::boolTy()), false}}));
  Expr *options = optionsArg ? checkArg(optionsArg, optionsType) : undefinedOf(optionsType);
  if (options->type->isError())
    return call;
  call->args = {list, options};
  std::vector<bool> given(2, true);
  if (sinkValue && !sinkValue->isVoid())
    return callLibraryFunction(call, "streamPipelineValue", {sinkValue}, given, loc);
  return callLibraryFunction(call, "streamPipelinePromise", {}, given, loc);
}

} // namespace tz
