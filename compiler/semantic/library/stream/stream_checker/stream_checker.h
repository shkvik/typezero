#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class FsChecker;
class ChunkTypes;
class CallChecker;
class ExprChecker;
class FlowChecker;
class ResultChecker;
class TypeRelations;
class EmitterChecker;
class ArgumentChecker;
class SetEncodingChecker;

class StreamChecker {
public:
  StreamChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg);
  const Type *streamCallbackType();
  Expr *streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing, llvm::StringRef method,
                       bool checked);
  Expr *checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  using ResultStreamSideOk = Result<const Type *, StreamCheckerStreamSideError>;
  using ResultChunkIsGiven = Result<const Expr *, StreamCheckerChunkNotUndefinedError>;
  using ResultOverrideArgs = Result<const ClassDecl *, StreamCheckerOverridesArgsError>;
  using ResultMethodExists = Result<const FunctionDecl *, StreamCheckerOverriddenMethodError>;

  static ResultOverrideArgs overridesArgs(const ClassDecl *cls, const StringLitExpr *lit);
  static ResultMethodExists overriddenMethod(const ClassDecl *cls,
                                             llvm::ArrayRef<FunctionDecl *> methods,
                                             llvm::StringRef method);
  static ResultChunkIsGiven chunkNotUndefined(const Expr *arg, bool eof, llvm::StringRef method);
  static ResultStreamSideOk streamSide(const Type *chunk, const Type *receiver,
                                       llvm::StringRef name, bool reading);

  static bool isByteChunk(const Type *type);

  SemanticState &state;
  Prelude *prelude = nullptr;
  FsChecker *fsChecker = nullptr;
  ChunkTypes *chunkTypes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  SetEncodingChecker *setEncodingChecker = nullptr;
};

}
