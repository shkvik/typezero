#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class JsonChecker;
class SemanticParts;
class DateChecker;
class BytesChecker;
class RegexChecker;
class TypeRelations;
class ClassDeclarer;
class GenericChecker;
class UnknownChecker;

class ConcatChecker {
public:
  ConcatChecker(SemanticState &owner, SemanticParts &all);

  Expr *toStringOperand(Expr *expr);
  std::string mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed) const;

private:
  using Parts = std::vector<std::pair<const Expr *, llvm::StringRef>>;
  using ResultConvertsToText = Result<const Expr *, ConcatCheckerConvertibleError>;

  struct TemplateText {
    std::string text;
    llvm::StringRef only;
  };

  const Expr *unconverted(const Expr *expr) const;
  Parts concatParts(const BinaryExpr *binary) const;
  const BinaryExpr *leftConcat(const BinaryExpr *binary) const;
  TemplateText concatTemplate(const Parts &parts) const;
  std::string escapedBody(llvm::StringRef text) const;
  std::string templateChar(llvm::StringRef body, size_t index) const;
  ResultConvertsToText checkConvertible(const Expr *expr) const;
  std::optional<ConcatCheckerConvertibleErrorCode> textProblem(const Expr *expr) const;
  bool hasOwnToString(const Type *base) const;
  const Type *heldUnionMember(const Type *base) const;
  bool isHeldMember(const Type *member) const;
  std::string textProblemMessage(const Expr *expr, ConcatCheckerConvertibleErrorCode problem) const;
  std::string unionMemberMessage(const Type *type, const Type *base) const;
  std::string ownStringMessage(const Expr *expr) const;
  std::string dictMessage(const Type *base) const;
  llvm::StringRef dictKind(const Type *base) const;
  Expr *taggedText(Expr *expr);
  std::string sumHint(const Parts &parts) const;
  bool isTextValue(const Expr *expr) const;

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericChecker *genericChecker = nullptr;
  UnknownChecker *unknownChecker = nullptr;
};

}
