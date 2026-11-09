#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <cstdint>

namespace tz {

RuntimeFiles::RuntimeFiles(const char *argv0) {
  std::string self =
      llvm::sys::fs::getMainExecutable(argv0, (void *)(intptr_t)&RuntimeFiles::anchor);
  this->dir = llvm::sys::path::parent_path(self).str();
}

void RuntimeFiles::anchor() {}

std::string RuntimeFiles::path(llvm::StringRef name) const {
  llvm::SmallString<256> path(this->dir);
  llvm::sys::path::append(path, name);

  return std::string(path);
}

}
