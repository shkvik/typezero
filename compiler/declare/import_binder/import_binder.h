#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ImportBinder {
public:
  explicit ImportBinder(Checker &checker);

  void resolveImports(Module *module);
  void bindImports();
  void noteImportsInFunctions(SourceFile &program);
  FunctionDecl *cycleCallee(FunctionDecl *fn, llvm::StringRef name);

private:
  Checker &checker_;
};

}
