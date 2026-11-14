#include "compiler/semantic/types_v2/intersections/types_intersections_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIntersection::fieldTypes(llvm::StringRef field, llvm::StringRef one,
                                              llvm::StringRef other) {
  return (llvm::Twine("field '") + field + "' has type '" + one +
          "' in one part of the intersection and '" + other + "' in another; a field has one type")
      .str();
}

std::string ErrorTextIntersection::notObject(llvm::StringRef type) {
  return (llvm::Twine("'&' joins object types into one with the fields of all, and '") + type +
          "' is not an object type; of TypeScript's other idioms with '&', 'K & string' "
          "(what of K is a string) and 'T & {}' (T without undefined) work")
      .str();
}

}
