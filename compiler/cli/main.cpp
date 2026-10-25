#include "compiler/cli/command_line/command_line.h"
#include "llvm/Support/InitLLVM.h"

int main(int argc, char **argv) {
  // Prints a stack trace if tz crashes and, on Windows, turns argv into UTF-8.
  llvm::InitLLVM initLLVM(argc, argv);
  tz::CommandLine commandLine;

  return commandLine.run(argc, argv);
}
