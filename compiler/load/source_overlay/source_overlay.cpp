#include "compiler/load/source_overlay/source_overlay.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

}

void SourceOverlay::set(llvm::StringRef path, llvm::StringRef text) {
  texts_[key(path)] = text.str();
}

const std::string *SourceOverlay::find(llvm::StringRef path) const {
  if (texts_.empty()) {
    return nullptr;
  }
  auto it = texts_.find(key(path));

  return it == texts_.end() ? nullptr : &it->second;
}

std::string SourceOverlay::key(llvm::StringRef file) {
  llvm::SmallString<256> full(file);
  fs::make_absolute(full);
  path::remove_dots(full, true);
  path::native(full);
  llvm::SmallString<256> real;
  if (!fs::real_path(full, real)) {
    full = real;
  }
  if (path::is_style_windows(path::Style::native)) {
    return full.str().lower();
  }

  return std::string(full);
}

}
