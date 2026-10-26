#pragma once

#include "compiler/load/loader/loader.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryModules {
public:
  explicit LibraryModules(Loader &loader);

  Module *libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc);
  void importLibraryGlobals(Module *module, llvm::StringRef text);
  void importFsStreams(Module *module, llvm::StringRef text);

private:
  std::optional<SourceFile> parseLibraryFile(llvm::StringRef file, llvm::StringRef name,
                                             llvm::SMLoc loc, std::string &fullPath,
                                             std::vector<unsigned> &buffers);

  Loader &loader_;
};

}
