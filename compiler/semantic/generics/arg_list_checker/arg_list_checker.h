#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class PlacedNodes;
class Scopes;
class ExprChecker;
class FlowChecker;
class ClassDeclarer;
class TypeRelations;

class ArgListChecker {
public:
  static bool isSpreadingLists(const Type *type);
  ArgListChecker(SemanticState &owner, SemanticParts &all);

  Expr *listPart(Expr *expr);
  const Type *listElement(const Type *list);
  void declareArgumentList(FunctionDecl *functionDecl);
  const Type *memberType(const Type *object, const Type *key);
  void expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function);
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  bool checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                          const std::vector<bool> *checked);

private:
  enum class ListFit {
    FITS,
    UNTYPED,
    REPORTED,
  };

  struct ExpandedArgs {
    std::vector<Expr *> args;
    std::vector<bool> spread;
    bool hasChanged;
  };

  using ResultListIsSpread = Result<const FunctionDecl *, ArgListCheckerRestListValueError>;
  using ResultArgumentFits = Result<const Expr *, ArgListCheckerListArgumentError>;
  using ResultListIsFilled = Result<const Type *, ArgListCheckerListFilledError>;

  ResultListIsSpread restListValue(const FunctionDecl *functionDecl) const;
  ResultArgumentFits listArgument(const Type *list, llvm::ArrayRef<const Type *> parts, size_t used,
                                  const Expr *arg, bool isSpread) const;
  ResultListIsFilled listFilled(const Type *list, llvm::ArrayRef<const Type *> parts,
                                size_t used) const;

  bool isNumericKey(const Type *key) const;
  bool isFixedTuple(const Type *type) const;
  std::vector<const Type *> listParts(const Type *list) const;
  Param listRestParam(const Param &rest, const Type *more);
  bool isChecked(const std::vector<bool> *checked, size_t index) const;
  const Type *tupleFieldAt(const Type *tuple, double position) const;
  const Type *tupleMemberType(const Type *tuple, const Type *key) const;
  const Type *objectMemberType(const Type *object, const Type *key);
  Param listPartParam(const Param &rest, const Type *list, size_t index);
  std::optional<std::vector<llvm::StringRef>> argumentListOf(const Expr *object) const;
  void addExpandedArgument(Expr *arg, bool isSpreading, ExpandedArgs &expanded);
  void addSpreadUses(const Expr *expr, llvm::SmallPtrSetImpl<const Expr *> &uses) const;
  void addFixedUses(const Expr *expr, llvm::SmallPtrSetImpl<const Expr *> &uses) const;
  bool isUsedAsValue(const Stmt *body, llvm::StringRef name, bool isFixed) const;
  llvm::SmallPtrSet<const Expr *, 4> listUses(const PlacedNodes &nodes, bool isFixed) const;
  void addTupleElements(const NameExpr *name, const Type *tuple, ExpandedArgs &expanded);
  bool isExpandableRest(const FunctionDecl *functionDecl, const FuncSymbol *function) const;
  void addListParts(llvm::ArrayRef<llvm::StringRef> parts, llvm::SMLoc loc, ExpandedArgs &expanded);
  const Type *expectedPart(llvm::ArrayRef<const Type *> parts, size_t used, bool isSpread) const;
  void declareListValue(FunctionDecl *functionDecl, llvm::SMLoc loc,
                        llvm::ArrayRef<llvm::StringRef> parts,
                        llvm::ArrayRef<const Type *> elements);
  ListFit fitListArgument(CallExpr *call, size_t index, const Type *list,
                          llvm::ArrayRef<const Type *> parts, size_t used,
                          const std::vector<bool> *checked);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
