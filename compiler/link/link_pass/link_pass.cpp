#include "compiler/link/link_pass/link_pass.h"
#include "compiler/emit/object_file/object_file.h"
#include "compiler/link/linker/linker.h"
#include "compiler/optimize/target/target.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include <string>
#include <vector>

namespace tz {

LinkPass::LinkPass(const Target &target, const BuildOptions &options)
    : target(target),
      options(options) {}

bool LinkPass::inUse(llvm::StringRef path) {
  namespace fs = llvm::sys::fs;
  if (!fs::is_regular_file(path) || fs::access(path, fs::AccessMode::Write)) {
    return false;
  }
  int fd;
  std::error_code ec = fs::openFileForWrite(path, fd, fs::CD_OpenExisting, fs::OF_Append);
  if (!ec) {
    llvm::sys::Process::SafelyCloseFileDescriptor(fd);
    return false;
  }

  return ec == std::errc::permission_denied;
}

LinkPass::ResultLinked LinkPass::run(const ObjectFile &object, const CryptoUse &crypto,
                                     bool parallel) const {
  std::vector<Diagnostic> errors;
  const llvm::Triple &triple = this->target.triple();
  std::string variant = parallel ? "tzrt_mt" : "tzrt";
  std::string runtime =
      this->options.runtime.path(triple.isOSWindows() ? variant + ".lib" : "lib" + variant + ".a");
  if (!llvm::sys::fs::exists(runtime)) {
    std::string message = "the runtime is missing: " + runtime;
    errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
    return err(errors);
  }

  std::vector<std::string> inputs = {object.path, runtime};
  if (crypto.any) {
    std::vector<const char *> libs = {"tzcrypto.lib"};
    if (crypto.tls) {
      libs.push_back("libssl.lib");
    }
    libs.push_back(crypto.full ? "libcrypto.lib" : "libcrypto_direct.lib");
    for (const char *lib : libs) {
      std::string path = this->options.runtime.path(lib);
      if (!llvm::sys::fs::exists(path)) {
        std::string message = "the library of node:crypto is missing: " + path;
        errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
        return err(errors);
      }
      inputs.push_back(path);
    }
    if (crypto.full) {
      for (const char *lib : {"ws2_32", "advapi32", "crypt32", "user32"}) {
        inputs.push_back(std::string("/defaultlib:") + lib);
      }
    }
  }

  std::string messages;
  std::string natvis;
  if (this->options.debug == DebugLevel::Full) {
    natvis = this->options.runtime.path("tz.natvis");
    if (!llvm::sys::fs::exists(natvis)) {
      std::string message = "the debugger views of tz are missing: " + natvis;
      errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
      return err(errors);
    }
  }
  if (!Linker::linkExecutable(triple, inputs, this->options.output,
                              this->options.debug != DebugLevel::None, natvis, messages)) {
    if (LinkPass::inUse(this->options.output)) {
      std::string message = "cannot write '" + this->options.output +
                            "': it is in use, probably still running; stop it and build again";
      errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
    } else {
      errors.push_back({.form = Diagnostic::Form::Raw, .message = messages});
      errors.push_back({.form = Diagnostic::Form::Tool, .message = "linking failed"});
    }
    return err(errors);
  }
  std::vector<Diagnostic> warnings;
  if (!messages.empty()) {
    warnings.push_back(
        {.kind = llvm::SourceMgr::DK_Warning, .form = Diagnostic::Form::Raw, .message = messages});
  }

  return ok(warnings);
}

}
