#include "compiler/semantic/flow_v2/function_facts/flow_function_facts_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextPredicateExit::mayLie(llvm::StringRef function, llvm::StringRef var,
                                           llvm::StringRef type, llvm::StringRef shown) {
  return (llvm::Twine(function) + " says '" + var + " is " + type +
          "', but this may give true while '" + var + "' is only known to be '" + shown +
          "'; show it with what narrows types: a union's tag, 'typeof', "
          "'instanceof', '!= undefined', 'Array.isArray' or a function that tests "
          "it (checks of fields alone do not make an object of a type)")
      .str();
}

std::string ErrorTextByteView::offset(llvm::StringRef what, int64_t size) {
  return "the offset of " + what.str() + " must be a multiple of " + std::to_string(size) +
         ", written as a number; JavaScript throws a "
         "RangeError otherwise";
}

std::string ErrorTextByteView::size(llvm::StringRef what, int64_t size) {
  return "cannot prove that the size of the buffer is a multiple of " + std::to_string(size) +
         " for " + what.str() +
         ", and JavaScript would throw a RangeError; give the length: "
         "'new X(buffer, offset, length)'";
}

std::string ErrorTextByteView::end(llvm::StringRef offset, llvm::StringRef length, int64_t size) {
  return (offset.empty() ? std::string() : offset.str() + " + ") + length.str() +
         (size > 1 ? " * " + std::to_string(size) : "");
}

std::string ErrorTextByteView::notFits(llvm::StringRef what, llvm::StringRef end,
                                       llvm::StringRef buffer) {
  return "cannot prove that " + what.str() + " fits in the buffer, '" + end.str() +
         " <= " + buffer.str() +
         ".byteLength', and JavaScript would throw a RangeError; check it "
         "first, with the offset or the length written as a number";
}

std::string ErrorTextByteView::element(llvm::StringRef type) {
  return "an element of '" + type.str() + "'";
}

std::string ErrorTextByteView::offsetOf(llvm::StringRef what) {
  return "the offset of " + what.str();
}

std::string ErrorTextByteView::lengthOf(llvm::StringRef what) {
  return "the length of " + what.str();
}

}
