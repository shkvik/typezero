#include "compiler/semantic/declaration/modules/declaration_modules_registry_errors.h"
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

std::string ErrorTextWholeImport::byDefault(llvm::StringRef path) {
  return (llvm::Twine("default imports are not supported: there is no 'export default'; import "
                      "the names you need, e.g. 'import { EventEmitter, once } from \"") +
          path + "\"'")
      .str();
}

std::string ErrorTextWholeImport::star(llvm::StringRef path) {
  return (llvm::Twine("'import * as' is not supported; import the names you need, e.g. 'import "
                      "{ EventEmitter, once } from \"") +
          path + "\"'")
      .str();
}

std::string ErrorTextFreeName::previous() {
  return "previously imported here";
}

std::string ErrorTextFreeName::duplicate(llvm::StringRef alias) {
  return (llvm::Twine("duplicate import of '") + alias + "'").str();
}

std::string ErrorTextFreeName::declared(llvm::StringRef alias) {
  return (llvm::Twine("'") + alias + "' is declared here").str();
}

std::string ErrorTextFreeName::conflicts(llvm::StringRef alias, llvm::StringRef name,
                                         llvm::StringRef path) {
  return (llvm::Twine("import '") + alias + "' conflicts with the declaration of '" + alias +
          "' in this file; import it under another name, e.g. 'import { " + name + " as " + name +
          "2 } from \"" + path + "\"'")
      .str();
}

std::string ErrorTextNotLoaded::whileLoads(llvm::StringRef reader, llvm::StringRef what,
                                           llvm::StringRef source, llvm::StringRef chain,
                                           llvm::StringRef declared) {
  return (llvm::Twine(reader) + " reads " + what + " while it loads, but " + source +
          " is not loaded yet: " + chain + "; compute the value inside a function, or move '" +
          declared + "' to a file outside the cycle")
      .str();
}

std::string ErrorTextNotLoaded::beforeCall(llvm::StringRef what, llvm::StringRef reader,
                                           llvm::StringRef early) {
  return (llvm::Twine(what) + " has no type yet when " + reader + " first calls '" + early +
          "', which needs to know what '" + early +
          "' returns: the files import each other; declare the return type of '" + early + "'")
      .str();
}

std::string ErrorTextNotLoaded::inFunction(llvm::StringRef what, llvm::StringRef reader) {
  return (llvm::Twine(what) + " has no type yet where " + reader +
          " creates this function: the files import each other; declare the "
          "function with 'function' at the top level of the file, which is "
          "checked after every file")
      .str();
}

std::string ErrorTextNotLoaded::label(llvm::StringRef name, llvm::StringRef declared,
                                      llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' ('" + declared + "' of " + file + ")").str();
}

std::string ErrorTextNotLoaded::labelOf(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' of " + file).str();
}

std::string ErrorTextNotLoaded::quoted(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextImportSource::neverthrow() {
  return "Result, ok and err are built in: remove the import from 'neverthrow'";
}

}
