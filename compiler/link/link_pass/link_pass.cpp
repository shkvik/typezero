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

namespace {

// Whether `path` is a file that nothing forbids writing but that cannot be
// opened for it: in Windows, an executable that is running.
bool inUse(llvm::StringRef path) {
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

} // namespace

LinkPass::LinkPass(const Target &target, const BuildOptions &options)
    : target_(target), options_(options) {}

Errors LinkPass::run(const ObjectFile &object, const CryptoUse &crypto, bool parallel) const {
  Errors errors;
  const llvm::Triple &triple = target_.triple();
  std::string variant = parallel ? "tzrt_mt" : "tzrt";
  std::string runtime =
      options_.runtime.path(triple.isOSWindows() ? variant + ".lib" : "lib" + variant + ".a");
  if (!llvm::sys::fs::exists(runtime)) {
    errors.tool("the runtime is missing: " + runtime);
    return errors;
  }

  // node:crypto: the glue, and OpenSSL, all of it or only the objects its direct layer calls
  // (docs/crypto.md, section 3); TLS, with libssl (docs/net.md, section 4).
  std::vector<std::string> inputs = {object.path, runtime};
  if (crypto.any) {
    std::vector<const char *> libs = {"tzcrypto.lib"};
    if (crypto.tls) {
      libs.push_back("libssl.lib");
    }
    libs.push_back(crypto.full ? "libcrypto.lib" : "libcrypto_direct.lib");
    for (const char *lib : libs) {
      std::string path = options_.runtime.path(lib);
      if (!llvm::sys::fs::exists(path)) {
        errors.tool("the library of node:crypto is missing: " + path);
        return errors;
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
  if (!linkExecutable(triple, inputs, options_.output, messages)) {
    if (inUse(options_.output)) {
      errors.tool("cannot write '" + options_.output +
                  "': it is in use, probably still running; stop it and build again");
    } else {
      errors.raw(messages);
      errors.tool("linking failed");
    }
    return errors;
  }
  if (!messages.empty()) {
    errors.raw(messages, llvm::SourceMgr::DK_Warning);
  }

  return errors;
}

} // namespace tz
