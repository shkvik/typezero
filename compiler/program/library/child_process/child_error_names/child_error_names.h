#pragma once

#include "compiler/program/library/child_process/child_function_table/child_function_table.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct ChildErrorInfo {
  llvm::StringLiteral name;
  ChildFunction function;
  bool isShell;
  bool isText;
};

struct ChildResultInfo {
  llvm::StringLiteral name;
  bool isText;
};

class ChildErrorNames {
public:
  static std::vector<llvm::StringRef> getTypeNames();
  static llvm::StringRef getErrorName(ChildFunction function, bool isText, bool isShell);
  static llvm::StringRef getResultName(bool isText);
};

}
