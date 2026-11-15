#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextVariance {
public:
  static std::string takes(llvm::StringRef method, llvm::StringRef name);
  static std::string settable(llvm::StringRef field);
  static std::string baseTakes(llvm::StringRef name);
  static std::string sampleName();
  static std::string noExtends(llvm::StringRef own, llvm::StringRef wide, llvm::StringRef example);
  static std::string wider(llvm::StringRef generic, llvm::StringRef name, llvm::StringRef writer);
};

}
