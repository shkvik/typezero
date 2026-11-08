#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextHeldPlace {
public:
  static std::string moduleVar();
  static std::string newObject();
  static std::string array();
  static std::string unknownFn();
  static std::string storedBy(llvm::StringRef name);
  static std::string outerVar();
  static std::string container();
  static std::string field();
};

}
