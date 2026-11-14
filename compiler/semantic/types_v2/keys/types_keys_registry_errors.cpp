#include "compiler/semantic/types_v2/keys/types_keys_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextKeys::keyOf(llvm::StringRef type) {
  return (llvm::Twine("'keyof' takes an object type, whose fields it names, not '") + type + "'")
      .str();
}

std::string ErrorTextKeys::noMember(llvm::StringRef object, llvm::StringRef key) {
  return (llvm::Twine("'") + object + "' has no member '" + key + "'").str();
}

}
