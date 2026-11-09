#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker_errors.h"
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
  using ResultSourceElement = Result<const Type *, BufferStaticCheckerSourceElementError>;
  using ResultConcatSources = Result<const Type *, BufferStaticCheckerConcatSourcesError>;

  static ResultConcatSources concatSources(const Type *list, const Type *buffer);
  static ResultSourceElement sourceElement(const Type *from, llvm::StringRef what);
  static std::optional<std::u16string> literalText(const Expr *e);

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
