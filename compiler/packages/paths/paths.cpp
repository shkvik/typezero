#include "compiler/packages/paths/paths.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

std::string canonical(llvm::StringRef dir) {
  llvm::SmallString<256> real;
  if (fs::real_path(dir, real)) {
    real = dir;
    fs::make_absolute(real);
    path::remove_dots(real, true);
  }

  return std::string(real);
}

}
