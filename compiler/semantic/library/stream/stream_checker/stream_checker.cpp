#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool StreamChecker::isByteChunk(const Type *type) {
  return type->isBuffer() || type->isString();
}

StreamChecker::StreamChecker(SemanticState &owner)
    : state(owner) {}

Expr *StreamChecker::checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg) {
  switch (call->builtin()) {
    case Builtin::LibChunk:
    case Builtin::LibIsChunk:
      call->args[0] = ArgumentChecker(this->state).checkArg(call->args[0], Type::unknown());
      call->type = call->builtin() == Builtin::LibChunk ? typeArg : Type::boolTy();
      if (call->builtin() == Builtin::LibIsChunk) {
        Expr *witness = this->state.getAst().make<UndefinedExpr>(call->loc());
        witness->type = typeArg;
        call->args.push_back(witness);
      }
      return call;
    case Builtin::LibChunkKind: {
      unsigned kind = typeArg->isBuffer() ? 0 : typeArg->isString() ? 1 : 2;
      return ExprChecker(this->state)
          .checkExpr(this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(kind)),
                     Type::intTy());
    }
    case Builtin::LibTypeName: {
      llvm::SmallVector<llvm::UTF16, 32> units;
      llvm::convertUTF8ToUTF16String(typeArg->name(), units);
      return ExprChecker(this->state)
          .checkExpr(this->state.getAst().make<StringLitExpr>(
                         call->loc(), std::u16string(units.begin(), units.end())),
                     Type::string());
    }
    case Builtin::LibOverrides: {
      call->args[0] = ExprChecker(this->state).checkExpr(call->args[0]);
      call->type = Type::boolTy();
      ClassDecl *cls = ClassDeclarer::classOf(call->args[0]->type);
      auto *lit = dyn_cast<StringLitExpr>(call->args[1]);
      ResultOverrideArgs args = StreamChecker::overridesArgs(cls, lit);
      if (args.isErr()) {
        this->state.getDiagnostics().error(name->loc(), args.error().message);
        return call;
      }
      std::string method = ToUtf8::toUtf8(lit->value);
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, method, false);
      ResultMethodExists there = StreamChecker::overriddenMethod(cls, methods, method);
      if (there.isErr()) {
        this->state.getDiagnostics().error(lit->loc(), there.error().message);
        return call;
      }
      call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
      call->target = BuiltinCall{.builtin = Builtin::LibOverrides, .method = methods[0]->sym};
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
          Type::result(Type::intTy(), typeArg ? typeArg : FsChecker(this->state).nativeError()));
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
        call->args[i] = ArgumentChecker(this->state).checkArg(call->args[i], params[i]);
      }
      call->type = result;
      this->state.getInventoryStore().markUsesAsync();
      FlowChecker(this->state).calledFunction();
      return call;
    }
    default:
      return call;
  }
}

StreamChecker::ResultOverrideArgs StreamChecker::overridesArgs(const ClassDecl *cls,
                                                               const StringLitExpr *lit) {
  if (!cls || !lit) {
    std::string message = ErrorTextStreamCall::overrides();

    return err(StreamCheckerOverridesArgsError{
        .code = StreamCheckerOverridesArgsErrorCode::NOT_CLASS_AND_LITERAL,
        .message = message,
    });
  }

  return ok(cls);
}

StreamChecker::ResultMethodExists
StreamChecker::overriddenMethod(const ClassDecl *cls, llvm::ArrayRef<FunctionDecl *> methods,
                                llvm::StringRef method) {
  if (methods.empty() || !methods[0]->sym) {
    std::string message = ErrorTextStreamCall::noMethod(cls->name, method);

    return err(StreamCheckerOverriddenMethodError{
        .code = StreamCheckerOverriddenMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods[0]);
}

StreamChecker::ResultChunkIsGiven StreamChecker::chunkNotUndefined(const Expr *arg, bool eof,
                                                                   llvm::StringRef method) {
  if (eof && isa<UndefinedExpr>(arg)) {
    std::string message = ErrorTextStreamCall::undefined(method);

    return err(StreamCheckerChunkNotUndefinedError{
        .code = StreamCheckerChunkNotUndefinedErrorCode::UNDEFINED_CHUNK,
        .message = message,
    });
  }

  return ok(arg);
}

StreamChecker::ResultStreamSideOk StreamChecker::streamSide(const Type *chunk, const Type *receiver,
                                                            llvm::StringRef name, bool reading) {
  if (!chunk && reading) {
    std::string message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(StreamCheckerStreamSideError{
        .code = StreamCheckerStreamSideErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (!chunk) {
    std::string message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(StreamCheckerStreamSideError{
        .code = StreamCheckerStreamSideErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(chunk);
}

const Type *StreamChecker::streamCallbackType() {
  return Type::function({Type::optional(Prelude(this->state).builtinType("StreamError"))}, 0, false,
                        Type::voidTy());
}

Expr *StreamChecker::streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing,
                                    llvm::StringRef method, bool checked) {
  ResultChunkIsGiven given = StreamChecker::chunkNotUndefined(arg, eof, method);
  if (given.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), given.error().message);
    arg->type = Type::error();

    return arg;
  }
  if (!StreamChecker::isByteChunk(chunk)) {
    const Type *want = chunk;
    arg = checked ? TypeRelations(this->state).coerce(arg, want)
                  : ArgumentChecker(this->state).checkArg(arg, want);
    return TypeRelations(this->state).convert(arg, Type::unknown());
  }
  if (!checked) {
    arg = ExprChecker(this->state).checkExpr(arg);
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
    std::string message =
        ErrorTextStreamCall::chunk(method, chunk->name(), onlyStrings, type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }

  return TypeRelations(this->state).convert(arg, Type::unknown());
}

Expr *StreamChecker::checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  llvm::StringRef name = member->name;
  if (name != "push" && name != "unshift" && name != "write" && name != "end" &&
      name != "setEncoding" && name != "pipe") {
    return nullptr;
  }
  if (!ChunkTypes(this->state).isStreamClass(cls)) {
    return nullptr;
  }
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, false);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return nullptr;
  }
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
    }
  };
  if (name == "setEncoding") {
    return SetEncodingChecker(this->state).checkSetEncoding(call, member, cls);
  }
  if (name == "pipe") {
    std::string message = ErrorTextStreamCall::pipe();
    this->state.getDiagnostics().error(member->loc(), message);

    return ResultChecker(this->state).markCallFailed(call);
  }
  StreamChunks chunks = ChunkTypes(this->state).streamChunksOf(cls);
  bool reading = name == "push" || name == "unshift";
  const Type *chunk = reading ? chunks.readable : chunks.writable;
  ResultStreamSideOk side = StreamChecker::streamSide(chunk, receiver, name, reading);
  if (side.isErr()) {
    this->state.getDiagnostics().error(member->loc(), side.error().message);
    checkArgsLoosely(0);
    return call;
  }
  llvm::SMLoc loc = call->loc();
  auto undefinedOf = [&](const Type *type) {
    return TypeRelations(this->state)
        .convert(ExprChecker(this->state).checkExpr(this->state.getAst().make<UndefinedExpr>(loc)),
                 type);
  };
  const Type *callback = this->streamCallbackType();
  const Type *encodingType = Type::optional(Type::string());
  const Type *bufferEncoding = Type::optional(Prelude(this->state).builtinType("BufferEncoding"));
  llvm::SmallPtrSet<const Expr *, 4> checked;
  auto isCallback = [&](Expr *&arg) {
    if (isa<FunctionExpr>(arg)) {
      return true;
    }
    if (isa<StringLitExpr>(arg)) {
      return false;
    }
    arg = ExprChecker(this->state).checkExpr(arg);
    checked.insert(arg);

    return arg->type->nonOptional()->isFunction();
  };
  auto checkedAs = [&](Expr *arg, const Type *type) {
    return checked.contains(arg) ? TypeRelations(this->state).coerce(arg, type)
                                 : ArgumentChecker(this->state).checkArg(arg, type);
  };
  auto encodingArg = [&](Expr *arg) {
    Expr *e = checkedAs(arg, bufferEncoding);
    return e->type->isError() ? e : TypeRelations(this->state).convert(e, encodingType);
  };
  std::vector<Expr *> args;
  llvm::StringRef impl;
  if (this->state.getEventStore().isOpenMap(chunk) && !call->args.empty()) {
    size_t at = name == "end" && call->args.size() == 1 && isa<FunctionExpr>(call->args[0])
                    ? call->args.size()
                    : 0;
    for (size_t i = 0; i < call->args.size(); ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
      if (i == at && !isa<UndefinedExpr>(call->args[i])) {
        ChunkTypes(this->state).recordStreamChunk(chunk, call->args[i]->type, call->args[i]->loc());
      }
    }
    call->type = name == "end"
                     ? Type::promise(Type::result(Type::voidTy(),
                                                  Prelude(this->state).builtinType("StreamError")))
                     : Type::boolTy();
    return call;
  }
  if (reading) {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    args.push_back(this->streamChunkArg(call->args[0], chunk, true, false, name, false));
    args.push_back(call->args.size() > 1 ? encodingArg(call->args[1]) : undefinedOf(encodingType));
    impl = name == "push" ? "pushTyped" : "unshiftTyped";
  } else if (name == "write") {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
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
        ReturnInferrer(this->state).resolveParams(typed->sym);
        written = typed->sym->params[2];
        break;
      }
    }
    args.push_back(cb ? checkedAs(cb, written) : undefinedOf(written));
    impl = "writeTyped";
  } else {
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 0, 3);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *chunkArg = nullptr;
    Expr *encoding = nullptr;
    size_t count = call->args.size();
    for (size_t i = 0; i < count; ++i) {
      if (isCallback(call->args[i])) {
        std::string message = ErrorTextStreamCall::endWithFn();
        this->state.getDiagnostics().error(call->args[i]->loc(), message);

        return ResultChecker(this->state).markCallFailed(call);
      }
    }
    if (count >= 1) {
      chunkArg = call->args[0];
      if (count >= 2) {
        encoding = call->args[1];
      }
      if (count == 3) {
        std::string message = ErrorTextStreamCall::endArgs();
        this->state.getDiagnostics().error(call->args[2]->loc(), message);
        checkArgsLoosely(2);
        return call;
      }
    }
    args.push_back(
        ExprChecker(this->state)
            .checkExpr(this->state.getAst().make<BoolLitExpr>(loc, chunkArg != nullptr)));
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
  EmitterChecker(this->state).callLibraryMethod(call, member, cls, impl, {}, &given, false);

  return call;
}

}
