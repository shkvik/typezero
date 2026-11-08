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
    this->final = final.str();
    if (std::error_code ec = fs::create_directories(path::parent_path(final))) {
      error = "cannot create '" + path::parent_path(final).str() + "': " + ec.message();
      return false;
    }
    if (std::error_code ec = fs::createUniqueDirectory(final + ".tmp", this->temp)) {
      error = "cannot create a directory next to '" + this->final + "': " + ec.message();
      return false;
    }

    return true;
  }
  llvm::StringRef path() const {
    return this->temp;
  }
  bool commit(std::string &error) {
    std::error_code ec = fs::rename(this->temp, this->final);
    if (!ec) {
      this->temp.clear();
      return true;
    }
    if (fs::is_directory(this->final)) {
      return true;
    }
    error =
        "cannot move '" + std::string(this->temp) + "' to '" + this->final + "': " + ec.message();

    return false;
  }
  ~StagedDir() {
    if (!this->temp.empty()) {
      fs::remove_directories(this->temp, true);
    }
  }

private:
  std::string final;
  llvm::SmallString<256> temp;
};

}
