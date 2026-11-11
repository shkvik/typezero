#pragma once

#include "compiler/program/builtin_declarations/builtin_declarations.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct ErrorExportInfo {
  NodeModule module;
  std::vector<llvm::StringRef> (*names)();
};

class ErrorExportTable {
public:
  static llvm::ArrayRef<BuiltinText> getTexts();
  static llvm::ArrayRef<ErrorExportInfo> getExports();
};

}
