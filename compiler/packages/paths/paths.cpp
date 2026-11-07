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

std::string Paths::canonical(llvm::StringRef dir) {
  llvm::SmallString<256> real;
  if (fs::real_path(dir, real)) {
    real = dir;
    fs::make_absolute(real);
    path::remove_dots(real, true);
  }

  return std::string(real);
}

std::string Paths::findAbove(llvm::StringRef dir, llvm::StringRef name) {
  llvm::SmallString<256> current(dir);
  if (current.empty()) {
    fs::current_path(current);
  }
  fs::make_absolute(current);
  path::remove_dots(current, true);
  while (!current.empty()) {
    llvm::SmallString<256> candidate(current);
    path::append(candidate, name);
    if (fs::is_regular_file(candidate)) {
      return std::string(candidate);
    }
    llvm::StringRef parent = path::parent_path(current);
    if (parent == current) {
      break;
    }
    current = parent;
  }

  return "";
}

}
