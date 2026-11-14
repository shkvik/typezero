#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/concatenation/code_concatenation_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class CodeConcatenationRegistry {
public:
  explicit CodeConcatenationRegistry(SemanticState &owner);

  void markTextCast();
  void toStringOperand();
  void toStringSlot(const ToStringSlot &task);
  void trackMixedConcat(const TrackMixedConcat &task);
  void checkConcatenation(const CheckConcatenation &task);

private:
  struct TemplateText {
    std::string text;
    llvm::StringRef only;
  };

  using Parts = std::vector<std::pair<const Expr *, llvm::StringRef>>;
  using ResultConvertsToText = Result<const Expr *, CodeConcatenationRegistryConvertibleError>;

  bool isTextValue(const Expr *expr) const;
  bool hasOwnToString(const Type *base) const;
  bool isHeldMember(const Type *member) const;
  bool isHoldingJson(const Type *type) const;
  bool isJoiningAsText(const Type *type) const;
  bool isHoldingUnknown(const Type *type) const;


  std::string sumHint(const Parts &parts) const;
  std::string dictMessage(const Type *base) const;
  std::string escapedBody(llvm::StringRef text) const;
  std::string ownStringMessage(const Expr *expr) const;
  std::string templateChar(llvm::StringRef body, size_t index) const;
  std::string unionMemberMessage(const Type *type, const Type *base) const;
  std::string mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed) const;
  std::string textProblemMessage(const Expr *expr,
                                 CodeConcatenationRegistryConvertibleErrorCode problem) const;


  ClassDecl *classOf(const Type *type) const;
  const Expr *unconverted(const Expr *expr) const;
  llvm::StringRef dictKind(const Type *base) const;
  Parts concatParts(const BinaryExpr *binary) const;
  const Type *heldUnionMember(const Type *base) const;
  TemplateText concatTemplate(const Parts &parts) const;
  const BinaryExpr *leftConcat(const BinaryExpr *binary) const;
  ResultConvertsToText checkConvertible(const Expr *expr) const;
  std::optional<CodeConcatenationRegistryConvertibleErrorCode> textProblem(const Expr *expr) const;

  SemanticState &state;
};

}
