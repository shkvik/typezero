#pragma once

#include "compiler/program/errors/errors.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

namespace pkg {
struct Graph;
}

struct Program;

class LoadPass {
public:
  LoadPass(llvm::StringRef path, const pkg::Graph *packages);

  Errors run(Program &program) const;

private:
  std::string path_;
  const pkg::Graph *packages_;
};

} // namespace tz
