#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextCryptoExport {
public:
  static std::string webcrypto(llvm::StringRef name, llvm::StringRef path);
  static std::string subtle(llvm::StringRef name, llvm::StringRef path);
};

}
