#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CallBindings {
public:
  CallBindings(SemanticState &owner, BindingRules &sharedBindingRules);

  void literalShape(const LiteralShape &task);
  void startBinding(const StartBinding &task);
  void bindGenericCall(const BindGenericCall &task);
  void checkGenericCall(const CheckGenericCall &task);
  void afterExplicitTypeArgs(const AfterExplicitTypeArgs &task);

private:
  using ResultTypeArgsCount = Result<size_t, GenericsInferenceRegistryTypeArgCountError>;
  using ResultArityMatches = Result<const CallExpr *, GenericsInferenceRegistryArityError>;
  using ResultSpreadIsFree = Result<const CallExpr *, GenericsInferenceRegistrySpreadFreeError>;
  using ResultNoClassReturn =
      Result<const FuncSymbol *, GenericsInferenceRegistryOwnClassReturnError>;

  void bindExplicitTypeArgs(GenericBindingDraft draft);


  bool returnsOwnClass(const FuncSymbol *generic, const Type *returnType);


  const ClassDecl *literalClassIn(const Type *type) const;
  const Type *listRestOf(const FuncSymbol *generic) const;
  ResultSpreadIsFree checkSpreadFree(const CallExpr *call, size_t index) const;
  ResultArityMatches checkArity(const CallExpr *call, unsigned min, unsigned max) const;
  ResultNoClassReturn ownClassReturn(const FuncSymbol *generic, const ClassDecl *classDecl) const;
  ResultTypeArgsCount typeArgCount(const FuncSymbol *generic,
                                   llvm::ArrayRef<TypeRef> typeArgs) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
