#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
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
  static NodeChildren<const Expr *> spreadOptions(const Expr *options);
  static bool nodeMayBeFatal(const Expr *options, std::vector<bool> &answers);
  static bool fatalFlag(const Expr *value);
  static Expr *madeBuiltin(CallExpr *call, Builtin builtin, const Type *result);
  Expr *reportBytesNew(NewExpr *expr, const std::string &message);
  void walkArgsLoosely(NewExpr *expr);
  bool hasWrongArity(CallExpr *call, unsigned min, unsigned max);
  void walkNonNegative(const Expr *value, const std::string &what);
  void walkView(CallExpr *call, int64_t elementSize, const std::string &what);
  Expr *checkArrayBufferNew(NewExpr *expr, CallExpr *call, const Type *type);
  Expr *checkDataViewNew(CallExpr *call, const Type *type);
  Expr *checkTextEncoderNew(CallExpr *call, const Type *type);
  Expr *checkTextDecoderNew(CallExpr *call, const Type *type);
  static const Type *decoderOptionsType();
  std::optional<Expr *> checkDecoderLabelArg(CallExpr *call, IntLitExpr *resolved);
  std::optional<std::u16string> knownDecoderName(const Expr *label, const std::u16string &text);
  bool decoderLabelsFit(const Expr *checked, llvm::SMLoc at);
  Expr *decoderNameLiteral(llvm::SMLoc loc, const std::u16string &name);
  Expr *checkTypedArrayNew(CallExpr *call, const Type *type);
  Expr *checkLengthNew(CallExpr *call, const Type *type);

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
