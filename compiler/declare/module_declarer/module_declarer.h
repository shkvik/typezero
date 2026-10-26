#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ModuleDeclarer {
public:
  explicit ModuleDeclarer(Checker &checker);

  void useModule(Module *module);
  std::vector<FunctionDecl *> declareModules(SourceFile &program);
  const llvm::StringMap<Checker::Export> &exportsOf(Module *module);
  std::optional<Checker::Binding> findExport(Module *from, llvm::StringRef path,
                                             const ModuleName &name);
  std::optional<Checker::Binding> resolveLocal(Module *module, llvm::StringRef name);

private:
  Checker &checker_;
};

}
