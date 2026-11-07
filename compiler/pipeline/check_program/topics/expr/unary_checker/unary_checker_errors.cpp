#include "compiler/pipeline/check_program/topics/expr/unary_checker/unary_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUnary::noNumber(llvm::StringRef op, bool string) {
  return (llvm::Twine("unary '") + op + "' does not turn a " + (string ? "string" : "boolean") +
          " into a number in tz; write " +
          (string ? "'Number(s)', which gives a Result" : "'Number(b)'"))
      .str();
}

std::string ErrorTextUnary::decimalSum() {
  return "unary '+' cannot be applied to a decimal; for a double, write "
         "'Number(x)', or drop the '+'";
}

std::string ErrorTextUnary::notNumber(llvm::StringRef op, llvm::StringRef type) {
  return (llvm::Twine("operator '") + op + "' cannot be applied to type '" + type + "'").str();
}

std::string ErrorTextUnary::bitsDouble() {
  return "operator '~' works on the bits of integers, and this "
         "is a double; drop the fraction first, e.g. "
         "'Math.trunc(x).toInt()'";
}

std::string ErrorTextUnary::bitsType(llvm::StringRef type) {
  return (llvm::Twine("operator '~' cannot be applied to type '") + type + "'").str();
}

std::string ErrorTextUnary::dropsAsync() {
  return "'void' drops the Result that the Promise gives, so an error in "
         "it would go unchecked; let the task handle it, e.g. an async "
         "function that checks 'if (r.isErr()) ...'";
}

std::string ErrorTextUnary::drops() {
  return "'void' drops the Result, so an error in it would go unchecked; "
         "handle it, e.g. with '.match(...)'";
}

std::string ErrorTextUnary::allUndef() {
  return "'!' is not supported, and the value is always undefined";
}

std::string ErrorTextUnary::neverUndef(llvm::StringRef text) {
  return (llvm::Twine("'!' is not supported, and '") + text +
          "' is never undefined here; drop the '!'")
      .str();
}

std::string ErrorTextUnary::checkFirst(llvm::StringRef text) {
  return (llvm::Twine("'!' is not supported: check the value first, e.g. 'if (") + text +
          " == undefined) return ...', or give a "
          "default with '" +
          text + " ?? ...'")
      .str();
}

std::string ErrorTextDelete::optional(llvm::StringRef name, llvm::StringRef text) {
  return (llvm::Twine("'delete' cannot remove field '") + name +
          "': a field that is not there is undefined; write '" + text + " = undefined'")
      .str();
}

std::string ErrorTextDelete::required(llvm::StringRef name) {
  return (llvm::Twine("'delete' cannot remove field '") + name +
          "': an object always has the fields of its type")
      .str();
}

std::string ErrorTextDelete::notField() {
  return "'delete' removes fields of objects, and this is not a field";
}

std::string ErrorTextDelete::hole(llvm::StringRef array, llvm::StringRef position) {
  return (llvm::Twine("'delete' would leave a hole in the array, and arrays have none; "
                      "remove the element with '") +
          array + ".splice(" + position + ", 1)', or replace it: '" + array + "[" + position +
          "] = ...'")
      .str();
}

std::string ErrorTextDelete::character() {
  return "strings are immutable; a character cannot be deleted";
}

std::string ErrorTextDelete::tupleHole() {
  return "'delete' would leave a hole in the tuple, which has a fixed length; assign "
         "the element instead";
}

std::string ErrorTextDelete::objectFld() {
  return "'delete' cannot remove a field: an object always has the fields of its "
         "type";
}

std::string ErrorTextIn::undefined() {
  return "the value may be undefined, and 'in' would throw then; check "
         "it first";
}

std::string ErrorTextIn::byLiteral() {
  return "operator 'in' tells the objects of a union apart by a field, "
         "written as a string literal: '\"radius\" in shape'";
}

std::string ErrorTextIn::maybeUndef(llvm::StringRef name, llvm::StringRef member,
                                    llvm::StringRef text) {
  return (llvm::Twine("operator 'in' cannot test field '") + name + "' of '" + member +
          "', which may hold undefined, as one that is "
          "not there; check the value instead: '" +
          text + " != undefined'")
      .str();
}

std::string ErrorTextIn::always(bool with, llvm::StringRef unionName, llvm::StringRef name) {
  return (llvm::Twine("operator 'in' is always ") + (with ? "true" : "false") + " here: " +
          (with ? "every" : "no") + " member of '" + unionName + "' has field '" + name + "'")
      .str();
}

std::string ErrorTextIn::tuple() {
  return "operator 'in' is not supported for tuples: a tuple always has all its elements";
}

std::string ErrorTextIn::objMaybe(llvm::StringRef text) {
  return (llvm::Twine("operator 'in' is not supported for objects: a field that is not there is "
                      "undefined; check it with '") +
          text + " != undefined'")
      .str();
}

std::string ErrorTextIn::objTrue() {
  return "operator 'in' is not supported for objects: an object always has the "
         "fields of its type, so this would always be true";
}

std::string ErrorTextIn::objFalse(llvm::StringRef type, llvm::StringRef name) {
  return (llvm::Twine("operator 'in' is not supported for objects: type '") + type +
          "' has no field '" + name + "', so this would always be false")
      .str();
}

std::string ErrorTextIn::objPlain() {
  return "operator 'in' is not supported for objects: an object always has the "
         "fields of its type";
}

std::string ErrorTextIn::array(llvm::StringRef check) {
  return (llvm::Twine("operator 'in' is not supported for arrays, which have no holes; compare "
                      "the index with the length instead: '") +
          check + "'")
      .str();
}

}
