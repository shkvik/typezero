#include "compiler/declare/import_binder/import_binder_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

}
