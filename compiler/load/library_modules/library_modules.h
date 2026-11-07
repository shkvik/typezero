#pragma once

#include "compiler/load/loader/loader.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

inline constexpr llvm::StringLiteral kSubtleName = "%subtle";
inline constexpr llvm::StringLiteral kIntlLocaleName = "%IntlLocale";

class LibraryModules {
public:
  explicit LibraryModules(Loader &loader);

  Module *libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc);
  void importLibraryGlobals(Module *module, llvm::StringRef text);
  void importFsStreams(Module *module, llvm::StringRef text);
  void importProcessSignals(Module *module, llvm::StringRef text);

private:
  static void appendPart(SourceFile &file, SourceFile &part);
  std::optional<SourceFile> parseLibraryFile(llvm::StringRef file, llvm::StringRef name,
                                             llvm::SMLoc loc, std::string &fullPath,
                                             std::vector<unsigned> &buffers);

  Loader &loader_;
};

}
