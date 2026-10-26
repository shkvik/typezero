#pragma once

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <string>
#include <system_error>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

class StagedDir {
public:
  bool create(llvm::StringRef final, std::string &error) {
    final_ = final.str();
    if (std::error_code ec = fs::create_directories(path::parent_path(final))) {
      error = "cannot create '" + path::parent_path(final).str() + "': " + ec.message();
      return false;
    }
    if (std::error_code ec = fs::createUniqueDirectory(final + ".tmp", temp_)) {
      error = "cannot create a directory next to '" + final_ + "': " + ec.message();
      return false;
    }

    return true;
  }
  llvm::StringRef path() const { return temp_; }
  bool commit(std::string &error) {
    std::error_code ec = fs::rename(temp_, final_);
    if (!ec) {
      temp_.clear();
      return true;
    }
    if (fs::is_directory(final_)) {
      return true;
    }
    error = "cannot move '" + std::string(temp_) + "' to '" + final_ + "': " + ec.message();

    return false;
  }
  ~StagedDir() {
    if (!temp_.empty()) {
      fs::remove_directories(temp_, true);
    }
  }

private:
  std::string final_;
  llvm::SmallString<256> temp_;
};

}
