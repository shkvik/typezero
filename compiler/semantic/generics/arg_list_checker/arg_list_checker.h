#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ExprChecker;
class FlowChecker;
class ClassDeclarer;
class TypeRelations;

class ArgListChecker {
public:
  static bool spreadsLists(const Type *type);
  ArgListChecker(SemanticState &owner, SemanticParts &all);

  void expandArgumentList(FunctionDecl *fn, FuncSymbol *sym);
  void declareArgumentList(FunctionDecl *fn);
  Expr *listPart(Expr *expr);
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  bool checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                          const std::vector<bool> *checked);
  const Type *listElement(const Type *list);
  const Type *memberType(const Type *object, const Type *key);

private:
  using ResultListIsSpread = Result<const FunctionDecl *, ArgListCheckerRestListValueError>;
  using ResultArgumentFits = Result<const Expr *, ArgListCheckerListArgumentError>;
  using ResultListIsFilled = Result<const Type *, ArgListCheckerListFilledError>;

  static ResultListIsSpread restListValue(const FunctionDecl *fn);
  static ResultArgumentFits listArgument(const Type *list, llvm::ArrayRef<const Type *> parts,
                                         size_t k, const Expr *arg, bool spreads);
  static ResultListIsFilled listFilled(const Type *list, llvm::ArrayRef<const Type *> parts,
                                       size_t k);
  static bool usedAsValue(const Stmt *body, llvm::StringRef name, bool known);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
};

}
