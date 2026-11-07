#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/closure_checker/closure_checker.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/events/event_map_inference/event_map_inference.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/library/stream/stream_checker/stream_checker.h"
#include "compiler/library/stream/stream_sources/stream_sources.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool StreamPipeline::inModule(const std::string &place, llvm::StringRef module) {
  llvm::StringRef at(place);

  return at.consume_front(module) && at.consume_front(":");
}

StreamPipeline::StreamPipeline(Checker &owner)
    : checker(owner) {}

Expr *StreamPipeline::streamStage(Expr *&arg, const Type *&chunk, bool first, bool last,
                                  bool &stream, const Type **sinkValue) {
  llvm::SMLoc loc = arg->loc();
  ClassDecl *stageClass = this->checker.libraryClass("PipelineStage");
  const Type *stageType = stageClass ? stageClass->type : Type::error();
  stream = false;
  auto stageCall = [&](llvm::StringRef fn, std::vector<const Type *> bound, Expr *value) -> Expr * {
    auto *callee = this->checker.ctx_.make<NameExpr>(loc, fn);
    auto *call = this->checker.ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{value});
    call->type = Type::error();
    std::vector<bool> given{true};
    Expr *made =
        this->checker.eventTargetChecker().callLibraryFunction(call, fn, bound, given, loc);

    return made->type->isError() ? made : this->checker.typeRelations().convert(made, stageType);
  };
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    Checker::Signature sig;
    if (first) {
      sig.params = {};
      sig.minArgs = 0;
    } else {
      sig.params = {Type::asyncIterator(chunk)};
      sig.minArgs = 1;
    }
    arg = this->checker.closureChecker().checkFunctionExpr(fe, &sig);
  } else {
    arg = this->checker.exprChecker().checkExpr(arg);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (ClassDecl *cls = ClassDeclarer::classOf(type);
      cls && this->checker.chunkTypes().isStreamClass(cls) && !type->isOptional()) {
    stream = true;
    Checker::StreamChunks chunks = this->checker.chunkTypes().streamChunksOf(cls);
    if (!first) {
      if (!chunks.writable) {
        this->checker.diags_.error(
            loc, "pipeline() writes the chunks before it to each stream after the first, "
                 "but '" +
                     type->name() + "' is not writable");
        return arg;
      }
      if (chunk && this->checker.chunkTypes().recordStreamChunk(chunks.writable, chunk, loc)) {
      } else if (chunk && !this->checker.openMaps_.count(chunk) &&
                 !this->streamTakes(chunks.writable, chunk)) {
        this->checker.diags_.error(loc, "pipeline() writes chunks of " + chunk->name().str() +
                                            " to '" + type->name().str() + "', which takes " +
                                            chunks.writable->name().str());
        return arg;
      }
    }
    if (!last) {
      if (!chunks.readable) {
        this->checker.diags_.error(loc, "pipeline() reads each stream but the last, and '" +
                                            type->name() + "' is not readable");
        return arg;
      }
      chunk = chunks.readable;
    }
    return this->checker.typeRelations().convert(
        arg, Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
  }
  if (type->isFunction()) {
    const Type *ret = type->ret();
    if (first) {
      if (!ret->isAsyncIterator()) {
        this->checker.diags_.error(
            loc, "the first function of pipeline() is the source of the chunks: an async "
                 "generator, e.g. 'async function* () { yield \"a\" }'");
        return arg;
      }
      chunk = ret->yielded();
      Expr *stage =
          stageCall("streamSourceStage", {chunk},
                    this->checker.typeRelations().coerce(arg, Type::function({}, 0, false, ret)));
      return stage->type->isError()
                 ? stage
                 : this->checker.typeRelations().convert(
                       stage,
                       Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
    }
    const Type *source = Type::asyncIterator(chunk);
    if (ret->isAsyncIterator()) {
      const Type *fn = Type::function({source}, 1, false, ret);
      Expr *stage = stageCall("streamTransformStage", {chunk, ret->yielded()},
                              this->checker.typeRelations().coerce(arg, fn));
      chunk = ret->yielded();
      return stage->type->isError()
                 ? stage
                 : this->checker.typeRelations().convert(
                       stage,
                       Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
    }
    if (ret->isPromise() && last) {
      const Type *fn = Type::function({source}, 1, false, ret);
      const Type *value = ret->promised();
      if (sinkValue) {
        *sinkValue = value;
      }
      Expr *stage = value->isVoid() ? stageCall("streamVoidSinkStage", {chunk},
                                                this->checker.typeRelations().coerce(arg, fn))
                                    : stageCall("streamSinkStage", {chunk, value},
                                                this->checker.typeRelations().coerce(arg, fn));
      return stage->type->isError()
                 ? stage
                 : this->checker.typeRelations().convert(
                       stage,
                       Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
    }
    this->checker.diags_.error(
        loc, last ? "the last function of pipeline() is an async generator over the "
                    "chunks before it, or an async function that takes them and gives a "
                    "Promise"
                  : "a function in pipeline() is an async generator over the chunks "
                    "before it, e.g. 'async function* (source) { for await (const chunk "
                    "of source) yield chunk }'");
    return arg;
  }
  if (first) {
    if (type->isAsyncIterator()) {
      chunk = type->yielded();
      Expr *stage = stageCall("streamAsyncIterableStage", {chunk}, arg);
      return stage->type->isError()
                 ? stage
                 : this->checker.typeRelations().convert(
                       stage,
                       Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
    }
    if (Expr *iterator = this->valuesIteratorOf(arg)) {
      chunk = iterator->type->yielded();
      Expr *stage = stageCall("streamIterableStage", {chunk}, iterator);
      return stage->type->isError()
                 ? stage
                 : this->checker.typeRelations().convert(
                       stage,
                       Type::unionOf({this->checker.libraryClass("Stream")->type, stageType}));
    }
  }
  this->checker.diags_.error(
      loc, "pipeline() takes streams, async generator functions over the chunks and, "
           "first, an iterable; this is '" +
               type->name() + "'");
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
    auto *member = this->checker.ctx_.make<MemberExpr>(loc, collection, "values");
    member->type = Type::function({}, 0, false, iterator);
    return this->checker.processChecker().builtinCall(loc, member, Builtin::ArrValues, {},
                                                      iterator);
  }
  Expr *iterator = this->checker.dictChecker().iterableOf(collection);

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
  std::string place = this->checker.eventMapInference().placeOf(decl->loc());
  if (StreamPipeline::inModule(place, "node:internal/fs/streams")) {
    return this->checker.streamSources().checkFsStreamFunction(call, decl, loc);
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
  if (this->checker.callChecker().rejectSpread(call)) {
    return call;
  }
  auto checkArgsLoosely = [&](size_t from) {
    for (size_t i = from; i < call->args.size(); ++i) {
      call->args[i] = this->checker.exprChecker().checkExpr(call->args[i]);
    }
  };
  ClassDecl *streamClass = this->checker.libraryClass("Stream");
  if (!streamClass) {
    return nullptr;
  }
  const Type *streamType = streamClass->type;
  const Type *callback = this->checker.streamChecker().streamCallbackType();
  auto undefinedOf = [&](const Type *type) {
    return this->checker.typeRelations().convert(
        this->checker.exprChecker().checkExpr(this->checker.ctx_.make<UndefinedExpr>(loc)), type);
  };
  if (fn == "finished") {
    unsigned most = promises ? 2 : 3;
    ArgumentChecker::ResultArityMatches arity =
        this->checker.argumentChecker().checkArity(call, most - 1, most);
    if (arity.isErr()) {
      this->checker.diags_.error(loc, arity.error().message);
      return call;
    }
    TypeAliasDecl *alias = this->checker.libTypes_.lookup("FinishedOptions");
    const Type *options =
        alias ? Type::optional(this->checker.typeDeclarer().resolveAlias(alias)) : Type::error();
    Expr *stream = this->checker.argumentChecker().checkArg(call->args[0], streamType);
    this->checker.emitterChecker().noteErrorsHandled(stream);
    Expr *opts = call->args.size() == most
                     ? this->checker.argumentChecker().checkArg(call->args[1], options)
                     : undefinedOf(options);
    if (promises) {
      call->args = {stream, opts};
      std::vector<bool> given(2, true);
      return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass,
                                                                  "finishedResult", {}, given, loc);
    }
    Expr *cb = this->checker.argumentChecker().checkArg(call->args.back(), callback);
    call->args = {stream, opts, cb};
    std::vector<bool> given(3, true);
    return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "eosFor", {},
                                                                given, loc);
  }
  if (fn == "compose") {
    if (call->args.empty()) {
      this->checker.diags_.error(
          loc, "compose() takes the streams to make one Duplex of, e.g. 'compose(a, b)'");
      return call;
    }
    std::vector<Expr *> streams;
    const Type *chunk = nullptr;
    Checker::StreamChunks head, tail;
    for (size_t i = 0; i < call->args.size(); ++i) {
      Expr *arg = this->checker.exprChecker().checkExpr(call->args[i]);
      ClassDecl *cls = ClassDeclarer::classOf(arg->type);
      if (arg->type->isError()) {
        return call;
      }
      if (!cls || !this->checker.chunkTypes().isStreamClass(cls) || arg->type->isOptional()) {
        this->checker.diags_.error(arg->loc(),
                                   "compose() takes streams, and this is '" + arg->type->name() +
                                       "'; to put a function between them, use pipeline()");
        checkArgsLoosely(i + 1);
        return call;
      }
      Checker::StreamChunks chunks = this->checker.chunkTypes().streamChunksOf(cls);
      if (i == 0) {
        head = chunks;
      }
      if (i + 1 == call->args.size()) {
        tail = chunks;
      }
      if (i > 0 && chunk && chunks.writable && !this->streamTakes(chunks.writable, chunk)) {
        this->checker.diags_.error(arg->loc(), "compose() writes chunks of " + chunk->name().str() +
                                                   " to '" + arg->type->name().str() +
                                                   "', which takes " +
                                                   chunks.writable->name().str());
        return call;
      }
      chunk = chunks.readable;
      streams.push_back(this->checker.typeRelations().convert(arg, streamType));
    }
    auto *list = this->checker.ctx_.make<ArrayLitExpr>(loc);
    for (Expr *s : streams) {
      list->elements.push_back({s, false});
    }
    list->type = Type::array(streamType);
    const Type *w = head.writable ? head.writable : Type::bytesType(Type::Bytes::Buffer);
    const Type *r = tail.readable ? tail.readable : Type::bytesType(Type::Bytes::Buffer);
    call->args = {list};
    std::vector<bool> given{true};
    return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "composeAll",
                                                                {w, r}, given, loc);
  }
  size_t count = call->args.size();
  Expr *callbackArg = nullptr;
  Expr *optionsArg = nullptr;
  if (!promises) {
    if (count < 3) {
      this->checker.diags_.error(loc, "pipeline() takes two streams or more and a callback, e.g. "
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
    this->checker.diags_.error(
        loc, "pipeline() takes two streams or more, e.g. 'pipeline(source, destination)'");
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
      this->checker.emitterChecker().noteErrorsHandled(stage);
    }
    if (i + 1 == count) {
      lastIsStream = stream;
    }
  }
  ClassDecl *stageClass = this->checker.libraryClass("PipelineStage");
  const Type *element = Type::unionOf({streamType, stageClass->type});
  auto *list = this->checker.ctx_.make<ArrayLitExpr>(loc);
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
    Expr *cb = this->checker.argumentChecker().checkArg(callbackArg, callback);
    if (cb->type->isError()) {
      return call;
    }
    std::vector<bool> given;
    if (lastStream) {
      call->args = {list, lastStream, cb};
      given.assign(3, true);
      return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "pipelineTo",
                                                                  {lastStream->type}, given, loc);
    }
    call->args = {list, cb};
    given.assign(2, true);
    return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "pipelineOut",
                                                                {}, given, loc);
  }
  const Type *optionsType =
      Type::optional(Type::object({Field{"end", Type::optional(Type::boolTy()), false}}));
  Expr *options = optionsArg ? this->checker.argumentChecker().checkArg(optionsArg, optionsType)
                             : undefinedOf(optionsType);
  if (options->type->isError()) {
    return call;
  }
  call->args = {list, options};
  std::vector<bool> given(2, true);
  if (sinkValue && !sinkValue->isVoid()) {
    return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "pipelineValue",
                                                                {sinkValue}, given, loc);
  }

  return this->checker.eventTargetChecker().callLibraryStatic(call, streamClass, "pipelineResult",
                                                              {}, given, loc);
}

Expr *StreamPipeline::checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func) {
  if (!func->decl || !isa<FunctionDecl>(func->decl)) {
    return nullptr;
  }

  return this->checkStreamFunction(call, func->decl, name->loc());
}

}
