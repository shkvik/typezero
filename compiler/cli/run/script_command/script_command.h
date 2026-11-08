#pragma once

#include "llvm/ADT/StringRef.h"

namespace tz {

class Args;

class ScriptCommand {
public:
  explicit ScriptCommand(const char *argv0);

  int run(const Args &args) const;

private:
  static std::string shellWord(llvm::StringRef arg);
  static void addToPath(llvm::StringRef dir);
  static int spawn(const std::string &command);
  const char *argv0;
};

}
