#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct PendingExportInfo {
  NodeModule module;
  llvm::StringLiteral name;
};

class PendingExportTable {
public:
  static bool isListed(NodeModule module, llvm::StringRef name);
};

}
