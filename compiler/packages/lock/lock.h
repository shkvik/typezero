#pragma once

#include "compiler/packages/locked/locked.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>

namespace tz::pkg {

// tz.lock: the packages a program uses, by id.
struct Lock {
  std::map<std::string, Locked> packages; // by id
};

// Reads tz.lock, reporting its errors; a missing file is an empty lock.
bool readLock(llvm::StringRef file, Lock &out);
// The text of tz.lock.
std::string lockText(const Lock &lock);

} // namespace tz::pkg
