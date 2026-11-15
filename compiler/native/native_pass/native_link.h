#pragma once

#include "llvm/Support/SMLoc.h"
#include <string>
#include <vector>

namespace tz {

struct ForeignFunction {
  std::string name;
  llvm::SMLoc loc;
};

struct NativeLicense {
  std::string name;
  std::string license;
  std::string text;
};

struct NativeLink {
  std::vector<std::string> inputs;
  std::vector<std::string> dlls;
  std::vector<NativeLicense> licenses;
};

}
