#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ToUtf16 {
public:
  static std::u16string toUtf16(llvm::StringRef text);
};

}
