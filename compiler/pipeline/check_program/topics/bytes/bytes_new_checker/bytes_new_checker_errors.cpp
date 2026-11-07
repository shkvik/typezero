#include "compiler/pipeline/check_program/topics/bytes/bytes_new_checker/bytes_new_checker_errors.h"

namespace tz {

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

}
