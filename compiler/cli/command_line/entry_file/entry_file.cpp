#include "compiler/cli/command_line/entry_file/entry_file.h"
#include "compiler/cli/command_line/args/args.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/tz_config/tz_config.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

int EntryFile::takeOutputDir(Args &args) {
  if (args.command != Args::Command::Build || !args.output.empty()) {
    return -1;
  }
  llvm::SmallString<256> dir(args.input);
  llvm::sys::path::remove_filename(dir);
  std::string configPath = pkg::TzConfigFile::findTzConfig(dir);
  if (configPath.empty()) {
    return -1;
  }
  pkg::TzConfig config;
  if (!pkg::TzConfigFile::readTzConfig(configPath, config)) {
    return kExitError;
  }
  args.outputDir = config.outDir;

  return -1;
}

int EntryFile::find(Args &args) const {
  if (!args.input.empty()) {
    return EntryFile::takeOutputDir(args);
  }
  std::string manifestPath = pkg::ManifestFile::findManifest("");
  if (manifestPath.empty()) {
    return Args::usageError(args.programArgs.empty()
                                ? "no source file given, and there is no package.json here or "
                                  "above"
                                : "'" + args.programArgs.front() +
                                      "' is not a .tz file nor a script of package.json, and "
                                      "there is no package.json here or above whose \"main\" "
                                      "would run");
  }
  pkg::Manifest manifest;
  if (!pkg::ManifestFile::readManifest(manifestPath, manifest)) {
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

  return EntryFile::takeOutputDir(args);
}

}
