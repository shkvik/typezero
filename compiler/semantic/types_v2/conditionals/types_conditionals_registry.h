#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

struct SpreadRef {
  const TypeRef *ref;
  bool isSpread;
};

class TypesConditionalsRegistry {
public:
  explicit TypesConditionalsRegistry(SemanticState &owner);

  void closeConditionalScope();
  void chooseBranch(const ChooseBranch &task);
  void evalFiltered(const EvalFiltered &task);
  void finishBranches(const FinishBranches &task);
  void finishFiltered(const FinishFiltered &task);
  void evalConditional(const EvalConditional &task);
  void startConditional(const StartConditional &task);
  void prepareExpectation(const PrepareExpectation &task);
  void afterEvalConditional(const AfterEvalConditional &task);
  void afterInferConstraint(const AfterInferConstraint &task);
  void afterConditionalCheck(const AfterConditionalCheck &task);
  void afterDistributedMember(const AfterDistributedMember &task);
  void afterConditionalPattern(const AfterConditionalPattern &task);

private:
  void nextInferConstraint(ConditionalDraft draft);
  void pushBranch(const TypeRef &branch, BranchCall call);
  void nextDistributedMember(const DistributionDraft &draft);
  void noteInfer(SpreadRef part, std::vector<InferRef> &infers) const;
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  void startBranchChoice(const ConditionalDraft &draft, BranchCall call);
  void pushExpectations(llvm::ArrayRef<const Type *> branches, unsigned depth);
  void pushSpreadArgs(const TypeRef &ref, std::vector<SpreadRef> &stack) const;
  void bindInferred(const ConditionalDraft &draft, const InferMatch &inference);
  void startDistribution(const TypeRef &ref, std::vector<const Type *> members);
  void enterConditionalScope(const ConditionalInfo &info, llvm::ArrayRef<const Type *> operands);


  bool isBinding(const Stmt *decl) const;
  bool isDistributing(const TypeRef &ref) const;
  bool isSpreadAt(const TypeRef &ref, size_t index) const;


  const Type *deferConditional(const TypeRef &ref);
  const Type *boundType(llvm::StringRef name) const;
  const Type *boundOrDefault(const Type *bound, const Type *infer) const;


  std::string spellingOf(const TypeRef &ref) const;
  std::vector<InferRef> infersIn(const TypeRef &ref) const;
  const ConditionalInfo *knownConditional(const TypeRef &ref);
  TypeAliasDecl *genericAliasNamed(const TypeRef &branch) const;
  std::vector<llvm::StringRef> namesIn(const TypeRef &ref) const;
  std::vector<const Type *> bindInferParams(llvm::ArrayRef<InferRef> inferRefs);

  SemanticState &state;
};

}
