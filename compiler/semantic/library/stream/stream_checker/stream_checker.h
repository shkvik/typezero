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
class ClassDeclarer;
class EmitterChecker;
class ArgumentChecker;
class SetEncodingChecker;

class StreamChecker {
public:
  StreamChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg);
  const Type *streamCallbackType();
  enum class ChunkSide {
    PUSHED,
    WRITTEN,
  };

  enum class ChunkState {
    UNCHECKED,
    CHECKED,
  };

  Expr *streamChunkArg(Expr *arg, const Type *chunk, ChunkSide side, llvm::StringRef method,
                       ChunkState chunkState);
  Expr *checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);

private:
  using ResultStreamSideOk = Result<const Type *, StreamCheckerStreamSideError>;
  using ResultChunkIsGiven = Result<const Expr *, StreamCheckerChunkNotUndefinedError>;
  using ResultOverrideArgs = Result<const ClassDecl *, StreamCheckerOverridesArgsError>;
  using ResultMethodExists = Result<const FunctionDecl *, StreamCheckerOverriddenMethodError>;

  ResultOverrideArgs checkOverridesArgs(const ClassDecl *classDecl,
                                        const StringLitExpr *literal) const;
  ResultMethodExists checkOverriddenMethod(const ClassDecl *classDecl,
                                           llvm::ArrayRef<FunctionDecl *> methods,
                                           llvm::StringRef method) const;
  bool isChunkValue(bool isOnlyStrings, const Type *candidate) const;
  unsigned chunkKind(const Type *typeArg) const;
  ResultChunkIsGiven checkChunkNotUndefined(const Expr *arg, ChunkSide side,
                                            llvm::StringRef method) const;
  ResultStreamSideOk checkStreamSide(const Type *chunk, const Type *receiver, llvm::StringRef name,
                                     bool reading) const;

  bool isByteChunk(const Type *type) const;

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

  bool isNativeIo(Builtin builtin) const;
  void walkArgsFrom(CallExpr *call, size_t from);
  Expr *checkStreamEnd(StreamCall &stream);
  Expr *checkStreamRead(StreamCall &stream);
  Expr *checkStreamWrite(StreamCall &stream);
  Expr *reportPipe(CallExpr *call, MemberExpr *member);
  bool isCallbackArg(StreamCall &stream, Expr *&arg);
  Expr *undefinedOf(llvm::SMLoc loc, const Type *type);
  Expr *typeNameLiteral(CallExpr *call, const Type *typeArg);
  const Type *writtenCallbackType(const ClassDecl *classDecl);
  Expr *checkOverridesIntrinsic(CallExpr *call, NameExpr *name);
  Expr *checkChunkIntrinsic(CallExpr *call, const Type *typeArg);
  Expr *encodingArg(const StreamCall &stream, Expr *arg);
  Expr *checkNativeIoIntrinsic(CallExpr *call, const Type *typeArg);
  NativeIoSignature nativeIoSignature(Builtin builtin, const Type *typeArg);
  Expr *checkedAs(const StreamCall &stream, Expr *arg, const Type *type);
  Expr *finishStreamCall(StreamCall &stream, std::vector<Expr *> args,
                         llvm::StringRef implementation);
  Expr *checkStreamData(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                        StreamMethod method);

  SemanticState &state;
  Prelude *prelude = nullptr;
  FsChecker *fsChecker = nullptr;
  ChunkTypes *chunkTypes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  SetEncodingChecker *setEncodingChecker = nullptr;
};

}
