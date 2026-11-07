#include "compiler/pipeline/check_program/topics/bytes/typed_array_checker/typed_array_checker_errors.h"

namespace tz {

std::string ErrorTextTypedArray::mapFits(llvm::StringRef what, llvm::StringRef range,
                                         llvm::StringRef hint) {
  return "cannot prove that what the function returns fits in " + what.str() + " (" + range.str() +
         "), and JavaScript would cut it without a word; pass an arrow that cuts it "
         "explicitly, e.g. 'x => " +
         hint.str() + "'";
}

std::string ErrorTextTypedArray::flatMap() {
  return "typed arrays have no 'flatMap'; spread the elements into an "
         "array first, e.g. '[...u].flatMap(f)'";
}

std::string ErrorTextTypedArray::iterator() {
  return "'set' takes an array or a typed array; collect the values "
         "of the iterator first, e.g. '[...it]'";
}

std::string ErrorTextTypedArray::index(llvm::StringRef array) {
  return "cannot prove that the index is within the array, and JavaScript "
         "would throw a RangeError; check it first, e.g. 'if (i >= 0 && i < " +
         array.str() + ".length)'";
}

std::string ErrorTextTypedArray::locale() {
  return "'toLocaleString' of a typed array is not supported; join the "
         "elements with 'join()', or format each with toLocaleString";
}

std::string ErrorTextTypedArray::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextTypedArray::offsetOf(llvm::StringRef name) {
  return "the offset of '" + name.str() + "'";
}

std::string ErrorTextTypedArray::setOffset() {
  return "the offset of 'set'";
}

std::string ErrorTextTypedArray::setFits(llvm::StringRef offset, llvm::StringRef source,
                                         llvm::StringRef target) {
  return "cannot prove that the values fit, '" +
         (offset.empty() ? std::string() : offset.str() + " + ") + source.str() + ".length" +
         " <= " + target.str() +
         ".length', and JavaScript would throw a RangeError; "
         "check it first, with the offset written as a number";
}

}
