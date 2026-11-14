#pragma once

#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class ArgumentInference {
public:
  ArgumentInference(SemanticState &owner, BindingRules &sharedBindingRules);

  void retryDeferred(const RetryDeferred &task);
  void inferFromArgument(const InferFromArgument &task);
  void inferFromListRest(const InferFromListRest &task);
  void nextBindingArgument(const NextBindingArgument &task);
  void nextPrecheckedField(const NextPrecheckedField &task);
  void afterPrecheckedField(const AfterPrecheckedField &task);
  void inferPrecheckedField(const InferPrecheckedField &task);

private:
  void settleArguments(GenericBindingDraft draft);
  void inferFromExpected(GenericBindingDraft &draft);
  void settleWithCandidates(GenericBindingDraft &draft);
  void finishInferredArguments(GenericBindingDraft draft);
  void deferLiteral(GenericBindingDraft draft, size_t index);
  void inferFromCheckedArgument(GenericBindingDraft draft, size_t index);
  void walkArgumentNow(GenericBindingDraft draft, size_t index, const Type *hint);
  void inferFromLiteralShape(GenericBindingDraft draft, size_t index, const Type *shape);
  void inferFromLiteralText(GenericBindingDraft draft, size_t index, const StringLitExpr *text);
  void inferArgument(CandidateInference &inference, std::optional<ArgumentStrength> strength,
                     bool isIntLiteral, const Type *pattern, const Type *actual) const;


  bool isBareGenericName(Expr *arg) const;
  bool isMakingResults(const Expr *arg) const;
  bool isNeedingContext(const Expr *arg) const;
  bool isContextSensitive(const Expr *arg) const;
  bool hasSpecialProperty(const ObjectLitExpr *literal) const;
  bool hasUntypedParam(const FunctionDecl *functionDecl) const;
  bool isPrecheckingFields(const ObjectLitExpr *literal, const Type *pattern) const;
  bool isPrecheckable(const Field *field, const Expr *value,
                      llvm::ArrayRef<const Type *> genericTypeParams) const;


  const Type *listRestArguments(const GenericBindingDraft &draft) const;
  const Type *expectedCandidate(const CandidateInference &fromExpected, size_t index) const;


  GenericArgumentForm argumentForm(const GenericBindingDraft &draft, size_t index);
  size_t nextPrecheckable(const ObjectLitExpr *object, const Type *target, size_t from,
                          llvm::ArrayRef<const Type *> params) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
