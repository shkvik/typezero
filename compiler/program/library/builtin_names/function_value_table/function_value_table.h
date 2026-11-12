#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct FunctionValueGlobalInfo {
  GlobalName global;
  unsigned maxArgs;
};

struct FunctionValueStaticInfo {
  GlobalName owner;
  llvm::StringLiteral name;
  unsigned maxArgs;
  bool isReturningNothing;
};

class FunctionValueTable {
public:
  static constexpr unsigned kAnyArgs = ~0u;

  static const FunctionValueGlobalInfo *findGlobal(GlobalName global);
  static const FunctionValueStaticInfo *findStatic(GlobalName owner, llvm::StringRef name);
};

}
