#pragma once

#include "compiler/load/loader/loader.h"

namespace tz {

class ModuleResolver {
public:
  explicit ModuleResolver(Loader &loader);

  Module *resolve(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  Module *resolvePackage(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  Module *load(llvm::StringRef baseDir, const std::vector<std::string> &candidates,
               llvm::StringRef spec, llvm::SMLoc loc, int package);

private:
  static std::string foreignFile(llvm::StringRef dir, llvm::StringRef file, llvm::StringRef spec);
  const std::string *realFile(llvm::StringRef full);

  Loader &loader;
  llvm::StringMap<std::string> realFiles;
};

}
