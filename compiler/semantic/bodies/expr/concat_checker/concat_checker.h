#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class DateChecker;
class BytesChecker;
class RegexChecker;
class TypeRelations;
class GenericChecker;

class ConcatChecker {
public:
  ConcatChecker(SemanticState &owner, SemanticParts &all);

  Expr *toStringOperand(Expr *expr);
  static std::string mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed);

private:
  using Parts = std::vector<std::pair<const Expr *, llvm::StringRef>>;
  using ResultConvertsToText = Result<const Expr *, ConcatCheckerConvertibleError>;

  static const Expr *unconverted(const Expr *expr);
  static ResultConvertsToText convertible(const Expr *expr);
  static std::optional<ConcatCheckerConvertibleErrorCode> textProblem(const Expr *expr);
  static bool hasOwnToString(const Type *base);
  static const Type *heldUnionMember(const Type *base);
  static std::string textProblemMessage(const Expr *expr,
                                        ConcatCheckerConvertibleErrorCode problem);
  static std::string ownStringMessage(const Expr *expr);
  static std::string dictMessage(const Type *base);
  static void concatParts(const BinaryExpr *bin, Parts &parts);
  static const BinaryExpr *leftConcat(const BinaryExpr *bin);
  static std::string concatTemplate(const Parts &parts, llvm::StringRef &only);
  static std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source,
                                                                  llvm::SMLoc op, size_t length);

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  GenericChecker *genericChecker = nullptr;
};

}
