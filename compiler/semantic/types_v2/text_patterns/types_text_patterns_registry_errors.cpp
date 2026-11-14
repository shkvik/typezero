#include "compiler/semantic/types_v2/text_patterns/types_text_patterns_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextHoleType::notHole(llvm::StringRef type) {
  return (llvm::Twine(
              "a template literal type holds strings, numbers, booleans and undefined, not '") +
          type + "'")
      .str();
}

std::string ErrorTextStringCount::tooMany(size_t most) {
  return (llvm::Twine("this template literal type makes more than ") + std::to_string(most) +
          " strings, the most TypeScript allows; use 'string'")
      .str();
}

}
