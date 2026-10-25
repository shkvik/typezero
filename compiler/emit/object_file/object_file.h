#pragma once

#include "llvm/Support/FileUtilities.h"
#include <memory>
#include <string>

namespace tz {

// The object file of the program, a temporary file removed when this goes.
struct ObjectFile {
  std::string path;
  std::unique_ptr<llvm::FileRemover> remover;
};

} // namespace tz
