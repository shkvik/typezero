#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include <string>
#include <vector>

namespace tz {

// The command line of tz, but for the commands of projects and packages (init, add...).
class Args {
public:
  enum class Command { Build, Run, Licenses, Help, Version };

  // Returns -1 if the arguments are complete, else the code tz should exit with.
  int parse(llvm::ArrayRef<const char *> args);
  // Where `tz build` writes: -o, or the name of the input with the extension of the target.
  std::string outputPath() const;

  static int usageError(const llvm::Twine &message);
  static void printUsage();

  Command command = Command::Build;
  std::string input;
  std::string output;
  // The name of the executable when it is not named after the input: the
  // project's, for a build of its "main".
  std::string outputStem;
  bool optimize = true;
  bool emitLLVM = false;
  bool explainParallel = false;
  std::vector<std::string> programArgs;
};

} // namespace tz
