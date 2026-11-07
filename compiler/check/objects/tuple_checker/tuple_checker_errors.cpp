#include "compiler/check/objects/tuple_checker/tuple_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTupleJoin::notText(size_t index, llvm::StringRef tuple,
                                        llvm::StringRef element) {
  return (llvm::Twine("'join' turns each element into a string, and element ") +
          std::to_string(index) + " of '" + tuple + "' is '" + element +
          "', which cannot be one; build the text from the elements")
      .str();
}

std::string ErrorTextTupleMember::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextTupleMethod::fixedLen(llvm::StringRef name, llvm::StringRef text) {
  return (llvm::Twine("a tuple has a fixed length, and '") + name +
          "' would change it; to change the length, copy it into an array: '[..." + text + "]'")
      .str();
}

std::string ErrorTextTupleMethod::copyFirst(llvm::StringRef name, llvm::StringRef text) {
  return (llvm::Twine("'") + name +
          "' is not supported on tuples; copy the tuple into an array first: '[..." + text + "]." +
          name + "(...)'")
      .str();
}

std::string ErrorTextTupleMethod::destruct(llvm::StringRef name, llvm::StringRef text) {
  return (llvm::Twine("'") + name +
          "' is not supported on tuples; destructure it instead, e.g. 'const [a, b] = " + text +
          "'")
      .str();
}

std::string ErrorTextTupleLit::spread(llvm::StringRef tuple) {
  return (llvm::Twine("a tuple cannot take spread values: type '") + tuple +
          "' has a fixed length; write each element")
      .str();
}

std::string ErrorTextTupleLit::length(llvm::StringRef tuple, size_t want, size_t got) {
  return (llvm::Twine("type '") + tuple + "' has " + std::to_string(want) +
          " elements, and this literal has " + std::to_string(got))
      .str();
}

std::string ErrorTextTupleLit::element(size_t index) {
  return (llvm::Twine("element ") + std::to_string(index)).str();
}

}
