#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextFsExport {
public:
  static std::string inTzFs(llvm::StringRef path, llvm::StringRef name);
  static std::string callback(llvm::StringRef name, llvm::StringRef path, llvm::StringRef sync);
  static std::string syncOnly(llvm::StringRef name, llvm::StringRef path, llvm::StringRef sync);
  static std::string waiting(llvm::StringRef path, llvm::StringRef name);
  static std::string readline(llvm::StringRef name, llvm::StringRef path);
  static std::string tzFs(llvm::StringRef path, llvm::StringRef name, bool inNodeFs);
  static std::string tzIo(llvm::StringRef path, llvm::StringRef name);
};

}
