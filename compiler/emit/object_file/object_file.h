#pragma once

#include "llvm/Support/FileUtilities.h"
#include <memory>
#include <string>

namespace tz {

struct ObjectFile {
  std::string path;
  std::unique_ptr<llvm::FileRemover> remover;
};

}
