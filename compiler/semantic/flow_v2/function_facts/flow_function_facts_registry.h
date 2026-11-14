#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow_v2/function_facts/flow_function_facts_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class FlowFunctionFactsRegistry {
public:
  explicit FlowFunctionFactsRegistry(SemanticState &owner);

  void endPredicate(const EndPredicate &task);
  void beginPredicate(const BeginPredicate &task);
  void buildPredicate(const BuildPredicate &task);
  void notePredicateExit(const NotePredicateExit &task);
  void startPredicateCopy(const StartPredicateCopy &task);
  void addByteCallbackFacts(const AddByteCallbackFacts &task);
  void addIndexCallbackFacts(const AddIndexCallbackFacts &task);
  void checkByteCallbackResult(const CheckByteCallbackResult &task);

private:
  using ResultExitNarrowed = Result<const Flow *, FlowFunctionFactsRegistryNotePredicateExitError>;

  void addElementFacts(const FunctionDecl *functionDecl);
  void addIndexParamFacts(const FunctionDecl *functionDecl);


  bool isParamTerm(const FunctionDecl *functionDecl, const Term &term) const;


  const VarSymbol *paramVariable(const FunctionDecl *functionDecl, size_t index) const;
  const VarSymbol *predicatedVariable(const FunctionDecl *functionDecl, int param) const;


  ResultExitNarrowed checkPredicateExit(const Flow &whenTrue, const Flow &whenFalse);
  std::vector<const VarSymbol *> paramSymbols(const FunctionDecl *functionDecl) const;
  int paramPosition(const FunctionDecl *functionDecl, const VarSymbol *variable) const;
  PredicateSide projectedSide(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<int> projectedDefined(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<Facts::Fact> projectedFacts(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<std::pair<int, const Type *>> projectedNarrowed(const FunctionDecl *functionDecl,
                                                              const Flow &flow) const;
  std::vector<PredicateSide::NarrowedField>
  projectedNarrowedFields(const FunctionDecl *functionDecl, const Flow &flow) const;
  std::vector<std::pair<int, llvm::SmallVector<llvm::StringRef, 2>>>
  projectedFields(const FunctionDecl *functionDecl, const Flow &flow) const;

  SemanticState &state;
};

}
