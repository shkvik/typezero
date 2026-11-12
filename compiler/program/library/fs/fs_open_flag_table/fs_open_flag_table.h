#pragma once

#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct FsDefaultFlagInfo {
  FsFunction function;
  llvm::StringLiteral flag;
};

class FsOpenFlagTable {
public:
  static bool isKnown(llvm::StringRef flag);
  static llvm::StringRef getDefault(FsFunction function);
};

}
