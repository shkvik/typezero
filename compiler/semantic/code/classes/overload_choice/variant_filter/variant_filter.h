#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/code/classes/overload_choice/choice_rules/choice_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"
#include <string>
#include <vector>

namespace tz {

class VariantFilter {
public:
  VariantFilter(SemanticState &owner, ChoiceRules &sharedChoiceRules);

  void chooseVariant(const ChooseVariant &task);

private:
  using ResultChoiceAllowed = Result<void, CodeClassesRegistryChooseVariantError>;
  using ResultVariantsFound =
      Result<std::vector<FunctionDecl *>, CodeClassesRegistryChooseVariantError>;

  void walkLoosely(std::vector<Expr *> &args);


  bool hasFloatLiteral(const Expr *expr) const;
  bool isPlainObjectParam(const Type *param) const;
  bool isTakingCount(const FuncSymbol *function, size_t count) const;


  const Type *commonParameter(llvm::ArrayRef<FunctionDecl *> fit, size_t index) const;
  const Type *soleObjectShape(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                              size_t index) const;
  const Type *expectedArgument(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                               size_t index) const;
  const Type *decimalForLiteral(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                size_t index) const;


  ResultChoiceAllowed checkNoSpread(llvm::StringRef what, const std::vector<bool> &spread) const;
  ResultVariantsFound checkFittingVariants(llvm::StringRef what,
                                           llvm::ArrayRef<FunctionDecl *> variants,
                                           size_t count) const;


  std::string variantsName(const FunctionDecl *first) const;
  VariantChoice uncheckedChoice(FunctionDecl *variant) const;
  std::vector<const Type *> objectShapes(llvm::ArrayRef<FunctionDecl *> fit, size_t index) const;

  SemanticState &state;
  ChoiceRules &choiceRules;
};

}
