#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextCheckVisible::otherCase(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is declared in an earlier case of the 'switch'; declare it "
          "before the 'switch' to use it in several cases")
      .str();
}

}
