#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class Args;

class ScriptCommand {
public:
  explicit ScriptCommand(const char *argv0);

  int run(const Args &args) const;

private:
  const char *argv0_;
};

}
