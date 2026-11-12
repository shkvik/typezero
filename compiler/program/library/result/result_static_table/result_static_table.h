#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ResultStatic {
  COMBINE,
  COMBINE_WITH_ALL_ERRORS,
};

struct ResultStaticInfo {
  llvm::StringLiteral name;
  ResultStatic value;
};

class ResultStaticTable {
public:
  static std::optional<ResultStatic> find(llvm::StringRef name);
};

}
