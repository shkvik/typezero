#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StreamChecker::StreamChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->fsChecker = &all.fsChecker;
  this->chunkTypes = &all.chunkTypes;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->resultChecker = &all.resultChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->emitterChecker = &all.emitterChecker;
  this->argumentChecker = &all.argumentChecker;
  this->setEncodingChecker = &all.setEncodingChecker;
}

bool StreamChecker::isByteChunk(const Type *type) const {
  return type->isBuffer() || type->isString();
}

Expr *StreamChecker::checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg) {
  Builtin builtin = call->builtin();
  if (builtin == Builtin::LibChunk || builtin == Builtin::LibIsChunk) {
    return this->checkChunkIntrinsic(call, typeArg);
  }
  if (builtin == Builtin::LibChunkKind) {
    unsigned kind = this->chunkKind(typeArg);
    auto *literal = this->state.getAst().make<IntLitExpr>(call->loc(), uint64_t(kind));
    return this->exprChecker->checkExpr(literal, Type::intTy());
  }
  if (builtin == Builtin::LibTypeName) {
    return this->typeNameLiteral(call, typeArg);
  }
  if (builtin == Builtin::LibOverrides) {
    return this->checkOverridesIntrinsic(call, name);
  }
  if (builtin == Builtin::LibStdinFailed) {
    call->type = Type::boolTy();
    return call;
  }
  if (this->isNativeIo(builtin)) {
    return this->checkNativeIoIntrinsic(call, typeArg);
  }

  return call;
}

Expr *StreamChecker::checkChunkIntrinsic(CallExpr *call, const Type *typeArg) {
  bool isChunk = call->builtin() == Builtin::LibChunk;
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::unknown());
  call->type = isChunk ? typeArg : Type::boolTy();
  if (!isChunk) {
    Expr *witness = this->state.getAst().make<UndefinedExpr>(call->loc());
    witness->type = typeArg;
    call->args.push_back(witness);
  }

  return call;
}

Expr *StreamChecker::typeNameLiteral(CallExpr *call, const Type *typeArg) {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(typeArg->name(), units);
  std::u16string text(units.begin(), units.end());
  auto *literal = this->state.getAst().make<StringLitExpr>(call->loc(), text);

  return this->exprChecker->checkExpr(literal, Type::string());
}

Expr *StreamChecker::checkOverridesIntrinsic(CallExpr *call, NameExpr *name) {
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  call->type = Type::boolTy();
  auto *classDecl = this->classDeclarer->classOf(call->args[0]->type);
  auto *literal = dyn_cast<StringLitExpr>(call->args[1]);
  auto args = this->checkOverridesArgs(classDecl, literal);
  if (args.isErr()) {
    this->state.getDiagnostics().error(name->loc(), args.error().message);
    return call;
  }
  auto method = ToUtf8::toUtf8(literal->value);
  auto methods = this->classDeclarer->methodsNamed(classDecl, method);
  auto there = this->checkOverriddenMethod(classDecl, methods, method);
  if (there.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), there.error().message);
    return call;
  }
  call->args[1] = this->exprChecker->checkExpr(call->args[1]);
  call->target = BuiltinCall{.builtin = Builtin::LibOverrides, .method = methods[0]->sym};

  return call;
}

bool StreamChecker::isNativeIo(Builtin builtin) const {
  return builtin == Builtin::LibFsOpen || builtin == Builtin::LibFsRead ||
         builtin == Builtin::LibFsWrite || builtin == Builtin::LibFsClose ||
         builtin == Builtin::LibStdinChunk || builtin == Builtin::LibStdWrite;
}

Expr *StreamChecker::checkNativeIoIntrinsic(CallExpr *call, const Type *typeArg) {
  auto signature = this->nativeIoSignature(call->builtin(), typeArg);
  for (size_t index = 0; index < signature.params.size(); ++index) {
    call->args[index] = this->argumentChecker->checkArg(call->args[index], signature.params[index]);
  }
  call->type = signature.result;
  this->state.getInventoryStore().markUsesAsync();
  this->flowChecker->calledFunction();

  return call;
}

StreamChecker::NativeIoSignature StreamChecker::nativeIoSignature(Builtin builtin,
                                                                  const Type *typeArg) {
  const auto *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *failure = typeArg != nullptr ? typeArg : this->fsChecker->nativeError();
  const auto *counted = Type::promise(Type::result(Type::intTy(), failure));
  if (builtin == Builtin::LibFsOpen) {
    return NativeIoSignature{
        .params = {Type::string(), Type::string(), Type::intTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibFsRead || builtin == Builtin::LibFsWrite) {
    return NativeIoSignature{
        .params = {Type::intTy(), buffer, Type::intTy(), Type::intTy(), Type::doubleTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibFsClose) {
    return NativeIoSignature{
        .params = {Type::intTy(), Type::boolTy()},
        .result = counted,
    };
  }
  if (builtin == Builtin::LibStdinChunk) {
    return NativeIoSignature{
        .params = {Type::intTy()},
        .result = Type::promise(Type::optional(buffer)),
    };
  }

  return NativeIoSignature{
      .params = {Type::intTy(), buffer},
      .result = Type::intTy(),
  };
}

StreamChecker::ResultOverrideArgs
StreamChecker::checkOverridesArgs(const ClassDecl *classDecl, const StringLitExpr *literal) const {
  if (classDecl == nullptr || literal == nullptr) {
    auto message = ErrorTextStreamCall::overrides();

    return err(StreamCheckerOverridesArgsError{
        .code = StreamCheckerOverridesArgsErrorCode::NOT_CLASS_AND_LITERAL,
        .message = message,
    });
  }

  return ok(classDecl);
}

StreamChecker::ResultMethodExists
StreamChecker::checkOverriddenMethod(const ClassDecl *classDecl,
                                     llvm::ArrayRef<FunctionDecl *> methods,
                                     llvm::StringRef method) const {
  if (methods.empty() || methods[0]->sym == nullptr) {
    auto message = ErrorTextStreamCall::noMethod(classDecl->name, method);

    return err(StreamCheckerOverriddenMethodError{
        .code = StreamCheckerOverriddenMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(methods[0]);
}

StreamChecker::ResultChunkIsGiven
StreamChecker::checkChunkNotUndefined(const Expr *arg, ChunkSide side,
                                      llvm::StringRef method) const {
  if (side == ChunkSide::PUSHED && isa<UndefinedExpr>(arg)) {
    auto message = ErrorTextStreamCall::undefined(method);

    return err(StreamCheckerChunkNotUndefinedError{
        .code = StreamCheckerChunkNotUndefinedErrorCode::UNDEFINED_CHUNK,
        .message = message,
    });
  }

  return ok(arg);
}

StreamChecker::ResultStreamSideOk StreamChecker::checkStreamSide(const Type *chunk,
                                                                 const Type *receiver,
                                                                 llvm::StringRef name,
                                                                 bool reading) const {
  if (chunk == nullptr && reading) {
    auto message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(StreamCheckerStreamSideError{
        .code = StreamCheckerStreamSideErrorCode::NOT_READABLE,
        .message = message,
    });
  }
  if (chunk == nullptr) {
    auto message = ErrorTextStreamCall::noSide(receiver->name(), name, reading);

    return err(StreamCheckerStreamSideError{
        .code = StreamCheckerStreamSideErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(chunk);
}

const Type *StreamChecker::streamCallbackType() {
  return Type::function({Type::optional(this->prelude->builtinType(PreludeType::STREAM_ERROR))}, 0,
                        false, Type::voidTy());
}

Expr *StreamChecker::streamChunkArg(Expr *arg, const Type *chunk, ChunkSide side,
                                    llvm::StringRef method, ChunkState chunkState) {
  bool isChecked = chunkState == ChunkState::CHECKED;
  auto given = this->checkChunkNotUndefined(arg, side, method);
  if (given.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), given.error().message);
    arg->type = Type::error();

    return arg;
  }
  if (!this->isByteChunk(chunk)) {
    const Type *want = chunk;
    arg = isChecked ? this->typeRelations->coerce(arg, want)
                    : this->argumentChecker->checkArg(arg, want);
    return this->typeRelations->convert(arg, Type::unknown());
  }
  if (!isChecked) {
    arg = this->exprChecker->checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  bool isOnlyStrings = side == ChunkSide::WRITTEN && chunk->isString();
  auto fits = std::bind_front(&StreamChecker::isChunkValue, this, isOnlyStrings);
  bool isOk = type->isUnion() ? llvm::all_of(type->members(), fits) : fits(type);
  if (!isOk) {
    auto message = ErrorTextStreamCall::chunk(method, chunk->name(), isOnlyStrings, type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }

  return this->typeRelations->convert(arg, Type::unknown());
}

bool StreamChecker::isChunkValue(bool isOnlyStrings, const Type *candidate) const {
  const Type *base = candidate->isLiteral() ? candidate->literalBase() : candidate;
  if (base->isString()) {
    return true;
  }

  return !isOnlyStrings && (base->isBuffer() || base->bytes() == Type::Bytes::Uint8);
}

unsigned StreamChecker::chunkKind(const Type *typeArg) const {
  if (typeArg->isBuffer()) {
    return 0;
  }
  if (typeArg->isString()) {
    return 1;
  }

  return 2;
}

Expr *StreamChecker::checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl) {
  auto method = StreamMethodTable::find(member->name);
  if (!method.has_value() || !this->chunkTypes->isStreamClass(classDecl)) {
    return nullptr;
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, member->name);
  if (!methods.empty() && !methods[0]->cls()->library()) {
    return nullptr;
  }
  call->type = Type::error();
  member->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  switch (*method) {
    case StreamMethod::SET_ENCODING:
      return this->setEncodingChecker->checkSetEncoding(call, member, classDecl);
    case StreamMethod::PIPE:
      return this->reportPipe(call, member);
    case StreamMethod::PUSH:
    case StreamMethod::UNSHIFT:
    case StreamMethod::WRITE:
    case StreamMethod::END:
      return this->checkStreamData(call, member, classDecl, *method);
  }

  return call;
}

Expr *StreamChecker::reportPipe(CallExpr *call, MemberExpr *member) {
  auto message = ErrorTextStreamCall::pipe();
  this->state.getDiagnostics().error(member->loc(), message);

  return this->resultChecker->markCallFailed(call);
}

Expr *StreamChecker::checkStreamData(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                                     StreamMethod method) {
  auto chunks = this->chunkTypes->streamChunksOf(classDecl);
  bool isReading = method == StreamMethod::PUSH || method == StreamMethod::UNSHIFT;
  const Type *chunk = isReading ? chunks.readable : chunks.writable;
  const Type *receiver = member->object->type;
  auto side = this->checkStreamSide(chunk, receiver, member->name, isReading);
  if (side.isErr()) {
    this->state.getDiagnostics().error(member->loc(), side.error().message);
    this->walkArgsFrom(call, 0);
    return call;
  }

  StreamCall stream{
      .call = call,
      .member = member,
      .cls = classDecl,
      .method = method,
      .chunk = chunk,
  };
  if (isReading) {
    return this->checkStreamRead(stream);
  }
  if (method == StreamMethod::WRITE) {
    return this->checkStreamWrite(stream);
  }

  return this->checkStreamEnd(stream);
}

void StreamChecker::walkArgsFrom(CallExpr *call, size_t from) {
  for (size_t index = from; index < call->args.size(); ++index) {
    call->args[index] = this->exprChecker->checkExpr(call->args[index]);
  }
}

Expr *StreamChecker::undefinedOf(llvm::SMLoc loc, const Type *type) {
  auto *none = this->exprChecker->checkExpr(this->state.getAst().make<UndefinedExpr>(loc));

  return this->typeRelations->convert(none, type);
}

bool StreamChecker::isCallbackArg(StreamCall &stream, Expr *&arg) {
  if (isa<FunctionExpr>(arg)) {
    return true;
  }
  if (isa<StringLitExpr>(arg)) {
    return false;
  }
  arg = this->exprChecker->checkExpr(arg);
  stream.checked.insert(arg);

  return arg->type->nonOptional()->isFunction();
}

Expr *StreamChecker::checkedAs(const StreamCall &stream, Expr *arg, const Type *type) {
  if (stream.checked.contains(arg)) {
    return this->typeRelations->coerce(arg, type);
  }

  return this->argumentChecker->checkArg(arg, type);
}

Expr *StreamChecker::encodingArg(const StreamCall &stream, Expr *arg) {
  const auto *bufferEncoding =
      Type::optional(this->prelude->builtinType(PreludeType::BUFFER_ENCODING));
  auto *encoding = this->checkedAs(stream, arg, bufferEncoding);
  if (encoding->type->isError()) {
    return encoding;
  }

  return this->typeRelations->convert(encoding, Type::optional(Type::string()));
}

Expr *StreamChecker::checkStreamRead(StreamCall &stream) {
  CallExpr *call = stream.call;
  MemberExpr *member = stream.member;
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto args = std::vector<Expr *>();
  auto *pushed = this->streamChunkArg(call->args[0], stream.chunk, ChunkSide::PUSHED, member->name,
                                      ChunkState::UNCHECKED);
  args.push_back(pushed);
  const auto *encodingType = Type::optional(Type::string());
  args.push_back(call->args.size() > 1 ? this->encodingArg(stream, call->args[1])
                                       : this->undefinedOf(call->loc(), encodingType));
  llvm::StringRef implementation =
      stream.method == StreamMethod::PUSH ? "pushTyped" : "unshiftTyped";

  return this->finishStreamCall(stream, std::move(args), implementation);
}

Expr *StreamChecker::checkStreamWrite(StreamCall &stream) {
  CallExpr *call = stream.call;
  MemberExpr *member = stream.member;
  auto arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto args = std::vector<Expr *>();
  auto *chunk = this->streamChunkArg(call->args[0], stream.chunk, ChunkSide::WRITTEN, member->name,
                                     ChunkState::UNCHECKED);
  args.push_back(chunk);
  Expr *encoding = nullptr;
  Expr *callback = nullptr;
  if (call->args.size() == 2) {
    if (this->isCallbackArg(stream, call->args[1])) {
      callback = call->args[1];
    } else {
      encoding = call->args[1];
    }
  } else if (call->args.size() == 3) {
    encoding = call->args[1];
    callback = call->args[2];
  }
  const auto *encodingType = Type::optional(Type::string());
  args.push_back(encoding != nullptr ? this->encodingArg(stream, encoding)
                                     : this->undefinedOf(call->loc(), encodingType));
  const auto *written = this->writtenCallbackType(stream.cls);
  args.push_back(callback != nullptr ? this->checkedAs(stream, callback, written)
                                     : this->undefinedOf(call->loc(), written));

  return this->finishStreamCall(stream, std::move(args), "writeTyped");
}

const Type *StreamChecker::writtenCallbackType(const ClassDecl *classDecl) {
  for (FunctionDecl *typed : this->classDeclarer->methodsNamed(classDecl, "writeTyped")) {
    if (typed->sym != nullptr && typed->params.size() == 3) {
      return typed->sym->params[2];
    }
  }

  return Type::optional(this->streamCallbackType());
}

Expr *StreamChecker::checkStreamEnd(StreamCall &stream) {
  CallExpr *call = stream.call;
  MemberExpr *member = stream.member;
  auto arity = this->argumentChecker->checkArity(call, 0, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  size_t count = call->args.size();
  for (size_t index = 0; index < count; ++index) {
    if (this->isCallbackArg(stream, call->args[index])) {
      auto message = ErrorTextStreamCall::endWithFn();
      this->state.getDiagnostics().error(call->args[index]->loc(), message);
      return this->resultChecker->markCallFailed(call);
    }
  }
  if (count == 3) {
    auto message = ErrorTextStreamCall::endArgs();
    this->state.getDiagnostics().error(call->args[2]->loc(), message);
    this->walkArgsFrom(call, 2);
    return call;
  }
  Expr *chunkArg = count >= 1 ? call->args[0] : nullptr;
  Expr *encoding = count >= 2 ? call->args[1] : nullptr;
  llvm::SMLoc loc = call->loc();
  auto args = std::vector<Expr *>();
  auto *given = this->state.getAst().make<BoolLitExpr>(loc, chunkArg != nullptr);
  args.push_back(this->exprChecker->checkExpr(given));
  if (chunkArg != nullptr) {
    bool isChecked = stream.checked.contains(chunkArg);
    auto chunkState = isChecked ? ChunkState::CHECKED : ChunkState::UNCHECKED;
    auto *chunk =
        this->streamChunkArg(chunkArg, stream.chunk, ChunkSide::WRITTEN, member->name, chunkState);
    args.push_back(chunk);
  } else {
    args.push_back(this->undefinedOf(loc, Type::unknown()));
  }
  const auto *encodingType = Type::optional(Type::string());
  args.push_back(encoding != nullptr ? this->encodingArg(stream, encoding)
                                     : this->undefinedOf(loc, encodingType));

  return this->finishStreamCall(stream, std::move(args), "endTyped");
}

Expr *StreamChecker::finishStreamCall(StreamCall &stream, std::vector<Expr *> args,
                                      llvm::StringRef implementation) {
  CallExpr *call = stream.call;
  for (Expr *arg : args) {
    if (arg->type->isError()) {
      return call;
    }
  }
  call->args = std::move(args);
  stream.member->name = implementation;
  std::vector<bool> given(call->args.size(), true);
  this->emitterChecker->callLibraryMethod(call, stream.member, stream.cls, implementation, {},
                                          &given, CallReceiver::OWN);

  return call;
}

}
