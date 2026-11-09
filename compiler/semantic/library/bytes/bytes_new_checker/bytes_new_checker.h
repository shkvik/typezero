#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/bytes_new_checker/bytes_new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class ByteProofs;
class ExprChecker;
class IndexProofs;
class LiteralTypes;
class MemberChecker;
class TypeRelations;
class ArgumentChecker;
class IteratorMethods;
class GeneratorChecker;

class BytesNewChecker {
public:
  BytesNewChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkBytesNew(NewExpr *expr, const Type *type);
  Expr *checkTextDecoderParse(CallExpr *call, MemberExpr *member);
  Expr *checkBytesValues(CallExpr *call, ArrayLitExpr *lit, const Type *type);
  bool checkBytesSource(Expr *source, const Type *target);

private:
  using ResultDecoderLabel = Result<const char *, BytesNewCheckerDecoderLabelError>;
  using ResultViewedBuffer = Result<const Type *, BytesNewCheckerViewedBufferError>;

  static ResultViewedBuffer viewedBuffer(const Type *buffer);
  static ResultDecoderLabel decoderLabel(llvm::StringRef label);
  static std::optional<std::u16string> literalText(const Expr *e);
  static bool mayBeFatal(const Expr *options);

  SemanticState &state;
  Prelude *prelude = nullptr;
  ByteProofs *byteProofs = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  LiteralTypes *literalTypes = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
