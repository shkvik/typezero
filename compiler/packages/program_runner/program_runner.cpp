#include "compiler/packages/program_runner/program_runner.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <vector>

namespace tz::pkg {

namespace fs = llvm::sys::fs;

int runProgram(llvm::StringRef program, llvm::ArrayRef<std::string> args, std::string &out,
               std::string &err, std::string &error) {
  llvm::SmallString<128> outFile, errFile;
  if (fs::createTemporaryFile("tz-out", "txt", outFile) ||
      fs::createTemporaryFile("tz-err", "txt", errFile)) {
    error = "cannot create a temporary file";
    return -1;
  }
  llvm::FileRemover removeOut(outFile), removeErr(errFile);
  std::vector<llvm::StringRef> argv{program};
  for (const std::string &arg : args) {
    argv.push_back(arg);
  }
  // No input: git must not wait for a password.
  std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outFile),
                                                llvm::StringRef(errFile)};
  int code = llvm::sys::ExecuteAndWait(program, argv, std::nullopt, redirects, 0, 0, &error);
  if (auto buffer = llvm::MemoryBuffer::getFile(outFile)) {
    out = (*buffer)->getBuffer().str();
  }
  if (auto buffer = llvm::MemoryBuffer::getFile(errFile)) {
    err = (*buffer)->getBuffer().str();
  }

  return code < 0 ? -1 : code;
}

} // namespace tz::pkg
