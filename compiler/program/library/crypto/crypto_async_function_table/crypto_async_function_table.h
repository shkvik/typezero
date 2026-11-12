#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct CryptoAsyncFunctionInfo {
  llvm::StringLiteral name;
  llvm::StringLiteral syncName;
  NodeModule module;
};

class CryptoAsyncFunctionTable {
public:
  static const CryptoAsyncFunctionInfo *find(llvm::StringRef name);
};

}
