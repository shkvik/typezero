#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Imports to the exports they name.
class ImportBinder {
public:
  explicit ImportBinder(Checker &checker);

  void resolveImports(Module *module);
  // Binds the imports of module_ whose declarations are there: types and
  // functions at once, the variables of a file once its code is checked.
  void bindImports();
  // The variables some function of the importing file uses through its
  // imports must be globals.
  void noteImportsInFunctions(SourceFile &program);
  // The top-level function of some file that `name` calls in the body of
  // `fn` (see findCycles).
  FunctionDecl *cycleCallee(FunctionDecl *fn, llvm::StringRef name);

private:
  Checker &checker_;
};

} // namespace tz
