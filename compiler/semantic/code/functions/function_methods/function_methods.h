#pragma once

#include "compiler/program/library/function/function_method_table/function_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class FunctionMethods {
public:
  explicit FunctionMethods(SemanticState &owner);

  void findListPart(const FindListPart &task);
  void checkThisCall(const CheckThisCall &task);
  void checkListMethod(const CheckListMethod &task);
  void nextListArgument(const NextListArgument &task);
  void afterListArgument(const AfterListArgument &task);
  void checkListArguments(const CheckListArguments &task);

private:
  struct ExpandedArgs {
    std::vector<Expr *> args;
    std::vector<bool> spread;
    bool hasChanged;
  };

  using ResultListIsFilled = Result<const Type *, CodeFunctionsRegistryListFilledError>;
  using ResultArgumentFits = Result<const Expr *, CodeFunctionsRegistryListArgumentError>;
  using ResultJoinArityFits = Result<const CallExpr *, CodeFunctionsRegistryJoinArityError>;
  using ResultNoSpreadPassed = Result<const CallExpr *, CodeFunctionsRegistrySpreadFreeError>;
  using ResultThisMethodKnown = Result<FunctionMethod, CodeFunctionsRegistryCheckThisCallError>;
  using ResultThisArgumentsFit = Result<const CallExpr *, CodeFunctionsRegistryCheckThisCallError>;
  using ResultJoinMethodKnown =
      Result<const MemberExpr *, CodeFunctionsRegistryCheckListMethodError>;

  void finishListArguments(const ListArgumentsDraft &draft);
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  void addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded);
  void addTupleElements(const NameExpr *name, const Type *tuple, ExpandedArgs &expanded);
  void addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc, ExpandedArgs &expanded);


  bool isSpreadingLists(const Type *type);
  bool isFixedTuple(const Type *type) const;
  bool isChecked(const std::vector<bool> &checked, size_t index) const;


  ResultListIsFilled listFilled(const Type *list, llvm::ArrayRef<const Type *> parts,
                                size_t used) const;
  ResultArgumentFits listArgument(const Type *list, llvm::ArrayRef<const Type *> parts, size_t used,
                                  const Expr *arg, bool isSpread) const;


  ResultThisMethodKnown checkThisMethod(const MemberExpr *member) const;
  ResultJoinMethodKnown checkJoinMethod(const MemberExpr *member) const;
  ResultThisMethodKnown checkThisCallShape(const CallExpr *call, const MemberExpr *member) const;


  std::vector<const Type *> listParts(const Type *list) const;
  ResultJoinArityFits checkJoinArity(const CallExpr *call) const;
  ResultNoSpreadPassed checkSpreadFree(const CallExpr *call, size_t index) const;
  std::optional<std::vector<llvm::StringRef>> argumentListOf(const Expr *object) const;
  const Type *expectedPart(llvm::ArrayRef<const Type *> parts, size_t used, bool isSpread) const;
  ResultThisArgumentsFit checkThisArguments(const CallExpr *call, const MemberExpr *member,
                                            FunctionMethod method) const;

  SemanticState &state;
};

}
