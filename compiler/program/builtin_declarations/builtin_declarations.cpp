#include "compiler/program/builtin_declarations/builtin_declarations.h"

namespace tz {

void BuiltinDeclarations::setItems(BuiltinText text, std::vector<Stmt *> parsed) {
  this->items[text] = std::move(parsed);
}

llvm::ArrayRef<Stmt *> BuiltinDeclarations::getItems(BuiltinText text) const {
  std::map<BuiltinText, std::vector<Stmt *>>::const_iterator found = this->items.find(text);
  if (found == this->items.end()) {
    return {};
  }

  return found->second;
}

}
