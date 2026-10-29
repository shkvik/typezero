#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz {

class McpCommand {
public:
  explicit McpCommand(const char *argv0);

  int run(llvm::ArrayRef<const char *> args) const;

private:
  const char *argv0_;
};

}
