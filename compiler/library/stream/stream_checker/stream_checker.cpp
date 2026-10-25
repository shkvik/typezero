#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

// A chunk type of a stream of bytes: Buffer, or string.
bool isByteChunk(const Type *type) { return type->isBuffer() || type->isString(); }

} // namespace

StreamChecker::StreamChecker(Sema &sema) : sema_(sema) {}

Expr *StreamChecker::checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg) {
  switch (call->builtin) {
  case Builtin::LibChunk:
  case Builtin::LibIsChunk:
    // `__tz_chunk<T>(u)`: the T an unknown holds; `__tz_isChunk<T>(u)`:
    // whether it holds one.
    call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::unknown());
    call->type = call->builtin == Builtin::LibChunk ? typeArg : Type::boolTy();
    if (call->builtin == Builtin::LibIsChunk) {
      // The type to test for, which CodeGen reads off an argument it does
      // not evaluate.
      Expr *witness = sema_.ctx_.make<UndefinedExpr>(call->loc());
      witness->type = typeArg;
      call->args.push_back(witness);
    }
    return call;
  case Builtin::LibChunkKind: {
    // `__tz_chunkKind<T>()`: 0 for Buffer, 1 for string, 2 for anything else;
    // known in each copy.
    unsigned kind = typeArg->isBuffer() ? 0 : typeArg->isString() ? 1 : 2;
    return sema_.exprChecker().checkExpr(sema_.ctx_.make<IntLitExpr>(call->loc(), uint64_t(kind)), Type::intTy());
  }
  case Builtin::LibTypeName: {
    // `__tz_typeName<T>()`: the name of T, for messages.
    llvm::SmallVector<llvm::UTF16, 32> units;
    llvm::convertUTF8ToUTF16String(typeArg->name(), units);
    return sema_.exprChecker().checkExpr(
        sema_.ctx_.make<StringLitExpr>(call->loc(), std::u16string(units.begin(), units.end())),
        Type::string());
  }
  case Builtin::LibOverrides: {
    // `__tz_overrides(this, "m")`: whether the class of the object replaces
    // the method m that the class the code knows it by has.
    call->args[0] = sema_.exprChecker().checkExpr(call->args[0]);
    call->type = Type::boolTy();
    ClassDecl *cls = ClassDeclarer::classOf(call->args[0]->type);
    auto *lit = dyn_cast<StringLitExpr>(call->args[1]);
    if (!cls || !lit) {
      sema_.diags_.error(name->loc(), "'__tz_overrides' takes an object of a class and the name of a "
                                "method, as a string literal");
      return call;
    }
    std::string method = utf8(lit->value);
    std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, method, /*isStatic=*/false);
    if (methods.empty() || !methods[0]->sym) {
      sema_.diags_.error(lit->loc(), "class '" + cls->name + "' has no method '" + method + "'");

      return call;
    }
    call->args[1] = sema_.exprChecker().checkExpr(call->args[1]);
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
    for (size_t i = 0; i < params.size(); ++i) {
      call->args[i] = sema_.argumentChecker().checkArg(call->args[i], params[i]);
    }
    call->type = result;
    sema_.file_->usesAsync = true;
    sema_.flowChecker().calledFunction(); // the runtime settles promises, which run code
    return call;
  }
  default:
    return call;
  }
}

const Type *StreamChecker::streamCallbackType() {
  return Type::function({Type::optional(sema_.errorClass_->type)}, 0, false, Type::voidTy());
}

Expr *StreamChecker::streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing,
                           llvm::StringRef method, bool checked) {
  // undefined ends the stream, as null does in node.
  if (eof && isa<UndefinedExpr>(arg)) {
    return sema_.typeRelations().convert(checked ? arg : sema_.exprChecker().checkExpr(arg), Type::unknown());
  }
  if (!isByteChunk(chunk)) {
    // An object-mode stream takes its chunk type.
    const Type *want = eof ? Type::optional(chunk) : chunk;
    arg = checked ? sema_.typeRelations().coerce(arg, want) : sema_.argumentChecker().checkArg(arg, want);
    return sema_.typeRelations().convert(arg, Type::unknown());
  }
  if (!checked) {
    arg = sema_.exprChecker().checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  // A string, a Buffer or a Uint8Array, as node takes; a Writable<string>
  // gives its _write() the strings written, and takes only strings.
  bool onlyStrings = writing && chunk->isString();
  auto fits = [&](const Type *t) {
    if (t->isLiteral()) {
      t = t->literalBase();
    }
    if (t->isString()) {
      return true;
    }

    return !onlyStrings && (t->isBuffer() || t->bytes() == Type::Bytes::Uint8);
  };
  const Type *value = eof ? type->nonOptional() : type;
  bool ok = value->isUnion() ? llvm::all_of(value->members(), fits) : fits(value);
  if (!ok) {
    std::string takes = onlyStrings ? "a string" : "a string, a Buffer or a Uint8Array";
    sema_.diags_.error(arg->loc(), "'" + method.str() + "' of a stream of " + chunk->name().str() +
                                 " takes " + takes + ", but this is '" + type->name().str() + "'" +
                                 (onlyStrings ? "; a Writable<string> gives its _write() the "
                                                "strings written; make it a Writable<Buffer> to "
                                                "write bytes"
                                              : "; to write objects, make the stream an "
                                                "object-mode one of their type, e.g. 'new "
                                                "Readable<User>({ objectMode: true, ... })'"));
    return arg;
  }

  return sema_.typeRelations().convert(arg, Type::unknown());
}

Expr *StreamChecker::checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  llvm::StringRef name = member->name;
  if (name != "push" && name != "unshift" && name != "write" && name != "end" &&
      name != "setEncoding" && name != "pipe") {
    return nullptr;
  }
  if (sema_.libraryCalls().inLibrary(call->loc()) || !sema_.chunkTypes().isStreamClass(cls)) {
    return nullptr;
  }
  // A class down the chain may declare a method of that name itself.
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, /*isStatic=*/false);
  if (!methods.empty() && !methods[0]->cls->library) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  if (sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = sema_.exprChecker().checkExpr(call->args[i]);
    }
  };
  if (name == "setEncoding") {
    return sema_.setEncodingChecker().checkSetEncoding(call, member, cls);
  }
  if (name == "pipe") {
    return sema_.setEncodingChecker().checkStreamPipe(call, member, cls);
  }
  Sema::StreamChunks chunks = sema_.chunkTypes().streamChunksOf(cls);
  bool reading = name == "push" || name == "unshift";
  const Type *chunk = reading ? chunks.readable : chunks.writable;
  if (!chunk) {
    sema_.diags_.error(member->loc(), "'" + receiver->name() + "' has no " + name.str() + "(): " +
                                    (reading ? "it is not readable" : "it is not writable"));
    checkArgsLoosely(0);
    return call;
  }
  llvm::SMLoc loc = call->loc();
  auto undefinedOf = [&](const Type *type) {
    return sema_.typeRelations().convert(sema_.exprChecker().checkExpr(sema_.ctx_.make<UndefinedExpr>(loc)), type);
  };
  const Type *callback = streamCallbackType();
  const Type *encodingType = Type::optional(Type::string());
  // The second argument of write() and end(): a callback, or an encoding;
  // to tell, the argument is checked, unless it is a function written there.
  llvm::SmallPtrSet<const Expr *, 4> checked;
  auto isCallback = [&](Expr *&arg) {
    if (isa<FunctionExpr>(arg)) {
      return true;
    }
    arg = sema_.exprChecker().checkExpr(arg);
    checked.insert(arg);

    return arg->type->nonOptional()->isFunction();
  };
  auto checkedAs = [&](Expr *arg, const Type *type) {
    return checked.contains(arg) ? sema_.typeRelations().coerce(arg, type) : sema_.argumentChecker().checkArg(arg, type);
  };
  std::vector<Expr *> args;
  llvm::StringRef impl;
  // A round that looks for the chunk type of an object-mode side.
  if (sema_.openMaps_.count(chunk) && !call->args.empty()) {
    size_t at = name == "end" && call->args.size() == 1 && isa<FunctionExpr>(call->args[0])
                    ? call->args.size()
                    : 0;
    for (size_t i = 0; i < call->args.size(); ++i) {
      call->args[i] = sema_.exprChecker().checkExpr(call->args[i]);
      if (i == at && !isa<UndefinedExpr>(call->args[i])) {
        sema_.chunkTypes().recordStreamChunk(chunk, call->args[i]->type, call->args[i]->loc());
      }
    }
    call->type = name == "end" ? receiver : Type::boolTy();
    return call;
  }
  if (reading) {
    // push(chunk, encoding?) and unshift(chunk, encoding?).
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 2)) {
      return call;
    }
    args.push_back(
        streamChunkArg(call->args[0], chunk, /*eof=*/true, /*writing=*/false, name, false));
    args.push_back(call->args.size() > 1
                       ? sema_.typeRelations().coerce(sema_.exprChecker().checkExpr(call->args[1], encodingType), encodingType)
                       : undefinedOf(encodingType));
    impl = name == "push" ? "_pushChunk" : "_unshiftChunk";
  } else if (name == "write") {
    // write(chunk), write(chunk, cb), write(chunk, encoding), write(chunk,
    // encoding, cb).
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 1, 3)) {
      return call;
    }
    args.push_back(
        streamChunkArg(call->args[0], chunk, /*eof=*/false, /*writing=*/true, name, false));
    Expr *encoding = nullptr;
    Expr *cb = nullptr;
    if (call->args.size() == 2) {
      if (isCallback(call->args[1])) {
        cb = call->args[1];
      } else {
        encoding = call->args[1];
      }
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
    if (!sema_.argumentChecker().checkArity(call, member->loc(), 0, 3)) {
      return call;
    }
    Expr *chunkArg = nullptr;
    Expr *encoding = nullptr;
    Expr *cb = nullptr;
    size_t count = call->args.size();
    if (count >= 1 && isCallback(call->args[0])) {
      cb = call->args[0];
      if (count > 1) {
        sema_.diags_.error(call->args[1]->loc(), "end(callback) takes nothing after the callback");
        checkArgsLoosely(1);
        return call;
      }
    } else if (count >= 1) {
      chunkArg = call->args[0];
      if (count == 2) {
        if (isCallback(call->args[1])) {
          cb = call->args[1];
        } else {
          encoding = call->args[1];
        }
      } else if (count == 3) {
        encoding = call->args[1];
        cb = call->args[2];
      }
    }
    args.push_back(sema_.exprChecker().checkExpr(sema_.ctx_.make<BoolLitExpr>(loc, chunkArg != nullptr)));
    args.push_back(chunkArg ? streamChunkArg(chunkArg, chunk, /*eof=*/false, /*writing=*/true, name,
                                             checked.contains(chunkArg))
                            : undefinedOf(Type::unknown()));
    args.push_back(encoding ? checkedAs(encoding, encodingType) : undefinedOf(encodingType));
    args.push_back(cb ? checkedAs(cb, Type::optional(callback))
                      : undefinedOf(Type::optional(callback)));
    impl = "_endWith";
  }
  for (Expr *arg : args) {
    if (arg->type->isError()) {
      return call;
    }
  }
  call->args = std::move(args);
  member->name = impl;
  std::vector<bool> given(call->args.size(), true);
  sema_.emitterChecker().callLibraryMethod(call, member, cls, impl, {}, &given, false);
  if (impl == "_endWith" && !call->type->isError()) {
    call->type = receiver; // `this`
  }

  return call;
}

} // namespace tz
