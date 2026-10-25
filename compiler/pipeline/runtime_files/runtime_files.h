#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

// The files tz brings next to its executable: the runtime, the libraries of node:crypto, the
// texts of the licenses.
class RuntimeFiles {
public:
  explicit RuntimeFiles(const char *argv0);

  std::string path(llvm::StringRef name) const;

private:
  std::string dir_;
};

} // namespace tz
