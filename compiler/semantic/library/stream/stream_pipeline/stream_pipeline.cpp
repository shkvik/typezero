#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/program/library/stream/stream_function_table/stream_function_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StreamPipeline::StreamPipeline(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.chunkTypes;
  this->callChecker = &all.callChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->streamChecker = &all.streamChecker;
  this->streamSources = &all.streamSources;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->emitterChecker = &all.emitterChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->eventMapInference = &all.eventMapInference;
  this->eventTargetChecker = &all.eventTargetChecker;
}

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

Expr *StreamPipeline::streamStage(Expr *&arg, const Type *&chunk, bool first, bool last,
                                  bool &stream, const Type **sinkValue) {
  ClassDecl *stageClass = this->state.getLibraryDeclarations().findLibraryClass("PipelineStage");
  StageShape stage{
      .loc = arg->loc(),
      .stageType = stageClass != nullptr ? stageClass->type : Type::error(),
      .first = first,
      .last = last,
  };
  stream = false;
  arg = this->checkStageValue(arg, chunk, first);
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (cls != nullptr && this->chunkTypes->isStreamClass(cls) && !type->isOptional()) {
    stream = true;
    return this->streamClassStage(arg, chunk, stage, cls);
  }
  if (type->isFunction()) {
    return this->functionStage(arg, chunk, stage, sinkValue);
  }
  Expr *source = first ? this->iterableSourceStage(arg, chunk, stage) : nullptr;
  if (source != nullptr) {
    return source;
  }
  std::string message = ErrorTextPipeline::stage(type->name());
  this->state.getDiagnostics().error(stage.loc, message);
  arg->type = Type::error();

  return arg;
}

Expr *StreamPipeline::checkStageValue(Expr *arg, const Type *chunk, bool first) {
  auto *fe = dyn_cast<FunctionExpr>(arg);
  if (fe == nullptr) {
    return this->exprChecker->checkExpr(arg);
  }
  Signature sig;
  if (!first) {
    sig.params = {Type::asyncIterator(chunk)};
    sig.minArgs = 1;
  }

  return this->closureChecker->checkFunctionExpr(fe, &sig);
}

Expr *StreamPipeline::stageCall(const StageShape &stage, llvm::StringRef fn,
                                std::vector<const Type *> bound, Expr *value) {
  auto *callee = this->state.getAst().make<NameExpr>(stage.loc, fn);
  auto *call = this->state.getAst().make<CallExpr>(stage.loc, callee, std::vector<Expr *>{value});
  call->type = Type::error();
  std::vector<bool> given{true};
  Expr *made = this->eventTargetChecker->callLibraryFunction(call, fn, bound, given, stage.loc);

  return made->type->isError() ? made : this->typeRelations->convert(made, stage.stageType);
}

Expr *StreamPipeline::asPipelineStage(const StageShape &stage, Expr *made) {
  if (made->type->isError()) {
    return made;
  }
  const Type *streamType = this->state.getLibraryDeclarations().findLibraryClass("Stream")->type;

  return this->typeRelations->convert(made, Type::unionOf({streamType, stage.stageType}));
}

Expr *StreamPipeline::streamClassStage(Expr *arg, const Type *&chunk, const StageShape &stage,
                                       ClassDecl *cls) {
  const Type *type = arg->type;
  StreamChunks chunks = this->chunkTypes->streamChunksOf(cls);
  if (!stage.first) {
    ResultWritableEnd writable = StreamPipeline::writableEnd(chunks, type);
    if (writable.isErr()) {
      this->state.getDiagnostics().error(stage.loc, writable.error().message);
      return arg;
    }
    if (chunk != nullptr && !this->streamTakes(chunks.writable, chunk)) {
      std::string message = ErrorTextPipeline::chunkKind("pipeline", chunk->name(), type->name(),
                                                         chunks.writable->name());
      this->state.getDiagnostics().error(stage.loc, message);
      return arg;
    }
  }
  if (!stage.last) {
    ResultReadableEnd readable = StreamPipeline::readableEnd(chunks, type);
    if (readable.isErr()) {
      this->state.getDiagnostics().error(stage.loc, readable.error().message);
      return arg;
    }
    chunk = chunks.readable;
  }

  return this->asPipelineStage(stage, arg);
}

Expr *StreamPipeline::functionStage(Expr *arg, const Type *&chunk, const StageShape &stage,
                                    const Type **sinkValue) {
  const Type *ret = arg->type->ret();
  if (stage.first) {
    return this->sourceFunctionStage(arg, chunk, stage);
  }
  const Type *source = Type::asyncIterator(chunk);
  const Type *fn = Type::function({source}, 1, false, ret);
  if (ret->isAsyncIterator()) {
    Expr *coerced = this->typeRelations->coerce(arg, fn);
    Expr *made = this->stageCall(stage, "streamTransformStage", {chunk, ret->yielded()}, coerced);
    chunk = ret->yielded();
    return this->asPipelineStage(stage, made);
  }
  if (ret->isPromise() && stage.last) {
    return this->sinkFunctionStage(arg, chunk, stage, fn, sinkValue);
  }
  std::string message = ErrorTextPipeline::function(stage.last);
  this->state.getDiagnostics().error(stage.loc, message);

  return arg;
}

Expr *StreamPipeline::sourceFunctionStage(Expr *arg, const Type *&chunk, const StageShape &stage) {
  const Type *ret = arg->type->ret();
  if (!ret->isAsyncIterator()) {
    std::string message = ErrorTextPipeline::source();
    this->state.getDiagnostics().error(stage.loc, message);
    return arg;
  }
  chunk = ret->yielded();
  Expr *coerced = this->typeRelations->coerce(arg, Type::function({}, 0, false, ret));
  Expr *made = this->stageCall(stage, "streamSourceStage", {chunk}, coerced);

  return this->asPipelineStage(stage, made);
}

Expr *StreamPipeline::sinkFunctionStage(Expr *arg, const Type *chunk, const StageShape &stage,
                                        const Type *fn, const Type **sinkValue) {
  const Type *value = arg->type->ret()->promised();
  if (sinkValue != nullptr) {
    *sinkValue = value;
  }
  Expr *coerced = this->typeRelations->coerce(arg, fn);
  Expr *made = value->isVoid() ? this->stageCall(stage, "streamVoidSinkStage", {chunk}, coerced)
                               : this->stageCall(stage, "streamSinkStage", {chunk, value}, coerced);

  return this->asPipelineStage(stage, made);
}

Expr *StreamPipeline::iterableSourceStage(Expr *arg, const Type *&chunk, const StageShape &stage) {
  const Type *type = arg->type;
  if (type->isAsyncIterator()) {
    chunk = type->yielded();
    Expr *made = this->stageCall(stage, "streamAsyncIterableStage", {chunk}, arg);
    return this->asPipelineStage(stage, made);
  }
  Expr *iterator = this->valuesIteratorOf(arg);
  if (iterator == nullptr) {
    return nullptr;
  }
  chunk = iterator->type->yielded();
  Expr *made = this->stageCall(stage, "streamIterableStage", {chunk}, iterator);

  return this->asPipelineStage(stage, made);
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
    return this->processChecker->builtinCall(loc, member, Builtin::ArrValues, {}, iterator);
  }
  Expr *iterator = this->dictChecker->iterableOf(collection);

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
  std::string place = this->eventMapInference->placeOf(decl->loc());
  if (StreamPipeline::inModule(place, "node:internal/fs/streams")) {
    return this->streamSources->checkFsStreamFunction(call, decl, loc);
  }
  bool hasPromises = StreamPipeline::inModule(place, "node:stream/promises");
  if (!hasPromises && !StreamPipeline::inModule(place, "node:stream")) {
    return nullptr;
  }
  std::optional<StreamFunction> function = StreamFunctionTable::find(decl->name);
  if (!function.has_value()) {
    return nullptr;
  }
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  ClassDecl *streamClass = this->state.getLibraryDeclarations().findLibraryClass("Stream");
  if (streamClass == nullptr) {
    return nullptr;
  }
  StreamCall stream{
      .call = call,
      .loc = loc,
      .streamClass = streamClass,
      .streamType = streamClass->type,
      .callback = this->streamChecker->streamCallbackType(),
      .promises = hasPromises,
  };
  switch (*function) {
    case StreamFunction::FINISHED:
      return this->checkFinished(stream);
    case StreamFunction::COMPOSE:
      return this->checkCompose(stream);
    case StreamFunction::PIPELINE:
      return this->checkPipeline(stream);
  }

  return call;
}

void StreamPipeline::walkArgsLoosely(CallExpr *call, size_t from) {
  for (size_t i = from; i < call->args.size(); ++i) {
    call->args[i] = this->exprChecker->checkExpr(call->args[i]);
  }
}

Expr *StreamPipeline::undefinedOf(llvm::SMLoc loc, const Type *type) {
  auto *nothing = this->state.getAst().make<UndefinedExpr>(loc);
  Expr *checked = this->exprChecker->checkExpr(nothing);

  return this->typeRelations->convert(checked, type);
}

Expr *StreamPipeline::checkFinished(const StreamCall &stream) {
  CallExpr *call = stream.call;
  unsigned most = stream.promises ? 2 : 3;
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, most - 1, most);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(stream.loc, arity.error().message);
    return call;
  }
  TypeAliasDecl *alias = this->state.getLibraryDeclarations().findLibraryType("FinishedOptions");
  const Type *options =
      alias != nullptr ? Type::optional(this->typeDeclarer->resolveAlias(alias)) : Type::error();
  Expr *target = this->argumentChecker->checkArg(call->args[0], stream.streamType);
  this->emitterChecker->noteErrorsHandled(target);
  Expr *opts = call->args.size() == most ? this->argumentChecker->checkArg(call->args[1], options)
                                         : this->undefinedOf(stream.loc, options);
  if (stream.promises) {
    call->args = {target, opts};
    std::vector<bool> given(2, true);
    return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "finishedResult",
                                                       {}, given, stream.loc);
  }
  Expr *cb = this->argumentChecker->checkArg(call->args.back(), stream.callback);
  call->args = {target, opts, cb};
  std::vector<bool> given(3, true);

  return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "eosFor", {}, given,
                                                     stream.loc);
}

Expr *StreamPipeline::checkCompose(const StreamCall &stream) {
  CallExpr *call = stream.call;
  if (call->args.empty()) {
    std::string message = ErrorTextCompose::streams();
    this->state.getDiagnostics().error(stream.loc, message);
    return call;
  }
  std::vector<Expr *> streams;
  const Type *chunk = nullptr;
  StreamChunks head;
  StreamChunks tail;
  for (size_t i = 0; i < call->args.size(); ++i) {
    std::optional<StreamChunks> chunks = this->composedPart(stream, i, chunk, streams);
    if (!chunks.has_value()) {
      return call;
    }
    if (i == 0) {
      head = *chunks;
    }
    tail = *chunks;
    chunk = chunks->readable;
  }
  auto *list = this->state.getAst().make<ArrayLitExpr>(stream.loc);
  for (Expr *s : streams) {
    list->elements.push_back({s, false});
  }
  list->type = Type::array(stream.streamType);
  const Type *buffer = Type::bytesType(Type::Bytes::Buffer);
  const Type *w = head.writable != nullptr ? head.writable : buffer;
  const Type *r = tail.readable != nullptr ? tail.readable : buffer;
  call->args = {list};
  std::vector<bool> given{true};

  return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "composeAll", {w, r},
                                                     given, stream.loc);
}

std::optional<StreamChunks> StreamPipeline::composedPart(const StreamCall &stream, size_t i,
                                                         const Type *chunk,
                                                         std::vector<Expr *> &streams) {
  CallExpr *call = stream.call;
  Expr *arg = this->exprChecker->checkExpr(call->args[i]);
  ClassDecl *cls = ClassDeclarer::classOf(arg->type);
  if (arg->type->isError()) {
    return std::nullopt;
  }
  bool isStreamed =
      cls != nullptr && this->chunkTypes->isStreamClass(cls) && !arg->type->isOptional();
  if (!isStreamed) {
    std::string message = ErrorTextCompose::notStream(arg->type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    this->walkArgsLoosely(call, i + 1);
    return std::nullopt;
  }
  StreamChunks chunks = this->chunkTypes->streamChunksOf(cls);
  bool isFitting = i == 0 || chunk == nullptr || chunks.writable == nullptr ||
                   this->streamTakes(chunks.writable, chunk);
  if (!isFitting) {
    std::string message = ErrorTextPipeline::chunkKind("compose", chunk->name(), arg->type->name(),
                                                       chunks.writable->name());
    this->state.getDiagnostics().error(arg->loc(), message);
    return std::nullopt;
  }
  streams.push_back(this->typeRelations->convert(arg, stream.streamType));

  return chunks;
}

Expr *StreamPipeline::checkPipeline(const StreamCall &stream) {
  CallExpr *call = stream.call;
  std::optional<PipelineArgs> args = this->pipelineArgs(stream);
  if (!args.has_value()) {
    return call;
  }
  std::optional<PipelineStages> stages = this->pipelineStages(stream, args->count);
  if (!stages.has_value()) {
    return call;
  }
  ArrayLitExpr *list = this->stageList(stream, *stages);
  if (!stream.promises) {
    return this->finishCallbackPipeline(stream, list, stages->lastStream, args->callback);
  }

  return this->finishPromisePipeline(stream, list, args->options, stages->sinkValue);
}

std::optional<StreamPipeline::PipelineArgs> StreamPipeline::pipelineArgs(const StreamCall &stream) {
  CallExpr *call = stream.call;
  PipelineArgs args{
      .count = call->args.size(),
      .callback = nullptr,
      .options = nullptr,
  };
  if (!stream.promises) {
    if (args.count < 3) {
      std::string message = ErrorTextPipeline::callback();
      this->state.getDiagnostics().error(stream.loc, message);
      this->walkArgsLoosely(call, 0);
      return std::nullopt;
    }
    args.callback = call->args.back();
    --args.count;
  } else if (args.count > 0 && isa<ObjectLitExpr>(call->args.back())) {
    args.options = call->args.back();
    --args.count;
  }
  if (args.count < 2) {
    std::string message = ErrorTextPipeline::streams();
    this->state.getDiagnostics().error(stream.loc, message);
    this->walkArgsLoosely(call, 0);
    return std::nullopt;
  }

  return args;
}

std::optional<StreamPipeline::PipelineStages>
StreamPipeline::pipelineStages(const StreamCall &stream, size_t count) {
  CallExpr *call = stream.call;
  PipelineStages result{
      .stages = {},
      .lastIsStream = false,
      .lastStream = nullptr,
      .sinkValue = nullptr,
  };
  const Type *chunk = nullptr;
  for (size_t i = 0; i < count; ++i) {
    bool isStream = false;
    bool isLast = i + 1 == count;
    Expr *stage =
        this->streamStage(call->args[i], chunk, i == 0, isLast, isStream, &result.sinkValue);
    if (stage->type->isError()) {
      this->walkArgsLoosely(call, i + 1);
      return std::nullopt;
    }
    result.stages.push_back(stage);
    if (isStream) {
      this->emitterChecker->noteErrorsHandled(stage);
    }
    if (isLast) {
      result.lastIsStream = isStream;
    }
  }

  return result;
}

ArrayLitExpr *StreamPipeline::stageList(const StreamCall &stream, PipelineStages &stages) {
  ClassDecl *stageClass = this->state.getLibraryDeclarations().findLibraryClass("PipelineStage");
  const Type *element = Type::unionOf({stream.streamType, stageClass->type});
  auto *list = this->state.getAst().make<ArrayLitExpr>(stream.loc);
  for (size_t i = 0; i < stages.stages.size(); ++i) {
    Expr *stage = stages.stages[i];
    bool isTail = i + 1 == stages.stages.size() && stages.lastIsStream && !stream.promises;
    if (isTail) {
      auto *conv = dyn_cast<CastExpr>(stage);
      stages.lastStream = conv != nullptr ? conv->operand : stage;
      continue;
    }
    list->elements.push_back({stage, false});
  }
  list->type = Type::array(element);

  return list;
}

Expr *StreamPipeline::finishCallbackPipeline(const StreamCall &stream, ArrayLitExpr *list,
                                             Expr *lastStream, Expr *callbackArg) {
  CallExpr *call = stream.call;
  Expr *cb = this->argumentChecker->checkArg(callbackArg, stream.callback);
  if (cb->type->isError()) {
    return call;
  }
  std::vector<bool> given;
  if (lastStream != nullptr) {
    call->args = {list, lastStream, cb};
    given.assign(3, true);
    return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "pipelineTo",
                                                       {lastStream->type}, given, stream.loc);
  }
  call->args = {list, cb};
  given.assign(2, true);

  return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "pipelineOut", {},
                                                     given, stream.loc);
}

Expr *StreamPipeline::finishPromisePipeline(const StreamCall &stream, ArrayLitExpr *list,
                                            Expr *optionsArg, const Type *sinkValue) {
  CallExpr *call = stream.call;
  const Type *optionsType =
      Type::optional(Type::object({Field{"end", Type::optional(Type::boolTy()), false}}));
  Expr *options = optionsArg != nullptr ? this->argumentChecker->checkArg(optionsArg, optionsType)
                                        : this->undefinedOf(stream.loc, optionsType);
  if (options->type->isError()) {
    return call;
  }
  call->args = {list, options};
  std::vector<bool> given(2, true);
  if (sinkValue != nullptr && !sinkValue->isVoid()) {
    return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "pipelineValue",
                                                       {sinkValue}, given, stream.loc);
  }

  return this->eventTargetChecker->callLibraryStatic(call, stream.streamClass, "pipelineResult", {},
                                                     given, stream.loc);
}

Expr *StreamPipeline::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl || !isa<FunctionDecl>(func->decl)) {
    return nullptr;
  }

  return this->checkStreamFunction(call, func->decl, name->loc());
}

}
