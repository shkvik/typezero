#pragma once

#include "compiler/packages/locked/locked.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>

namespace tz::pkg {

struct Lock {
  std::map<std::string, Locked> packages;
};

class LockFile {
public:
  static std::string lockText(const Lock &lock);
  static bool readLock(llvm::StringRef file, Lock &out);
};

}
