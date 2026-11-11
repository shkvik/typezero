#pragma once

#include "compiler/program/library/stream/stream_method_table/stream_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_checker/stream_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/SmallPtrSet.h"

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

  struct StreamCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    ClassDecl *cls = nullptr;
    StreamMethod method = StreamMethod::PUSH;
    const Type *chunk = nullptr;
    llvm::SmallPtrSet<const Expr *, 4> checked;
  };

  struct NativeIoSignature {
    std::vector<const Type *> params;
    const Type *result = nullptr;
  };

  static bool isNativeIo(Builtin builtin);
  void walkArgsFrom(CallExpr *call, size_t from);
  Expr *checkStreamEnd(StreamCall &stream);
  Expr *checkStreamRead(StreamCall &stream);
  Expr *checkStreamWrite(StreamCall &stream);
  Expr *reportPipe(CallExpr *call, MemberExpr *member);
  bool isCallbackArg(StreamCall &stream, Expr *&arg);
  Expr *undefinedOf(llvm::SMLoc loc, const Type *type);
  Expr *typeNameLiteral(CallExpr *call, const Type *typeArg);
  const Type *writtenCallbackType(const ClassDecl *cls);
  Expr *checkOverridesIntrinsic(CallExpr *call, NameExpr *name);
  Expr *checkChunkIntrinsic(CallExpr *call, const Type *typeArg);
  Expr *encodingArg(const StreamCall &stream, Expr *arg);
  Expr *checkNativeIoIntrinsic(CallExpr *call, const Type *typeArg);
  NativeIoSignature nativeIoSignature(Builtin builtin, const Type *typeArg);
  Expr *checkedAs(const StreamCall &stream, Expr *arg, const Type *type);
  Expr *finishStreamCall(StreamCall &stream, std::vector<Expr *> args, llvm::StringRef impl);
  Expr *checkStreamData(CallExpr *call, MemberExpr *member, ClassDecl *cls, StreamMethod method);

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
