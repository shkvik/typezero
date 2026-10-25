#pragma once

#include "compiler/load/loader/loader.h"

namespace tz {

// The file an import names: a path relative to the importing file, a package by
// its name, read if it is new.
class ModuleResolver {
public:
  explicit ModuleResolver(Loader &loader);

  // The file an import names, read if it is new; null after reporting an
  // error.
  Module *resolve(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  // The file of a package an import names by the package's name.
  Module *resolvePackage(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  // The first of `candidates` (relative to `baseDir`) that exists, read if it
  // is new, as a file of package `package`; null after reporting that none
  // does.
  Module *load(llvm::StringRef baseDir, const std::vector<std::string> &candidates,
               llvm::StringRef spec, llvm::SMLoc loc, int package);

private:
  Loader &loader_;
};

} // namespace tz
