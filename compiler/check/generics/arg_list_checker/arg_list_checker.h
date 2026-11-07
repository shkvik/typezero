#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ArgListChecker {
public:
  explicit ArgListChecker(Checker &owner);

  void expandArgumentList(FunctionDecl *fn, FuncSymbol *sym);
  void declareArgumentList(FunctionDecl *fn);
  Expr *listPart(Expr *expr);
  void expandListSpreads(std::vector<Expr *> &args, std::vector<bool> &spread);
  bool checkListArguments(CallExpr *call, size_t fixed, const Type *list, llvm::SMLoc loc,
                          const std::vector<bool> *checked);
  const Type *listElement(const Type *list);
  const Type *memberType(const Type *object, const Type *key);

private:
  static std::string utf8(const std::u16string &text);
  static bool usedAsValue(const Stmt *body, llvm::StringRef name, bool known);

  Checker &checker;
  llvm::DenseMap<const VarSymbol *, std::vector<llvm::StringRef>> argumentLists;
};

}
