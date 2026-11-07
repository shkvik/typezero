#include "compiler/pipeline/check_program/topics/bytes/byte_proofs/byte_proofs_errors.h"

namespace tz {

std::string ErrorTextBytes::range(int64_t lo, int64_t hi) {
  return std::to_string(lo) + " to " + std::to_string(hi);
}

std::string ErrorTextByteFit::constant(int64_t value, llvm::StringRef what, llvm::StringRef range,
                                       int64_t stored, llvm::StringRef hint) {
  return std::to_string(value) + " does not fit in " + what.str() + " (" + range.str() +
         "): JavaScript would store " + std::to_string(stored) +
         "; write a value that fits, or cut it explicitly: '" + hint.str() + "'";
}

std::string ErrorTextByteFit::unproven(llvm::StringRef what, llvm::StringRef range,
                                       llvm::StringRef text, int64_t lo, int64_t hi,
                                       llvm::StringRef hint) {
  return "cannot prove that the value fits in " + what.str() + " (" + range.str() +
         "), and JavaScript would cut it without a word; check it first, e.g. 'if (" + text.str() +
         " >= " + std::to_string(lo) + " && " + text.str() + " <= " + std::to_string(hi) +
         ")', or cut it explicitly: '" + hint.str() + "'";
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
