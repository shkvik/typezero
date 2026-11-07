#include "compiler/declare/module_declarer/module_declarer_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextFindExport::missing(llvm::StringRef path, llvm::StringRef name) {
  return (llvm::Twine("'") + path + "' has no export named '" + name + "'").str();
}

std::string ErrorTextFindExport::library(llvm::StringRef path, llvm::StringRef name,
                                         llvm::StringRef hint) {
  return (llvm::Twine("'") + path + "' has no export named '" + name + "'" + hint).str();
}

std::string ErrorTextFindExport::unmarked(llvm::StringRef path, llvm::StringRef name,
                                          llvm::StringRef file) {
  return (llvm::Twine("'") + path + "' has no export named '" + name +
          "'; add 'export' to its declaration in " + file)
      .str();
}

std::string ErrorTextExportSource::neverthrow() {
  return "Result, ok and err are built in: remove the export from 'neverthrow'";
}

std::string ErrorTextExportSource::builtin(llvm::StringRef path) {
  return (llvm::Twine("a built-in module is not re-exported: import what a file needs from '") +
          path + "' in that file")
      .str();
}

std::string ErrorTextLocalExport::builtin(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' comes from a built-in module, which is not re-exported: import it from there in "
          "the file that uses it")
      .str();
}

std::string ErrorTextLocalExport::notFound(llvm::StringRef name) {
  return (llvm::Twine("cannot find '") + name +
          "' to export: this file neither declares nor imports it")
      .str();
}

std::string ErrorTextFreeExport::previous() {
  return "previously exported here";
}

std::string ErrorTextFreeExport::duplicate(llvm::StringRef alias) {
  return (llvm::Twine("duplicate export of '") + alias + "'").str();
}

std::string ErrorTextStarExport::twice(llvm::StringRef name, llvm::StringRef first,
                                       llvm::StringRef second) {
  return (llvm::Twine("'") + name + "' is exported both by '" + first + "' and by '" + second +
          "' through 'export *'; re-export the one you mean by name, e.g. 'export { " + name +
          " } from \"" + first + "\"'")
      .str();
}

}
