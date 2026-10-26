#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include <string>
#include <vector>

namespace tz {

class Args {
public:
  enum class Command { Build, Run, Licenses, Help, Version };

  int parse(llvm::ArrayRef<const char *> args);
  std::string outputPath() const;

  static int usageError(const llvm::Twine &message);
  static void printUsage();

  Command command = Command::Build;
  std::string input;
  std::string output;
  std::string outputStem;
  bool optimize = true;
  bool emitLLVM = false;
  bool explainParallel = false;
  std::vector<std::string> programArgs;
};

}
