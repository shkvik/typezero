#include "compiler/cli/driver/driver.h"

#include "llvm/Support/InitLLVM.h"

int main(int argc, char **argv) {
  // Prints a stack trace if tz crashes and, on Windows, turns argv into UTF-8.
  llvm::InitLLVM initLLVM(argc, argv);
  return tz::runDriver(argc, argv);
}
