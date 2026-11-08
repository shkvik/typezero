#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker_errors.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::string ErrorTextNodeImport::conflict(llvm::StringRef alias, llvm::StringRef name,
                                          llvm::StringRef path) {
  return "import '" + alias.str() + "' conflicts with the declaration of '" + alias.str() +
         "' in this file; import it under another name, e.g. 'import { " + name.str() + " as " +
         name.str() + "2 } from \"" + path.str() + "\"'";
}

std::string ErrorTextNodeImport::declared(llvm::StringRef alias) {
  return "'" + alias.str() + "' is declared here";
}

std::string ErrorTextNodeImport::duplicate(llvm::StringRef alias) {
  return "duplicate import of '" + alias.str() + "'";
}

std::string ErrorTextNodeImport::list(llvm::ArrayRef<llvm::StringRef> used) {
  std::string names;
  for (llvm::StringRef name : used) {
    names += (names.empty() ? "" : ", ") + name.str();
  }

  return names;
}

std::string ErrorTextNodeImport::form(bool isDefault) {
  return isDefault ? "default imports are not supported: there is no 'export default'"
                   : "'import * as' is not supported";
}

std::string ErrorTextNodeImport::process(llvm::StringRef form) {
  return form.str() + "; 'process' is global, as in Node, so the import is "
                      "not needed: drop it";
}

std::string ErrorTextNodeImport::buffer(llvm::StringRef form, llvm::StringRef path) {
  return form.str() +
         "; 'Buffer' is global, as in Node: import it as "
         "'import { Buffer } from \"" +
         path.str() + "\"', or drop the import";
}

std::string ErrorTextNodeImport::uses(llvm::StringRef form, llvm::StringRef names,
                                      llvm::StringRef path) {
  return form.str() + "; import the names this file uses: 'import { " + names.str() + " } from \"" +
         path.str() + "\"'";
}

std::string ErrorTextNodeImport::typical(llvm::StringRef form, llvm::StringRef name,
                                         llvm::StringRef path) {
  return form.str() + "; import the names you need, e.g. 'import { " + name.str() + " } from \"" +
         path.str() + "\"'";
}

std::string ErrorTextNodeValue::notValue(llvm::StringRef label, bool path,
                                         llvm::StringRef typical) {
  return "this is not a value: the functions of '" + label.str() +
         "' are used by their names, e.g. '" +
         (path ? std::string("posix.join(a, b)'") : typical.str() + "(...)'");
}

std::string ErrorTextNodeValue::onlyCalled(llvm::StringRef name, llvm::StringRef label) {
  return "'" + name.str() + "' of '" + label.str() +
         "' can only be called; where a function is expected, pass an "
         "arrow that calls it, e.g. 'x => " +
         name.str() + "(x)'";
}

std::string ErrorTextNodeValue::notFunc(llvm::StringRef name, llvm::StringRef label) {
  return "'" + name.str() + "' is not a function: call the functions of '" + label.str() +
         "' by their names";
}

}
