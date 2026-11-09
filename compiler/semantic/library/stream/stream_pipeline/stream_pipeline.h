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
class ClosureChecker;
class EmitterChecker;
class ProcessChecker;
class ArgumentChecker;
class EventMapInference;
class EventTargetChecker;

class StreamPipeline {
public:
  StreamPipeline(SemanticState &owner, SemanticParts &all);

  Expr *checkStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  Expr *streamStage(Expr *&arg, const Type *&chunk, bool first, bool last, bool &stream,
                    const Type **sinkValue);
  bool streamTakes(const Type *writable, const Type *chunk);
  Expr *valuesIteratorOf(Expr *collection);

private:
  using ResultWritableEnd = Result<const Type *, StreamPipelineWritableEndError>;
  using ResultReadableEnd = Result<const Type *, StreamPipelineReadableEndError>;

  static ResultWritableEnd writableEnd(const StreamChunks &chunks, const Type *type);
  static ResultReadableEnd readableEnd(const StreamChunks &chunks, const Type *type);
  static bool inModule(const std::string &place, llvm::StringRef module);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  CallChecker *callChecker = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  StreamChecker *streamChecker = nullptr;
  StreamSources *streamSources = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
