#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The files of a program: their scopes and exports (docs/modules.md).
class ModuleDeclarer {
public:
  explicit ModuleDeclarer(Checker &checker);

  // Makes `module` the file being checked; only at the top level, as scopes_
  // holds just the file's scope there.
  void useModule(Module *module);
  // Declares the types and functions of every file, and binds what files
  // import. Returns the top-level functions.
  std::vector<FunctionDecl *> declareModules(SourceFile &program);
  const llvm::StringMap<Checker::Export> &exportsOf(Module *module);
  // What `from`, imported by `path`, exports as `name.name`; reports a name it
  // does not export.
  std::optional<Checker::Binding> findExport(Module *from, llvm::StringRef path,
                                             const ModuleName &name);
  // What `name` means in the top-level scope of `module`: its own
  // declaration, or what it imports by that name.
  std::optional<Checker::Binding> resolveLocal(Module *module, llvm::StringRef name);

private:
  Checker &checker_;
};

} // namespace tz
