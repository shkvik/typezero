#pragma once

#include "compiler/pipeline/runtime_files/runtime_files.h"
#include <string>

namespace tz {

namespace pkg {
struct Graph;
}

struct BuildOptions {
  std::string input;
  std::string output;
  bool optimize = true;
  bool emitLLVM = false;
  const pkg::Graph *packages = nullptr;
  RuntimeFiles runtime;
};

} // namespace tz
