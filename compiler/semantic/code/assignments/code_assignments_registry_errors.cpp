#include "compiler/semantic/code/assignments/code_assignments_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextAssign::imported(llvm::StringRef name) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is imported").str();
}

std::string ErrorTextAssign::nullishVar() {
  return "only variables can be the target of '??='";
}

std::string ErrorTextAssign::notOptional(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'??=' needs a variable that may be undefined; '") + name + "' has type '" +
          type + "'")
      .str();
}

std::string ErrorTextAssign::indexRange() {
  return "cannot prove that the index is within the array; check it first, "
         "e.g. 'if (i < a.length)'";
}

std::string ErrorTextAssign::untyped(llvm::StringRef name) {
  return (llvm::Twine("cannot infer the type of field '") + name +
          "' from this value; declare it, e.g. '" + name + ": int | undefined = undefined'")
      .str();
}

std::string ErrorTextAssign::outOfRange() {
  return "cannot prove that the index is within the array (0 <= index <= length); writing further "
         "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";
}

std::string ErrorTextAssign::lengthRange(llvm::StringRef element, llvm::StringRef array) {
  return (llvm::Twine("cannot prove that the new length is from 0 to the old one: an array "
                      "of '") +
          element +
          "' cannot grow by setting its length, which would leave elements "
          "without a value; check it first, e.g. 'if (n >= 0 && n <= " +
          array + ".length)', or use push()")
      .str();
}

std::string ErrorTextAssign::length() {
  return "an array length";
}

std::string ErrorTextCompound::joinsText(llvm::StringRef value, llvm::StringRef hint) {
  return (llvm::Twine("operator '+=' cannot be applied to types 'string' and '") + value +
          "' (JavaScript would join them as text)" + hint)
      .str();
}

std::string ErrorTextCompound::convertTo() {
  return "; convert the value with String()";
}

std::string ErrorTextCompound::writeAs(llvm::StringRef target, llvm::StringRef value) {
  return (llvm::Twine("; write '") + target + " += String(" + value + ")'").str();
}

std::string ErrorTextCompound::bitsDouble(llvm::StringRef spelling) {
  return (llvm::Twine("operator '") + spelling +
          "' works on the bits of integers, and this is a double; "
          "drop the fraction first, e.g. 'Math.trunc(x).toInt()'")
      .str();
}

std::string ErrorTextCompound::nothing(llvm::StringRef spelling) {
  return (llvm::Twine("'") + spelling +
          " 0' changes nothing in tz: integers are "
          "exact, and nothing wraps around here; to "
          "count modulo 2^32, use 'Bit.Wrap32'")
      .str();
}

std::string ErrorTextCompound::signedTo(llvm::StringRef type) {
  return (llvm::Twine("'>>>=' shifts in zeros, as a shift of an unsigned "
                      "type does; '") +
          type + "' is signed: use '>>='")
      .str();
}

std::string ErrorTextCompound::mixed(llvm::StringRef spelling, llvm::StringRef type,
                                     llvm::StringRef value, llvm::StringRef hint) {
  return (llvm::Twine("operator '") + spelling + "' cannot be applied to types '" + type +
          "' and '" + value + "': a number changes its type only explicitly" + hint)
      .str();
}

std::string ErrorTextLogicalAssign::simple(llvm::StringRef op) {
  return (llvm::Twine("'") + op +
          "' needs a variable, a field or an array element at a "
          "simple index, such as 'a[i]'")
      .str();
}

std::string ErrorTextLogicalAssign::field() {
  return "'??=' needs a variable or a field of one, such as 'o.x'";
}

std::string ErrorTextLogicalAssign::fieldOpt(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'??=' needs a field that may be undefined; '") + name + "' has type '" +
          type + "'")
      .str();
}

std::string ErrorTextUpdate::notNumber(llvm::StringRef op, llvm::StringRef type) {
  return (llvm::Twine("operator '") + op + "' cannot be applied to type '" + type + "'").str();
}

std::string ErrorTextIndexTarget::bytes(llvm::StringRef type, bool dataView) {
  llvm::StringRef how = dataView ? "write its bytes with setUint8(i, value) and the like"
                                 : "write its bytes through a view, e.g. 'new "
                                   "Uint8Array(buffer)[i] = value'";

  return (llvm::Twine("type '") + type + "' cannot be indexed; " + how).str();
}

std::string ErrorTextIndexTarget::undefined() {
  return "the value may be undefined; check it before assigning to it";
}

std::string ErrorTextIndexTarget::immutable() {
  return "strings are immutable; a character cannot be assigned to";
}

std::string ErrorTextIndexTarget::arrayUndef() {
  return "the array may be undefined; check it first";
}

std::string ErrorTextIndexTarget::notIndexed(llvm::StringRef type) {
  return (llvm::Twine("type '") + type + "' cannot be indexed").str();
}

std::string ErrorTextMemberTarget::undefined(llvm::StringRef name) {
  return (llvm::Twine("the value may be undefined; check it before assigning "
                      "to '") +
          name + "'")
      .str();
}

std::string ErrorTextMemberTarget::fixedSize(llvm::StringRef type) {
  return (llvm::Twine("the length of '") + type +
          "' cannot change; make a new one of the length wanted")
      .str();
}

std::string ErrorTextMemberTarget::noAssign(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' of type '" + type + "' cannot be assigned to")
      .str();
}

std::string ErrorTextMemberTarget::onlyAssign() {
  return "only '=' can change the length of an array";
}

std::string ErrorTextMemberTarget::textLength() {
  return "the length of a string cannot be changed";
}

std::string ErrorTextAssignTarget::arrayForm() {
  return "destructuring assignment can only be a statement, e.g. '[a, b] = [b, a]'";
}

std::string ErrorTextAssignTarget::objectForm() {
  return "destructuring assignment can only be a statement, e.g. '({ a, b } = o)'";
}

std::string ErrorTextAssignTarget::notTarget() {
  return "only variables, fields, array elements and 'length' can be assigned to";
}

std::string ErrorTextVarTarget::toThis() {
  return "cannot assign to 'this'";
}

std::string ErrorTextVarTarget::constant(llvm::StringRef name, bool function) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is a " +
          (function ? "function" : "constant"))
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

std::string ErrorTextImportAssign::imported(llvm::StringRef name) {
  return (llvm::Twine("cannot assign to '") + name +
          "' because it is imported; change it in "
          "the file that declares it, e.g. with a "
          "function that file exports")
      .str();
}

}
