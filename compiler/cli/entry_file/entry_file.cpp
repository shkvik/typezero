#include "compiler/cli/entry_file/entry_file.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/packages/manifest/manifest.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

int EntryFile::find(Args &args) const {
  if (!args.input.empty()) {
    return -1;
  }
  std::string manifestPath = pkg::findManifest("");
  if (manifestPath.empty()) {
    return Args::usageError(args.programArgs.empty()
                                ? "no source file given, and there is no tz.json here or above"
                                : "'" + args.programArgs.front() +
                                      "' is not a .tz file, and there is no tz.json here or above "
                                      "whose \"main\" would run");
  }
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest)) {
    return kExitError;
  }
  if (manifest.main.empty()) {
    llvm::errs() << "tz: " << manifestPath
                 << " has no \"main\": add \"main\": \"main.tz\", or name the file to build\n";
    return kExitError;
  }
  llvm::SmallString<256> main(manifest.dir);
  llvm::sys::path::append(main, manifest.main);
  args.input = std::string(main);
  llvm::StringRef name = manifest.name;
  name = name.substr(name.rfind('/') + 1);
  if (!name.empty()) {
    args.outputStem = name.str();
  }

  return -1;
}

}
