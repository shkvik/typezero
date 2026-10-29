#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

namespace pkg {
struct Graph;
}

struct Program;
class SourceOverlay;

class LoadPass {
public:
  LoadPass(llvm::StringRef path, const pkg::Graph *packages,
           const SourceOverlay *overlay = nullptr);

  Errors run(Program &program) const;

private:
  std::string path_;
  const pkg::Graph *packages_;
  const SourceOverlay *overlay_;
};

}
