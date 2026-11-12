#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class UrlSchemeTable {
public:
  static bool isSpecial(llvm::StringRef scheme);
};

}
