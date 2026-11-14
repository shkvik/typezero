#pragma once

#include "compiler/semantic/code/variables/code_variables_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class ValueDeclarators {
public:
  explicit ValueDeclarators(SemanticState &owner);

  void rememberValueDeclarator(const RememberValueDeclarator &task);

private:
  void rememberConstText(const DeclaratorDraft &draft);
  void rememberFixedLength(const DeclaratorDraft &draft);
  void rememberConstNumber(const DeclaratorDraft &draft);
  void rememberConstLiteral(const DeclaratorDraft &draft);
  void rememberResultVariable(const DeclaratorDraft &draft);
  void rememberReflection(VarDeclarator &declarator, const Expr *value);
  void rememberReflectedNames(VarDeclarator &declarator, const Expr *key);
  void markUnsharedArray(const VarDeclarator &declarator, const DeclaratorDraft &draft);


  bool isTextChoice(const Expr *expr) const;
  bool isFreshArray(const Expr *value) const;
  bool isExported(llvm::StringRef name) const;
  bool isLiteralValue(const Expr *value) const;
  bool isExportingName(const ExportDecl &exportDecl, llvm::StringRef name) const;


  std::vector<llvm::StringRef> reflectedNames(const Expr *key);
  std::vector<const Type *> keyMembers(const Type *keyType) const;
  std::vector<std::u16string> literalKeyTexts(const Expr *key) const;

  SemanticState &state;
};

}
