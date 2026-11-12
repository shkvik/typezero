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

  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *functionDecl, llvm::SMLoc loc);
  Expr *processStream(MemberExpr *member);
  VarSymbol *processStdinVar();
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  using ResultValuesIterator = Result<Expr *, StreamSourcesValuesIteratorError>;

  struct FsStreamArgs {
    const Type *options = nullptr;
    const Type *encodingType = nullptr;
    Expr *opts = nullptr;
    Expr *encoding = nullptr;
    bool text = false;
    bool fresh = false;
  };

  bool matchFsStreamArity(CallExpr *call, llvm::SMLoc loc);
  void walkFsStreamOptions(Expr *arg, FsStreamArgs &args);
  void scanOptionsLiteral(const ObjectLitExpr *literal, FsStreamArgs &args) const;
  Expr *undefinedArg(const Type *type, llvm::SMLoc loc);
  Expr *callFsStream(CallExpr *call, const FsStreamArgs &args, bool reading, llvm::SMLoc loc);
  ResultValuesIterator checkValuesIterator(Expr *source);

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
