#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker_errors.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class CallChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class BytesChecker;
class MemberChecker;
class TypeRelations;
class ArgumentChecker;
class BytesNewChecker;
class CallbackChecker;
class IteratorMethods;
class GeneratorChecker;
class JsonStaticChecker;

class BufferStaticChecker {
public:
  BufferStaticChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  struct StaticCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    NameExpr *object = nullptr;
    const Type *type = nullptr;
  };

  using ResultSourceElement = Result<const Type *, BufferStaticCheckerSourceElementError>;
  using ResultConcatSources = Result<const Type *, BufferStaticCheckerConcatSourcesError>;
  using ResultMapResultFits = Result<const Type *, BufferStaticCheckerMapResultFitsError>;

  static ResultConcatSources concatSources(const Type *list, const Type *buffer);
  static ResultSourceElement sourceElement(const Type *from, llvm::StringRef what);
  static std::optional<std::u16string> literalText(const Expr *e);
  static ResultMapResultFits mapResultFits(const Type *type, bool arrow);
  static Signature mapperSignature(const Type *element, const Type *type);
  static Expr *toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result);
  static bool isBuffer(const Type *type);
  static bool mayBeBuffer(const Type *type);

  Expr *convertByteFill(Expr *fill);
  void walkAllocSize(CallExpr *call);
  Expr *checkBufferView(CallExpr *call);
  IntLitExpr *makeUtf8Literal(llvm::SMLoc loc);
  Expr *checkIsView(const StaticCall &bytes);
  Expr *checkTypedArrayOf(const StaticCall &bytes);
  Expr *checkTypedArrayFrom(const StaticCall &bytes);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  Expr *encodingOrUtf8(CallExpr *call, Expr *encoding);
  Expr *allocEncoding(CallExpr *call, const Type *fillType);
  Expr *reportBigIntArray(CallExpr *call, NameExpr *object);
  Expr *checkAlloc(CallExpr *call, MemberExpr *member);
  Expr *checkConcat(CallExpr *call, MemberExpr *member);
  Expr *checkIsBuffer(CallExpr *call, MemberExpr *member);
  Expr *makeBufferTest(CallExpr *call, MemberExpr *member);
  Expr *checkBufferFrom(CallExpr *call, MemberExpr *member);
  Expr *checkIsEncoding(CallExpr *call, MemberExpr *member);
  Expr *checkByteLength(CallExpr *call, MemberExpr *member);
  Expr *checkAllocUnsafe(CallExpr *call, MemberExpr *member);
  Expr *checkBufferCompare(CallExpr *call, MemberExpr *member);
  Expr *checkBufferFromString(CallExpr *call, MemberExpr *member);
  CallExpr *makeArrayFrom(CallExpr *call, Expr *source, const Type *type);
  void rememberByteCallback(const Expr *arg, const Type *from, const Type *type);
  Expr *reportStatic(CallExpr *call, MemberExpr *member, const std::string &message);
  void reportUnprovenBetween(const IndexProofs::ResultBetweenProven &between, llvm::SMLoc at);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  BytesChecker *bytesChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  BytesNewChecker *bytesNewChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  JsonStaticChecker *jsonStaticChecker = nullptr;
};

}
