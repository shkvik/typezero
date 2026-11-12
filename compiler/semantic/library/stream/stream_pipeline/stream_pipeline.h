#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ChunkTypes;
class CallChecker;
class DictChecker;
class ExprChecker;
class TypeDeclarer;
class StreamChecker;
class StreamSources;
class TypeRelations;
class ClassDeclarer;
class ClosureChecker;
class EmitterChecker;
class ProcessChecker;
class ArgumentChecker;
class EventMapInference;
class EventTargetChecker;

class StreamPipeline {
public:
  StreamPipeline(SemanticState &owner, SemanticParts &all);

  Expr *checkStreamFunction(CallExpr *call, FunctionDecl *functionDecl, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *function);
  Expr *streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                    const Type **sinkValue);
  bool streamTakes(const Type *writable, const Type *chunk);
  Expr *valuesIteratorOf(Expr *collection);

private:
  using ResultWritableEnd = Result<const Type *, StreamPipelineWritableEndError>;
  using ResultReadableEnd = Result<const Type *, StreamPipelineReadableEndError>;

  ResultWritableEnd writableEnd(const StreamChunks &chunks, const Type *type) const;
  ResultReadableEnd readableEnd(const StreamChunks &chunks, const Type *type) const;
  bool inModule(const std::string &place, llvm::StringRef module) const;

  struct StageShape {
    llvm::SMLoc loc;
    const Type *stageType;
    bool first;
    bool last;
  };

  Expr *checkStageValue(Expr *arg, const Type *chunk, bool first);
  Expr *stageCall(const StageShape &stage, llvm::StringRef functionName,
                  std::vector<const Type *> bound, Expr *value);
  Expr *asPipelineStage(const StageShape &stage, Expr *made);
  Expr *streamClassStage(Expr *arg, const Type *&chunk, const StageShape &stage,
                         ClassDecl *classDecl);
  Expr *functionStage(Expr *arg, const Type *&chunk, const StageShape &stage,
                      const Type **sinkValue);
  Expr *sourceFunctionStage(Expr *arg, const Type *&chunk, const StageShape &stage);
  Expr *sinkFunctionStage(Expr *arg, const Type *chunk, const StageShape &stage,
                          const Type *functionType, const Type **sinkValue);
  Expr *iterableSourceStage(Expr *arg, const Type *&chunk, const StageShape &stage);

  struct StreamCall {
    CallExpr *call;
    llvm::SMLoc loc;
    ClassDecl *streamClass;
    const Type *streamType;
    const Type *callback;
    bool promises;
  };

  struct PipelineArgs {
    size_t count;
    Expr *callback;
    Expr *options;
  };

  struct PipelineStages {
    std::vector<Expr *> stages;
    bool lastIsStream;
    Expr *lastStream;
    const Type *sinkValue;
  };

  void walkArgsLoosely(CallExpr *call, size_t from);
  Expr *undefinedOf(llvm::SMLoc loc, const Type *type);
  Expr *checkFinished(const StreamCall &stream);
  Expr *checkCompose(const StreamCall &stream);
  std::optional<StreamChunks> composedPart(const StreamCall &stream, size_t index,
                                           const Type *chunk, std::vector<Expr *> &streams);
  Expr *checkPipeline(const StreamCall &stream);
  std::optional<PipelineArgs> pipelineArgs(const StreamCall &stream);
  std::optional<PipelineStages> pipelineStages(const StreamCall &stream, size_t count);
  ArrayLitExpr *stageList(const StreamCall &stream, PipelineStages &stages);
  Expr *finishCallbackPipeline(const StreamCall &stream, ArrayLitExpr *list, Expr *lastStream,
                               Expr *callbackArg);
  Expr *finishPromisePipeline(const StreamCall &stream, ArrayLitExpr *list, Expr *optionsArg,
                              const Type *sinkValue);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  StreamChecker *streamChecker = nullptr;
  StreamSources *streamSources = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
