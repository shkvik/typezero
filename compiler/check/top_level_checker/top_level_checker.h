#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TopLevelChecker {
public:
  explicit TopLevelChecker(Checker &checker);

  void checkTopLevel(Module *module, llvm::StringRef until = {});
  Checker::ScopeEntry lookupValue(llvm::StringRef name);
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  bool loadEarly(Module *source, llvm::StringRef name);
  bool reportNotLoaded(const NameExpr *name);
  static std::string readBeforeLoad(const Module *reader, const std::string &what,
                                    const Module *source, llvm::StringRef declared);
  bool reportImportAssign(const NameExpr *name, const VarSymbol *var);
  static std::string loadChain(const Module *reader, const Module *source);

private:
  Checker &checker_;
};

}
