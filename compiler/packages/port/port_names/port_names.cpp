#include "compiler/packages/port/port_names/port_names.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace path = llvm::sys::path;

std::string capitalize(llvm::StringRef s) {
  std::string out = s.str();
  if (!out.empty()) {
    out[0] = llvm::toUpper(out[0]);
  }

  return out;
}

std::string derivedName(llvm::StringRef file, llvm::StringRef packageDir,
                        llvm::StringRef packageName) {
  std::string stem = path::stem(file).str();
  llvm::StringRef dir = path::parent_path(file);
  if (stem == "index" || stem == "mod" || stem == "main") {
    llvm::StringRef parent = path::filename(dir);
    stem =
        dir == packageDir || parent == "src" || parent == "lib" ? packageName.str() : parent.str();
  }
  std::string name;
  bool upper = false;
  for (char c : stem) {
    if (!llvm::isAlnum(c)) {
      upper = !name.empty();
      continue;
    }
    name += upper ? llvm::toUpper(c) : c;
    upper = false;
  }
  if (name.empty() || llvm::isDigit(name[0])) {
    name = "_" + name;
  }

  return name;
}

} // namespace tz::pkg
