#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class CallRules {
public:
  explicit CallRules(SemanticState &owner);

  void startCalleeValueCall(CallExpr *call);
  void nextFunctionNameCall(const FunctionNameDraft &draft);
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);


  bool rejectSpread(CallExpr *call);
  bool isSpreadingLists(const Type *type) const;


  Expr *chainLink(Expr *object);

private:
  struct ExpandedArgs {
    std::vector<Expr *> args;
    std::vector<bool> spread;
    bool hasChanged;
  };

  using ResultNoSpreadPassed = Result<const CallExpr *, CodeCallsRegistrySpreadFreeError>;

  void addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded);
  void addTupleElements(const NameExpr *name, const Type *tuple, ExpandedArgs &expanded);
  void addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc, ExpandedArgs &expanded);


  bool isFixedTuple(const Type *type) const;


  Expr *shortCircuitLink(Expr *object, const Type *type);
  ResultNoSpreadPassed checkSpreadFree(const CallExpr *call, size_t index) const;

  SemanticState &state;
};

}
