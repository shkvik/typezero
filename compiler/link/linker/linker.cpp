#include "compiler/link/linker/linker.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <vector>

LLD_HAS_DRIVER(coff)

namespace tz {

bool linkExecutable(const llvm::Triple &triple, llvm::ArrayRef<std::string> inputs,
                    llvm::StringRef output, bool debug, llvm::StringRef natvis,
                    std::string &errors) {
  if (!triple.isOSWindows()) {
    errors = "tz: linking for " + triple.str() + " is not supported yet\n";
    return false;
  }

  std::vector<std::string> args = {
      "lld-link",
      "/nologo",
      "/subsystem:console",
      "/incremental:no",
      "/stack:1073741824",
      triple.getArch() == llvm::Triple::aarch64 ? "/machine:arm64" : "/machine:x64",
      "/out:" + output.str(),
  };
  args.insert(args.end(), inputs.begin(), inputs.end());
  args.push_back("/defaultlib:libcmt");
  args.push_back("/defaultlib:kernel32");
  if (std::getenv("TZ_LINK_DEBUG")) {
    args.push_back("/debug");
  } else if (debug) {
    args.push_back("/debug");
    args.push_back("/opt:ref");
    args.push_back("/opt:icf");
    args.push_back("/pdbaltpath:%_PDB%");
    if (!natvis.empty()) {
      args.push_back("/natvis:" + natvis.str());
    }
  }

  std::vector<const char *> argv;
  for (const std::string &arg : args) {
    argv.push_back(arg.c_str());
  }

  std::string messages;
  llvm::raw_string_ostream stdoutStream(messages);
  llvm::raw_string_ostream stderrStream(messages);
  llvm::CrashRecoveryContext::Enable();
  lld::Result result =
      lld::lldMain(argv, stdoutStream, stderrStream, {{lld::WinLink, &lld::coff::link}});
  llvm::CrashRecoveryContext::Disable();
  errors = std::move(messages);

  return result.retCode == 0;
}

}
