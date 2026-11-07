#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string StreamChecker::utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

bool StreamChecker::isByteChunk(const Type *type) {
  return type->isBuffer() || type->isString();
}

StreamChecker::StreamChecker(Checker &owner)
    : checker(owner) {}

Expr *StreamChecker::checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg) {
  switch (call->builtin()) {
    case Builtin::LibChunk:
    case Builtin::LibIsChunk:
      call->args[0] = this->checker.argumentChecker().checkArg(call->args[0], Type::unknown());
      call->type = call->builtin() == Builtin::LibChunk ? typeArg : Type::boolTy();
      if (call->builtin() == Builtin::LibIsChunk) {
        Expr *witness = this->checker.ctx_.make<UndefinedExpr>(call->loc());
        witness->type = typeArg;
        call->args.push_back(witness);
      }
      return call;
    case Builtin::LibChunkKind: {
      unsigned kind = typeArg->isBuffer() ? 0 : typeArg->isString() ? 1 : 2;
      return this->checker.exprChecker().checkExpr(
          this->checker.ctx_.make<IntLitExpr>(call->loc(), uint64_t(kind)), Type::intTy());
    }
    case Builtin::LibTypeName: {
      llvm::SmallVector<llvm::UTF16, 32> units;
      llvm::convertUTF8ToUTF16String(typeArg->name(), units);
      return this->checker.exprChecker().checkExpr(
          this->checker.ctx_.make<StringLitExpr>(call->loc(),
                                                 std::u16string(units.begin(), units.end())),
          Type::string());
    }
    case Builtin::LibOverrides: {
      call->args[0] = this->checker.exprChecker().checkExpr(call->args[0]);
      call->type = Type::boolTy();
      ClassDecl *cls = ClassDeclarer::classOf(call->args[0]->type);
      auto *lit = dyn_cast<StringLitExpr>(call->args[1]);
      if (!cls || !lit) {
        this->checker.diags_.error(name->loc(),
                                   "'overrides' takes an object of a class and the name of a "
                                   "method, as a string literal");
        return call;
      }
      std::string method = StreamChecker::utf8(lit->value);
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, method, false);
      if (methods.empty() || !methods[0]->sym) {
        this->checker.diags_.error(lit->loc(),
                                   "class '" + cls->name + "' has no method '" + method + "'");

        return call;
      }
      call->args[1] = this->checker.exprChecker().checkExpr(call->args[1]);
      call->target = MethodCall{.method = methods[0]->sym};
      return call;
    }
    case Builtin::LibStdinFailed:
      call->type = Type::boolTy();
      return call;
    case Builtin::LibFsOpen:
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
    case Builtin::LibFsClose:
    case Builtin::LibStdinChunk:
    case Builtin::LibStdWrite: {
      const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
      std::vector<const Type *> params;
      const Type *result = Type::promise(
          Type::result(Type::intTy(), typeArg ? typeArg : this->checker.fsChecker().nativeError()));
      switch (call->builtin()) {
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
          result = Type::intTy();
          break;
      }
      for (size_t i = 0; i < params.size(); ++i) {
        call->args[i] = this->checker.argumentChecker().checkArg(call->args[i], params[i]);
      }
      call->type = result;
      this->checker.file_->usesAsync = true;
      this->checker.flowChecker().calledFunction();
      return call;
    }
    default:
      return call;
  }
}

const Type *StreamChecker::streamCallbackType() {
  return Type::function({Type::optional(this->checker.prelude().builtinType("StreamError"))}, 0,
                        false, Type::voidTy());
}

Expr *StreamChecker::streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing,
                                    llvm::StringRef method, bool checked) {
  if (eof && isa<UndefinedExpr>(arg)) {
    this->checker.diags_.error(arg->loc(), "'" + method.str() +
                                               "(undefined)' does not end a stream: a chunk is "
                                               "never undefined; end the stream with 'pushEnd()'");
    arg->type = Type::error();

    return arg;
  }
  if (!StreamChecker::isByteChunk(chunk)) {
    const Type *want = chunk;
    arg = checked ? this->checker.typeRelations().coerce(arg, want)
                  : this->checker.argumentChecker().checkArg(arg, want);
    return this->checker.typeRelations().convert(arg, Type::unknown());
  }
  if (!checked) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
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
  const Type *value = type;
  bool ok = value->isUnion() ? llvm::all_of(value->members(), fits) : fits(value);
  if (!ok) {
    std::string takes = onlyStrings ? "a string" : "a string, a Buffer or a Uint8Array";
    this->checker.diags_.error(
        arg->loc(), "'" + method.str() + "' of a stream of " + chunk->name().str() + " takes " +
                        takes + ", but this is '" + type->name().str() + "'" +
                        (onlyStrings ? "; a Writable<string> gives its onWrite() the "
                                       "strings written; make it a Writable<Buffer> to "
                                       "write bytes"
                                     : "; to write objects, make the stream an "
                                       "object-mode one of their type, e.g. 'class "
                                       "Users extends Readable<User>' with "
                                       "'super({ objectMode: true })'"));
    return arg;
  }

  return this->checker.typeRelations().convert(arg, Type::unknown());
}

Expr *StreamChecker::checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  llvm::StringRef name = member->name;
  if (name != "push" && name != "unshift" && name != "write" && name != "end" &&
      name != "setEncoding" && name != "pipe") {
    return nullptr;
  }
  if (!this->checker.chunkTypes().isStreamClass(cls)) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = this->checker.exprChecker().checkExpr(call->args[i]);
    }
  };
  if (name == "setEncoding") {
    return this->checker.setEncodingChecker().checkSetEncoding(call, member, cls);
  }
  if (name == "pipe") {
    return this->checker.resultChecker().reject(
        call, member->loc(),
        "'pipe' is not supported: it does not pass an error on, so the source stops and the "
        "destination stays open; join the streams with 'await pipeline(source, destination)', "
        "which gives a Result");
  }
  Checker::StreamChunks chunks = this->checker.chunkTypes().streamChunksOf(cls);
  bool reading = name == "push" || name == "unshift";
  const Type *chunk = reading ? chunks.readable : chunks.writable;
  if (!chunk) {
    this->checker.diags_.error(
        member->loc(), "'" + receiver->name() + "' has no " + name.str() +
                           "(): " + (reading ? "it is not readable" : "it is not writable"));
    checkArgsLoosely(0);
    return call;
  }
  llvm::SMLoc loc = call->loc();
  auto undefinedOf = [&](const Type *type) {
    return this->checker.typeRelations().convert(
        this->checker.exprChecker().checkExpr(this->checker.ctx_.make<UndefinedExpr>(loc)), type);
  };
  const Type *callback = this->streamCallbackType();
  const Type *encodingType = Type::optional(Type::string());
  const Type *bufferEncoding =
      Type::optional(this->checker.prelude().builtinType("BufferEncoding"));
  llvm::SmallPtrSet<const Expr *, 4> checked;
  auto isCallback = [&](Expr *&arg) {
    if (isa<FunctionExpr>(arg)) {
      return true;
    }
    if (isa<StringLitExpr>(arg)) {
      return false;
    }
    arg = this->checker.exprChecker().checkExpr(arg);
    checked.insert(arg);

    return arg->type->nonOptional()->isFunction();
  };
  auto checkedAs = [&](Expr *arg, const Type *type) {
    return checked.contains(arg) ? this->checker.typeRelations().coerce(arg, type)
                                 : this->checker.argumentChecker().checkArg(arg, type);
  };
  auto encodingArg = [&](Expr *arg) {
    Expr *e = checkedAs(arg, bufferEncoding);
    return e->type->isError() ? e : this->checker.typeRelations().convert(e, encodingType);
  };
  std::vector<Expr *> args;
  llvm::StringRef impl;
  if (this->checker.openMaps_.count(chunk) && !call->args.empty()) {
    size_t at = name == "end" && call->args.size() == 1 && isa<FunctionExpr>(call->args[0])
                    ? call->args.size()
                    : 0;
    for (size_t i = 0; i < call->args.size(); ++i) {
      call->args[i] = this->checker.exprChecker().checkExpr(call->args[i]);
      if (i == at && !isa<UndefinedExpr>(call->args[i])) {
        this->checker.chunkTypes().recordStreamChunk(chunk, call->args[i]->type,
                                                     call->args[i]->loc());
      }
    }
    call->type = name == "end"
                     ? Type::promise(Type::result(
                           Type::voidTy(), this->checker.prelude().builtinType("StreamError")))
                     : Type::boolTy();
    return call;
  }
  if (reading) {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    args.push_back(this->streamChunkArg(call->args[0], chunk, true, false, name, false));
    args.push_back(call->args.size() > 1 ? encodingArg(call->args[1]) : undefinedOf(encodingType));
    impl = name == "push" ? "pushTyped" : "unshiftTyped";
  } else if (name == "write") {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    args.push_back(this->streamChunkArg(call->args[0], chunk, false, true, name, false));
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
    args.push_back(encoding ? encodingArg(encoding) : undefinedOf(encodingType));
    const Type *written = Type::optional(callback);
    for (FunctionDecl *typed : ClassDeclarer::methodsNamed(cls, "writeTyped", false)) {
      if (typed->sym && typed->params.size() == 3) {
        this->checker.returnInferrer().resolveParams(typed->sym);
        written = typed->sym->params[2];
        break;
      }
    }
    args.push_back(cb ? checkedAs(cb, written) : undefinedOf(written));
    impl = "writeTyped";
  } else {
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->checker.diags_.error(member->loc(), arity.error().message);
      return call;
    }
    Expr *chunkArg = nullptr;
    Expr *encoding = nullptr;
    size_t count = call->args.size();
    for (size_t i = 0; i < count; ++i) {
      if (isCallback(call->args[i])) {
        return this->checker.resultChecker().reject(
            call, call->args[i]->loc(),
            "end() takes no callback: it gives a Promise of a Result, which says when the "
            "stream finished and whether it failed; 'const r = await w.end()'");
      }
    }
    if (count >= 1) {
      chunkArg = call->args[0];
      if (count >= 2) {
        encoding = call->args[1];
      }
      if (count == 3) {
        this->checker.diags_.error(call->args[2]->loc(), "end() takes a chunk and its encoding");
        checkArgsLoosely(2);
        return call;
      }
    }
    args.push_back(this->checker.exprChecker().checkExpr(
        this->checker.ctx_.make<BoolLitExpr>(loc, chunkArg != nullptr)));
    args.push_back(chunkArg ? this->streamChunkArg(chunkArg, chunk, false, true, name,
                                                   checked.contains(chunkArg))
                            : undefinedOf(Type::unknown()));
    args.push_back(encoding ? encodingArg(encoding) : undefinedOf(encodingType));
    impl = "endTyped";
  }
  for (Expr *arg : args) {
    if (arg->type->isError()) {
      return call;
    }
  }
  call->args = std::move(args);
  member->name = impl;
  std::vector<bool> given(call->args.size(), true);
  this->checker.emitterChecker().callLibraryMethod(call, member, cls, impl, {}, &given, false);

  return call;
}

}
