#pragma once

#include "compiler/program/ast/access/access.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct AccessKeywordInfo {
  llvm::StringLiteral name;
  Access access;
};

class AccessKeywordTable {
public:
  static llvm::StringRef getName(Access access);
};

}
