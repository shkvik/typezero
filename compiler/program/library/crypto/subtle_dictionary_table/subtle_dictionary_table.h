#pragma once

#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct Dictionary {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> required;
};

struct SubtleDictionaryInfo {
  llvm::StringLiteral operation;
  llvm::StringLiteral algorithm;
  bool prefix;
  Dictionary dictionary;
};

class SubtleDictionaryTable {
public:
  static const SubtleDictionaryInfo *find(llvm::StringRef operation, llvm::StringRef algorithm);
};

}
