#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_sources/stream_sources_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class LibraryCalls;
class TypeDeclarer;
class TypeRelations;
class StreamPipeline;
class ArgumentChecker;
class IterableChecker;
class EventTargetChecker;

class StreamSources {
public:
  StreamSources(SemanticState &owner, SemanticParts &all);

  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  Expr *processStream(MemberExpr *member);
  VarSymbol *processStdinVar();
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  using ResultValuesIterator = Result<Expr *, StreamSourcesValuesIteratorError>;

  ResultValuesIterator valuesIterator(Expr *source);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  StreamPipeline *streamPipeline = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IterableChecker *iterableChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
