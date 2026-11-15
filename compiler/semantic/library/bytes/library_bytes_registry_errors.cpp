#include "compiler/semantic/library/bytes/library_bytes_registry_errors.h"

namespace tz {

std::string ErrorTextBytesIndex::notIndexed(const Type *type) {
  return "type '" + type->name().str() + "' cannot be indexed; " +
         (type->bytes() == Type::Bytes::DataView ? "read its bytes with getUint8(i) and the like"
          : type->bytes() == Type::Bytes::ArrayBuffer ? "read its bytes through a view, e.g. 'new "
                                                        "Uint8Array(buffer)[i]'"
                                                      : "it holds no elements");
}

std::string ErrorTextBytesIndex::store(llvm::StringRef target, llvm::StringRef object,
                                       llvm::StringRef position, std::optional<int64_t> at) {
  std::string check =
      at ? object.str() + ".length > " + std::to_string(*at)
         : position.str() + " >= 0 && " + position.str() + " < " + object.str() + ".length";

  return "cannot prove that the index is within the " + target.str() +
         " (0 <= index < length): it never grows, and JavaScript would "
         "drop the write; check the index first, e.g. 'if (" +
         check + ")'";
}

std::string ErrorTextBytesMember::deprecated(llvm::StringRef name) {
  return "'" + name.str() +
         "' of a Buffer is deprecated in node and not "
         "supported; use '" +
         (name == "parent" ? "buffer" : "byteOffset") + "'";
}

std::string ErrorTextBytesMember::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextBytesMember::notCalled(llvm::StringRef name, llvm::StringRef type) {
  return ErrorTextBytesMember::noMember(name, type) + "; methods must be called";
}

std::string ErrorTextBytesMember::narrow(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' of '" + type.str() +
         "' is not supported yet; narrow it first";
}

std::string ErrorTextBytesMember::lacking(llvm::StringRef name, llvm::StringRef lacking,
                                          llvm::StringRef type, llvm::StringRef with) {
  std::string message = "property '" + name.str() + "' does not exist on '" + lacking.str() +
                        "', which '" + type.str() + "' may be; narrow it first";
  if (!with.empty()) {
    message += ", e.g. 'if (x instanceof " + with.str() + ")'";
  }

  return message;
}

std::string ErrorTextBytesMember::callOnly(llvm::StringRef object, llvm::StringRef name) {
  return "'" + object.str() + "." + name.str() + "' can be used only in a call, e.g. '" +
         object.str() + "." + name.str() + "(...)'";
}

std::string ErrorTextBytesString::noText(llvm::StringRef type, bool buffer) {
  return "a value of type '" + type.str() +
         "' cannot be converted to a string: JavaScript would give '[object " + type.str() +
         "]'; print it with console.log" +
         (buffer ? ", or the text of its bytes with 'new TextDecoder().decode(buffer)'" : "");
}

std::string ErrorTextArrayBuffer::notYet(llvm::StringRef name) {
  return "'" + name.str() +
         "' is not supported yet: an ArrayBuffer keeps its "
         "size and its memory (docs/bytes.md, section 7)";
}

std::string ErrorTextArrayBuffer::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on type 'ArrayBuffer'";
}

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

std::string ErrorTextBufferLabel::comparePosition(size_t index) {
  static const char *const names[] = {"", "targetStart", "targetEnd", "sourceStart", "sourceEnd"};

  return names[index];
}

std::string ErrorTextBufferLabel::copyPosition(size_t index) {
  static const char *const names[] = {"", "targetStart", "sourceStart", "sourceEnd"};

  return names[index];
}

std::string ErrorTextBufferLabel::options() {
  return "the options of 'decode'";
}

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

std::string ErrorTextBufferStatic::buffer() {
  return "the Buffer";
}

std::string ErrorTextBufferStatic::size() {
  return "the size of a Buffer";
}

std::string ErrorTextBufferStatic::total() {
  return "the total length";
}

std::string ErrorTextBytesNew::typeArgs(llvm::StringRef type) {
  return "'new " + type.str() + "' takes no type arguments";
}

std::string ErrorTextBytesNew::buffer() {
  return "'new Buffer(...)' is not supported, as in node, where it is deprecated: "
         "make one with 'Buffer.from(values)' or 'Buffer.alloc(size)'";
}

std::string ErrorTextBytesNew::resizable() {
  return "an ArrayBuffer that can change its size is not supported yet "
         "(docs/bytes.md, section 7); make a new one of the size wanted";
}

std::string ErrorTextBytesNew::bufferLen() {
  return "the length of an ArrayBuffer";
}

std::string ErrorTextBytesNew::viewOver(llvm::StringRef type, bool typed) {
  return "a DataView is made over an ArrayBuffer, found '" + type.str() + "'" +
         (typed ? "; give its buffer: 'new DataView(u.buffer, "
                  "u.byteOffset, u.byteLength)'"
                : "");
}

std::string ErrorTextBytesNew::dataView() {
  return "the DataView";
}

std::string ErrorTextBytesNew::fatal() {
  return "a FatalTextDecoder is made by 'new TextDecoder(label, { fatal: "
         "true })'";
}

std::string ErrorTextBytesNew::the(llvm::StringRef type) {
  return "the " + type.str();
}

std::string ErrorTextBytesNew::offsetOnly(llvm::StringRef type, llvm::StringRef from) {
  return "'new " + type.str() +
         "' takes an offset and a length only with an "
         "ArrayBuffer, found '" +
         from.str() + "'";
}

std::string ErrorTextBytesNew::lengthOf(llvm::StringRef type) {
  return "the length of a " + type.str();
}

std::string ErrorTextBytesNew::floatLen() {
  return "the length of a typed array must be an integer, found 'double'; "
         "round it with Math.floor(x) or drop the fraction with 'x | 0'";
}

std::string ErrorTextTextDecoder::unknown(llvm::StringRef label) {
  return "'" + label.str() +
         "' is not an encoding TextDecoder knows (JavaScript "
         "throws a RangeError); e.g. 'utf-8', 'utf-16le', "
         "'windows-1251', 'latin1'";
}

std::string ErrorTextTextDecoder::notKnown() {
  return "a label known only when the program runs may name no "
         "encoding, which JavaScript would throw a RangeError for; "
         "parse it with 'TextDecoder.parse(label)', which gives a "
         "Result, or check it first, e.g. 'if (label == \"utf-8\" || "
         "label == \"windows-1251\")'";
}

std::string ErrorTextTextDecoder::options() {
  return "the options of TextDecoder";
}

std::string ErrorTextBytesSource::notSource(llvm::StringRef target, llvm::StringRef from,
                                            bool string) {
  return "a " + target.str() +
         " is made from a length, an array or an iterator of numbers, "
         "another typed array or an ArrayBuffer, found '" +
         from.str() + "'" +
         (string ? "; for the bytes of a string, use 'new "
                   "TextEncoder().encode(s)' or "
                   "'Buffer.from(s)'"
                 : "");
}

std::string ErrorTextBytesSource::quoted(llvm::StringRef target) {
  return "'" + target.str() + "'";
}

std::string ErrorTextBytesSource::notNumbers(llvm::StringRef from, llvm::StringRef target) {
  return "the elements of '" + from.str() + "' are not numbers, which a " + target.str() + " holds";
}

std::string ErrorTextBytesSource::doubles(llvm::StringRef from, llvm::StringRef target,
                                          llvm::StringRef text) {
  return "the elements of '" + from.str() +
         "' are doubles, which an "
         "element of '" +
         target.str() +
         "' cannot hold without an explicit conversion; convert them, "
         "e.g. '" +
         target.str() + ".from(" + text.str() + ", x => Math.round(x) & 0xff)'";
}

std::string ErrorTextBytesSource::hint(llvm::StringRef target, llvm::StringRef text,
                                       llvm::StringRef wrap) {
  return target.str() + ".from(" + text.str() + ", x => " + wrap.str() + ")";
}

std::string ErrorTextBytesSource::mayNotFit(llvm::StringRef from, llvm::StringRef fromRange,
                                            llvm::StringRef target, llvm::StringRef range,
                                            llvm::StringRef hint) {
  return "the elements of '" + from.str() + "' (" + fromRange.str() +
         ") may not fit in an element of '" + target.str() + "' (" + range.str() +
         "), and JavaScript would cut them without a word; convert "
         "them explicitly, e.g. '" +
         hint.str() + "'";
}

std::string ErrorTextBytesSource::unproven(llvm::StringRef from, llvm::StringRef target,
                                           llvm::StringRef range, llvm::StringRef hint) {
  return "cannot prove that the elements of '" + from.str() + "' fit in " + "an element of '" +
         target.str() + "' (" + range.str() +
         "), and JavaScript would cut them without a word; write the "
         "values in the literal, or convert them explicitly, e.g. '" +
         hint.str() + "'";
}

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
