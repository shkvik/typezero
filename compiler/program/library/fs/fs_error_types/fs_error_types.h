#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

class FsErrorTypes {
public:
  static llvm::StringRef verbOf(llvm::StringRef name);
  static std::vector<llvm::StringRef> getNodeFsNames();
  static std::vector<llvm::StringRef> getPromiseNames();
  static std::vector<llvm::StringRef> getTzFsNames();
};

}
