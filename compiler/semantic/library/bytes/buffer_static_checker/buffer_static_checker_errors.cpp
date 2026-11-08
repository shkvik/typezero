#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker_errors.h"

namespace tz {

std::string ErrorTextBytesStatic::missing(llvm::StringRef object, llvm::StringRef name) {
  return "'" + object.str() + "." + name.str() + "' does not exist";
}

std::string ErrorTextBytesStatic::notYet(llvm::StringRef object, llvm::StringRef name) {
  return "'" + object.str() + "." + name.str() + "' is not supported; of " + object.str() +
         " there are 'from' and 'of'";
}

std::string ErrorTextBytesStatic::from(llvm::StringRef object) {
  return "'" + object.str() + ".from'";
}

std::string ErrorTextBytesStatic::source(llvm::StringRef what, llvm::StringRef type) {
  return what.str() + " takes an array, a typed array or an iterator, found '" + type.str() + "'";
}

std::string ErrorTextBufferStatic::offsetOnly(llvm::StringRef type) {
  return "'Buffer.from' takes an offset and a length only with "
         "an ArrayBuffer, found '" +
         type.str() + "'";
}

std::string ErrorTextBufferStatic::allocFill(llvm::StringRef type) {
  return "'Buffer.alloc' fills with a number, a string or a Uint8Array, "
         "found '" +
         type.str() + "'";
}

std::string ErrorTextBufferStatic::fillCoding() {
  return "an encoding goes only with a string to fill with";
}

std::string ErrorTextBufferStatic::concat(llvm::StringRef type) {
  return "'Buffer.concat' takes an array of Uint8Arrays or Buffers, "
         "found '" +
         type.str() + "'";
}

std::string ErrorTextBufferStatic::byteLength(llvm::StringRef type) {
  return "'Buffer.byteLength' takes a string, a typed array, a "
         "DataView or an ArrayBuffer, found '" +
         type.str() + "'";
}

std::string ErrorTextBufferStatic::unknown(llvm::StringRef name) {
  return "'Buffer." + name.str() +
         "' is not supported; Buffer has from, alloc, "
         "allocUnsafe, concat, byteLength, compare, isBuffer, isEncoding "
         "and of";
}

std::string ErrorTextBufferStatic::embedSize(llvm::StringRef shown) {
  return "'" + shown.str() +
         "' is larger than the 1 GiB a Uint8Array "
         "may have";
}

std::string ErrorTextBufferStatic::buffer() {
  return "the Buffer";
}

std::string ErrorTextBufferStatic::size() {
  return "the size of a Buffer";
}

std::string ErrorTextBufferStatic::total() {
  return "the total length";
}

}
