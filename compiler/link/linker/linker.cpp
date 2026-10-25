#include "compiler/link/linker/linker.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <vector>

LLD_HAS_DRIVER(coff)

namespace tz {

bool linkExecutable(const llvm::Triple &triple, llvm::ArrayRef<std::string> inputs,
                    llvm::StringRef output, std::string &errors) {
  if (!triple.isOSWindows()) {
    errors = "tz: linking for " + triple.str() + " is not supported yet\n";
    return false;
  }

  std::vector<std::string> args = {
      "lld-link",
      "/nologo",
      "/subsystem:console",
      "/incremental:no",
      // 8 MiB of stack for the main thread, as Linux gives by default
      // (Windows gives 1 MiB).
      "/stack:8388608",
      triple.getArch() == llvm::Triple::aarch64 ? "/machine:arm64" : "/machine:x64",
      "/out:" + output.str(),
  };
  args.insert(args.end(), inputs.begin(), inputs.end());
  // The runtime is built against the static CRT (/MT). LLD locates the MSVC
  // and Windows SDK libraries on its own.
  args.push_back("/defaultlib:libcmt");
  args.push_back("/defaultlib:kernel32");
  // For the developers of tz: a PDB next to the program, which
  // llvm-symbolizer reads (see TZ_CRASH_TRACE in runtime/core/output/output.c).
  if (std::getenv("TZ_LINK_DEBUG")) {
    args.push_back("/debug");
  }

  std::vector<const char *> argv;
  for (const std::string &arg : args) {
    argv.push_back(arg.c_str());
  }

  std::string messages;
  llvm::raw_string_ostream stdoutStream(messages);
  llvm::raw_string_ostream stderrStream(messages);
  // A fatal error of LLD (such as an output it cannot write) ends the process
  // at once, without its message, unless crash recovery brings it back here.
  llvm::CrashRecoveryContext::Enable();
  lld::Result result =
      lld::lldMain(argv, stdoutStream, stderrStream, {{lld::WinLink, &lld::coff::link}});
  llvm::CrashRecoveryContext::Disable();
  errors = std::move(messages);

  return result.retCode == 0;
}

} // namespace tz
