#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class BindingRules {
public:
  explicit BindingRules(SemanticState &owner);

  void finishBinding(GenericBindingDraft draft);
  void pushLookups(const PendingLookups &pending);
  void pushBound(llvm::ArrayRef<const Type *> bound);
  void deliverFailure(const GenericBindingDraft &draft);
  void settleTypeParam(GenericBindingDraft &draft, size_t index);
  void takeLookups(CandidateInference &inference, PendingLookups &pending);


  bool hasLookups(const PendingLookups &pending) const;


  const Type *literalShapeOf(const Expr *expr) const;
  const Type *parameterFor(const GenericBindingDraft &draft, size_t index) const;


  PendingLookups missingLookups(const CandidateInference &inference) const;

private:
  using ResultCandidateJoin = Result<const Type *, GenericsInferenceRegistryCandidatesJoinError>;
  using ResultCandidateType = Result<const Type *, GenericsInferenceRegistryJoinCandidatesError>;

  bool hasGaps(const ArrayLitExpr *literal) const;
  bool hasArgumentErrors(const GenericBindingDraft &draft) const;
  bool isOtherNumber(const Type *type, const Type *candidate) const;
  bool hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;


  const Type *nodeShape(const Expr *expr, std::vector<const Type *> &answers) const;
  const Type *arrayShape(const ArrayLitExpr *literal, std::vector<const Type *> &answers) const;
  const Type *objectShape(const ObjectLitExpr *literal, std::vector<const Type *> &answers) const;
  const Type *withUndefined(const CandidateInference &inference, size_t index,
                            const Type *type) const;
  const Type *widenToConstraint(const GenericBindingDraft &draft, size_t index,
                                const Type *type) const;


  ResultCandidateType joinCandidates(const GenericBindingDraft &draft, size_t index) const;
  ResultCandidateType joinStrongCandidates(const GenericBindingDraft &draft, size_t index) const;
  ResultCandidateType joinWeakCandidates(const GenericBindingDraft &draft, size_t index,
                                         const Type *strong) const;
  ResultCandidateType joinLiteralCandidates(const GenericBindingDraft &draft, size_t index,
                                            const Type *known) const;


  NodeChildren<const Expr *> shapeParts(const Expr *expr) const;
  ResultCandidateJoin candidatesJoin(const FuncSymbol *generic, const Type *param, const Type *type,
                                     const Type *candidate) const;

  SemanticState &state;
};

}
