#include "compiler/semantic/library/bytes/buffer_checker/buffer_checker_errors.h"

namespace tz {

std::string ErrorTextBuffer::valueArg(llvm::StringRef name, llvm::StringRef type) {
  return "'" + name.str() + "' takes a number, a string or a Uint8Array, found '" + type.str() +
         "'";
}

std::string ErrorTextBuffer::slice() {
  return "'slice' of a Buffer is not supported: in node it shares the "
         "memory, unlike 'slice' of a Uint8Array, and is deprecated; use "
         "'subarray(start, end)' for the same memory, or "
         "'Buffer.from(b.subarray(start, end))' for a copy";
}

std::string ErrorTextBuffer::position(llvm::StringRef name) {
  return "'" + name.str() + "'";
}

std::string ErrorTextBuffer::encoding(llvm::StringRef name) {
  return "the encoding comes last in '" + name.str() + "'";
}

std::string ErrorTextBuffer::withString() {
  return "an encoding goes only with a string value";
}

std::string ErrorTextBuffer::byteCount(llvm::StringRef name) {
  return "the byte count of '" + name.str() + "' must be written as a number from 1 to 6";
}

std::string ErrorTextBuffer::varFits(int64_t count, llvm::StringRef range) {
  return "cannot prove that the value fits in " + std::to_string(count) + " bytes (" + range.str() +
         "), and node would throw a RangeError; check it first";
}

std::string ErrorTextBuffer::decode(llvm::StringRef type) {
  return "'decode' takes a typed array, a DataView or an ArrayBuffer, "
         "found '" +
         type.str() + "'";
}

std::string ErrorTextBuffer::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextBuffer::written(llvm::StringRef offset, llvm::StringRef length,
                                     llvm::StringRef object) {
  return "cannot prove that '" + offset.str() + " + " + length.str() + " <= " + object.str() +
         ".length', and node would throw a RangeError; check "
         "it first, with the offset or the length written as "
         "a number";
}

std::string ErrorTextBufferLabel::offset() {
  return "the offset";
}

std::string ErrorTextBufferLabel::end() {
  return "the end";
}

std::string ErrorTextBufferLabel::length() {
  return "the length";
}

std::string ErrorTextBufferLabel::byte() {
  return "a byte";
}

std::string ErrorTextBufferLabel::offsetOf(llvm::StringRef name) {
  return "the offset of '" + name.str() + "'";
}

std::string ErrorTextBufferLabel::options() {
  return "the options of 'decode'";
}

}
