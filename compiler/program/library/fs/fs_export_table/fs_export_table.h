#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class FsExport {
  FUNCTION,
  HANDLE_FUNCTION,
  VALUE,
  IN_TZ_FS,
  NOT_YET,
};

struct FsExportInfo {
  NodeModule module;
  llvm::StringLiteral name;
  FsExport kind;
};

class FsExportTable {
public:
  static std::optional<FsExport> find(NodeModule module, llvm::StringRef name);
  static bool isFunction(NodeModule module, llvm::StringRef name);
};

}
