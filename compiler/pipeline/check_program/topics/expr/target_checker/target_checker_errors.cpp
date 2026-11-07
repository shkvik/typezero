#include "compiler/pipeline/check_program/topics/expr/target_checker/target_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

}
