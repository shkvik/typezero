#include "compiler/pipeline/check_program/types/template_types/template_types_errors.h"
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
