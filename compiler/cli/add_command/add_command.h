#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

struct Manifest;
struct Dependency;

class AddCommand {
public:
  int run(llvm::ArrayRef<const char *> args) const;

private:
  static std::string relativePath(llvm::StringRef from, llvm::StringRef to);
  static bool isAbsolutePath(llvm::StringRef text);
  static bool interpretAdd(const Manifest &project, llvm::StringRef arg, Dependency &out);
};

}
