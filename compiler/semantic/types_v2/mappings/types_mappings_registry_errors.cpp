#include "compiler/semantic/types_v2/mappings/types_mappings_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextMappings::rename(llvm::StringRef name) {
  return (llvm::Twine("a mapped type renames its keys to names of fields, string literal "
                      "types such as `get${Capitalize<K>}`, not '") +
          name + "'")
      .str();
}

std::string ErrorTextMappings::keys(llvm::StringRef key) {
  return (llvm::Twine("the keys of a mapped type are names of fields, string literal types "
                      "such as '\"id\" | \"name\"' or 'keyof T', not '") +
          key + "'")
      .str();
}

}
