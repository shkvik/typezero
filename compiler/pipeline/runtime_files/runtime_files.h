#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class RuntimeFiles {
public:
  explicit RuntimeFiles(const char *argv0);

  std::string path(llvm::StringRef name) const;

private:
  std::string dir_;
};

}
