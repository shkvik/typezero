#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class Keyword {
  THIS,
  SUPER,
  ARGUMENTS,
  NULL_KEYWORD,
  UNDEFINED_KEYWORD,
  TRUE_KEYWORD,
  FALSE_KEYWORD,
  CONTINUE_KEYWORD,
};

struct KeywordInfo {
  llvm::StringLiteral name;
  Keyword value;
};

class KeywordTable {
public:
  static std::optional<Keyword> find(llvm::StringRef name);
  static llvm::StringRef getName(Keyword value);
};

}
