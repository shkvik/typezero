#include "compiler/pipeline/check_program/topics/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/topics/events/event_map_inference/event_map_inference.h"
#include "compiler/pipeline/check_program/topics/events/event_target_checker/event_target_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/functions/closure_checker/closure_checker.h"
#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "compiler/pipeline/check_program/topics/stream/chunk_types/chunk_types.h"
#include "compiler/pipeline/check_program/topics/stream/stream_checker/stream_checker.h"
#include "compiler/pipeline/check_program/topics/stream/stream_sources/stream_sources.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool StreamPipeline::inModule(const std::string &place, llvm::StringRef module) {
  llvm::StringRef at(place);

  return at.consume_front(module) && at.consume_front(":");
}

StreamPipeline::ResultWritableEnd StreamPipeline::writableEnd(const StreamChunks &chunks,
                                                              const Type *type) {
  if (!chunks.writable) {
    std::string message = ErrorTextPipeline::notWritten(type->name());

    return err(StreamPipelineWritableEndError{
        .code = StreamPipelineWritableEndErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(chunks.writable);
}

StreamPipeline::ResultReadableEnd StreamPipeline::readableEnd(const StreamChunks &chunks,
                                                              const Type *type) {
  if (!chunks.readable) {
    std::string message = ErrorTextPipeline::notRead(type->name());

    return err(StreamPipelineReadableEndError{
        .code = StreamPipelineReadableEndErrorCode::NOT_READABLE,
        .message = message,
    });
  }

  return ok(chunks.readable);
}

StreamPipeline::StreamPipeline(CheckState &owner)
    : state(owner) {}

Expr *StreamPipeline::streamStage(Expr *&arg, const Type *&chunk, bool first, bool last,
                                  bool &stream, const Type **sinkValue) {
  llvm::SMLoc loc = arg->loc();
  ClassDecl *stageClass = this->state.getLibraryDeclarations().findLibraryClass("PipelineStage");
  const Type *stageType = stageClass ? stageClass->type : Type::error();
  stream = false;
  auto stageCall = [&](llvm::StringRef fn, std::vector<const Type *> bound, Expr *value) -> Expr * {
    auto *callee = this->state.getAst().make<NameExpr>(loc, fn);
    auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::vector<Expr *>{value});
    call->type = Type::error();
    std::vector<bool> given{true};
    Expr *made = EventTargetChecker(this->state).callLibraryFunction(call, fn, bound, given, loc);

    return made->type->isError() ? made : TypeRelations(this->state).convert(made, stageType);
  };
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    Signature sig;
    if (first) {
      sig.params = {};
      sig.minArgs = 0;
    } else {
      sig.params = {Type::asyncIterator(chunk)};
      sig.minArgs = 1;
    }
    arg = ClosureChecker(this->state).checkFunctionExpr(fe, &sig);
  } else {
    arg = ExprChecker(this->state).checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type);
      cls && ChunkTypes(this->state).isStreamClass(cls) && !type->isOptional()) {
    stream = true;
    StreamChunks chunks = ChunkTypes(this->state).streamChunksOf(cls);
    if (!first) {
      ResultWritableEnd writable = StreamPipeline::writableEnd(chunks, type);
      if (writable.isErr()) {
        this->state.getDiagnostics().error(loc, writable.error().message);
        return arg;
      }
      if (chunk && ChunkTypes(this->state).recordStreamChunk(chunks.writable, chunk, loc)) {
      } else if (chunk && !this->state.getEventStore().isOpenMap(chunk) &&
                 !this->streamTakes(chunks.writable, chunk)) {
        std::string message = ErrorTextPipeline::chunkKind("pipeline", chunk->name(), type->name(),
                                                           chunks.writable->name());
        this->state.getDiagnostics().error(loc, message);
        return arg;
      }
    }
    if (!last) {
      ResultReadableEnd readable = StreamPipeline::readableEnd(chunks, type);
      if (readable.isErr()) {
        this->state.getDiagnostics().error(loc, readable.error().message);
        return arg;
      }
      chunk = chunks.readable;
    }
    return TypeRelations(this->state)
        .convert(arg, Type::unionOf(
                          {this->state.getLibraryDeclarations().findLibraryClass("Stream")->type,
                           stageType}));
  }
  if (type->isFunction()) {
    const Type *ret = type->ret();
    if (first) {
      if (!ret->isAsyncIterator()) {
        std::string message = ErrorTextPipeline::source();
        this->state.getDiagnostics().error(loc, message);
        return arg;
      }
      chunk = ret->yielded();
      Expr *stage =
          stageCall("streamSourceStage", {chunk},
                    TypeRelations(this->state).coerce(arg, Type::function({}, 0, false, ret)));
      return stage->type->isError()
                 ? stage
                 : TypeRelations(this->state)
                       .convert(stage, Type::unionOf({this->state.getLibraryDeclarations()
                                                          .findLibraryClass("Stream")
                                                          ->type,
                                                      stageType}));
    }
    const Type *source = Type::asyncIterator(chunk);
    if (ret->isAsyncIterator()) {
      const Type *fn = Type::function({source}, 1, false, ret);
      Expr *stage = stageCall("streamTransformStage", {chunk, ret->yielded()},
                              TypeRelations(this->state).coerce(arg, fn));
      chunk = ret->yielded();
      return stage->type->isError()
                 ? stage
                 : TypeRelations(this->state)
                       .convert(stage, Type::unionOf({this->state.getLibraryDeclarations()
                                                          .findLibraryClass("Stream")
                                                          ->type,
                                                      stageType}));
    }
    if (ret->isPromise() && last) {
      const Type *fn = Type::function({source}, 1, false, ret);
      const Type *value = ret->promised();
      if (sinkValue) {
        *sinkValue = value;
      }
      Expr *stage = value->isVoid() ? stageCall("streamVoidSinkStage", {chunk},
                                                TypeRelations(this->state).coerce(arg, fn))
                                    : stageCall("streamSinkStage", {chunk, value},
                                                TypeRelations(this->state).coerce(arg, fn));
      return stage->type->isError()
                 ? stage
                 : TypeRelations(this->state)
                       .convert(stage, Type::unionOf({this->state.getLibraryDeclarations()
                                                          .findLibraryClass("Stream")
                                                          ->type,
                                                      stageType}));
    }
    std::string message = ErrorTextPipeline::function(last);
    this->state.getDiagnostics().error(loc, message);
    return arg;
  }
  if (first) {
    if (type->isAsyncIterator()) {
      chunk = type->yielded();
      Expr *stage = stageCall("streamAsyncIterableStage", {chunk}, arg);
      return stage->type->isError()
                 ? stage
                 : TypeRelations(this->state)
                       .convert(stage, Type::unionOf({this->state.getLibraryDeclarations()
                                                          .findLibraryClass("Stream")
                                                          ->type,
                                                      stageType}));
    }
    if (Expr *iterator = this->valuesIteratorOf(arg)) {
      chunk = iterator->type->yielded();
      Expr *stage = stageCall("streamIterableStage", {chunk}, iterator);
      return stage->type->isError()
                 ? stage
                 : TypeRelations(this->state)
                       .convert(stage, Type::unionOf({this->state.getLibraryDeclarations()
                                                          .findLibraryClass("Stream")
                                                          ->type,
                                                      stageType}));
    }
  }
  std::string message = ErrorTextPipeline::stage(type->name());
  this->state.getDiagnostics().error(loc, message);
  arg->type = Type::error();

  return arg;
}

Expr *StreamPipeline::valuesIteratorOf(Expr *collection) {
  const Type *type = collection->type;
  if (type->isIterator()) {
    return collection;
  }
  llvm::SMLoc loc = collection->loc();
  if (type->isArray()) {
    const Type *iterator = Type::iterator(type->inner());
    auto *member = this->state.getAst().make<MemberExpr>(loc, collection, "values");
    member->type = Type::function({}, 0, false, iterator);
    return ProcessChecker(this->state).builtinCall(loc, member, Builtin::ArrValues, {}, iterator);
  }
  Expr *iterator = DictChecker(this->state).iterableOf(collection);

  return iterator != collection && iterator->type->isIterator() ? iterator : nullptr;
}

bool StreamPipeline::streamTakes(const Type *writable, const Type *chunk) {
  if (writable == chunk) {
    return true;
  }
  if (writable->isBuffer() && chunk->isString()) {
    return true;
  }

  return TypeRelations::isImplicitlyConvertible(chunk, writable);
}

Expr *StreamPipeline::checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc) {
  std::string place = EventMapInference(this->state).placeOf(decl->loc());
  if (StreamPipeline::inModule(place, "node:internal/fs/streams")) {
    return StreamSources(this->state).checkFsStreamFunction(call, decl, loc);
  }
  bool promises = StreamPipeline::inModule(place, "node:stream/promises");
  if (!promises && !StreamPipeline::inModule(place, "node:stream")) {
    return nullptr;
  }
  llvm::StringRef fn = decl->name;
  if (fn != "pipeline" && fn != "finished" && fn != "compose") {
    return nullptr;
  }
  call->type = Type::error();
  if (CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = ExprChecker(this->state).checkExpr(call->args[i]);
    }
  };
  ClassDecl *streamClass = this->state.getLibraryDeclarations().findLibraryClass("Stream");
  if (!streamClass) {
    return nullptr;
  }
  const Type *streamType = streamClass->type;
  const Type *callback = StreamChecker(this->state).streamCallbackType();
  auto undefinedOf = [&](const Type *type) {
    return TypeRelations(this->state)
        .convert(ExprChecker(this->state).checkExpr(this->state.getAst().make<UndefinedExpr>(loc)),
                 type);
  };
  if (fn == "finished") {
    unsigned most = promises ? 2 : 3;
    ArgumentChecker::ResultArityMatches arity =
        ArgumentChecker(this->state).checkArity(call, most - 1, most);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(loc, arity.error().message);
      return call;
    }
    TypeAliasDecl *alias = this->state.getLibraryDeclarations().findLibraryType("FinishedOptions");
    const Type *options =
        alias ? Type::optional(TypeDeclarer(this->state).resolveAlias(alias)) : Type::error();
    Expr *stream = ArgumentChecker(this->state).checkArg(call->args[0], streamType);
    EmitterChecker(this->state).noteErrorsHandled(stream);
    Expr *opts = call->args.size() == most
                     ? ArgumentChecker(this->state).checkArg(call->args[1], options)
                     : undefinedOf(options);
    if (promises) {
      call->args = {stream, opts};
      std::vector<bool> given(2, true);
      return EventTargetChecker(this->state)
          .callLibraryStatic(call, streamClass, "finishedResult", {}, given, loc);
    }
    Expr *cb = ArgumentChecker(this->state).checkArg(call->args.back(), callback);
    call->args = {stream, opts, cb};
    std::vector<bool> given(3, true);
    return EventTargetChecker(this->state)
        .callLibraryStatic(call, streamClass, "eosFor", {}, given, loc);
  }
  if (fn == "compose") {
    if (call->args.empty()) {
      std::string message = ErrorTextCompose::streams();
      this->state.getDiagnostics().error(loc, message);
      return call;
    }
    std::vector<Expr *> streams;
    const Type *chunk = nullptr;
    StreamChunks head, tail;
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *arg = ExprChecker(this->state).checkExpr(call->args[i]);
      ClassDecl *cls = ClassDeclarer::classOf(arg->type);
      if (arg->type->isError()) {
        return call;
      }
      if (!cls || !ChunkTypes(this->state).isStreamClass(cls) || arg->type->isOptional()) {
        std::string message = ErrorTextCompose::notStream(arg->type->name());
        this->state.getDiagnostics().error(arg->loc(), message);
        checkArgsLoosely(i + 1);
        return call;
      }
      StreamChunks chunks = ChunkTypes(this->state).streamChunksOf(cls);
      if (i == 0) {
        head = chunks;
      }
      if (i + 1 == call->args.size()) {
        tail = chunks;
      }
      if (i > 0 && chunk && chunks.writable && !this->streamTakes(chunks.writable, chunk)) {
        std::string message = ErrorTextPipeline::chunkKind(
            "compose", chunk->name(), arg->type->name(), chunks.writable->name());
        this->state.getDiagnostics().error(arg->loc(), message);
        return call;
      }
      chunk = chunks.readable;
      streams.push_back(TypeRelations(this->state).convert(arg, streamType));
    }
    auto *list = this->state.getAst().make<ArrayLitExpr>(loc);
    for (Expr *s : streams) {
      list->elements.push_back({s, false});
    }
    list->type = Type::array(streamType);
    const Type *w = head.writable ? head.writable : Type::bytesType(Type::Bytes::Buffer);
    const Type *r = tail.readable ? tail.readable : Type::bytesType(Type::Bytes::Buffer);
    call->args = {list};
    std::vector<bool> given{true};
    return EventTargetChecker(this->state)
        .callLibraryStatic(call, streamClass, "composeAll", {w, r}, given, loc);
  }
  size_t count = call->args.size();
  Expr *callbackArg = nullptr;
  Expr *optionsArg = nullptr;
  if (!promises) {
    if (count < 3) {
      std::string message = ErrorTextPipeline::callback();
      this->state.getDiagnostics().error(loc, message);
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
    std::string message = ErrorTextPipeline::streams();
    this->state.getDiagnostics().error(loc, message);
    checkArgsLoosely(0);
    return call;
  }
  const Type *chunk = nullptr;
  const Type *sinkValue = nullptr;
  std::vector<Expr *> stages;
  bool lastIsStream = false;
  for (size_t i = 0; i < count; ++i) {
    bool stream = false;
    Expr *stage =
        this->streamStage(call->args[i], chunk, i == 0, i + 1 == count, stream, &sinkValue);
    if (stage->type->isError()) {
      checkArgsLoosely(i + 1);
      return call;
    }
    stages.push_back(stage);
    if (stream) {
      EmitterChecker(this->state).noteErrorsHandled(stage);
    }
    if (i + 1 == count) {
      lastIsStream = stream;
    }
  }
  ClassDecl *stageClass = this->state.getLibraryDeclarations().findLibraryClass("PipelineStage");
  const Type *element = Type::unionOf({streamType, stageClass->type});
  auto *list = this->state.getAst().make<ArrayLitExpr>(loc);
  Expr *lastStream = nullptr;
  for (size_t i = 0; i < stages.size(); ++i) {
    if (i + 1 == stages.size() && lastIsStream && !promises) {
      auto *conv = dyn_cast<CastExpr>(stages[i]);
      lastStream = conv ? conv->operand : stages[i];
      continue;
    }
    list->elements.push_back({stages[i], false});
  }
  list->type = Type::array(element);
  if (!promises) {
    Expr *cb = ArgumentChecker(this->state).checkArg(callbackArg, callback);
    if (cb->type->isError()) {
      return call;
    }
    std::vector<bool> given;
    if (lastStream) {
      call->args = {list, lastStream, cb};
      given.assign(3, true);
      return EventTargetChecker(this->state)
          .callLibraryStatic(call, streamClass, "pipelineTo", {lastStream->type}, given, loc);
    }
    call->args = {list, cb};
    given.assign(2, true);
    return EventTargetChecker(this->state)
        .callLibraryStatic(call, streamClass, "pipelineOut", {}, given, loc);
  }
  const Type *optionsType =
      Type::optional(Type::object({Field{"end", Type::optional(Type::boolTy()), false}}));
  Expr *options = optionsArg ? ArgumentChecker(this->state).checkArg(optionsArg, optionsType)
                             : undefinedOf(optionsType);
  if (options->type->isError()) {
    return call;
  }
  call->args = {list, options};
  std::vector<bool> given(2, true);
  if (sinkValue && !sinkValue->isVoid()) {
    return EventTargetChecker(this->state)
        .callLibraryStatic(call, streamClass, "pipelineValue", {sinkValue}, given, loc);
  }

  return EventTargetChecker(this->state)
      .callLibraryStatic(call, streamClass, "pipelineResult", {}, given, loc);
}

Expr *StreamPipeline::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl || !isa<FunctionDecl>(func->decl)) {
    return nullptr;
  }

  return this->checkStreamFunction(call, func->decl, name->loc());
}

}
