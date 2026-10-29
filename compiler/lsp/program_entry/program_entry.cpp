#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

std::string ProgramEntry::forDirectory(llvm::StringRef dir) {
  std::string manifestPath = pkg::findManifest(dir);
  if (manifestPath.empty()) {
    return "";
  }
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest) || manifest.main.empty()) {
    return "";
  }
  llvm::SmallString<256> main(manifest.dir);
  llvm::sys::path::append(main, manifest.main);
  if (!llvm::sys::fs::is_regular_file(main)) {
    return "";
  }

  return SourcePlaces::absolute(main);
}

std::string ProgramEntry::rootOf(llvm::StringRef entry) {
  llvm::SmallString<256> dir(SourcePlaces::absolute(entry));
  llvm::sys::path::remove_filename(dir);
  std::string manifest = pkg::findManifest(dir);
  if (!manifest.empty()) {
    return llvm::sys::path::parent_path(manifest).str();
  }

  return std::string(dir);
}

std::string ProgramEntry::forFile(llvm::StringRef file) {
  llvm::SmallString<256> dir(SourcePlaces::absolute(file));
  llvm::sys::path::remove_filename(dir);
  std::string main = forDirectory(dir);

  return main.empty() ? SourcePlaces::absolute(file) : main;
}

}
