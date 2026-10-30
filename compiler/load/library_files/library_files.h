#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace tz {

struct LibraryFile {
  std::string tree;
  std::string file;
  std::string module;
};

class LibraryFiles {
public:
  static std::optional<LibraryFile> find(llvm::StringRef path);
  static std::string pathIn(llvm::StringRef tree, llvm::StringRef file);
};

}
