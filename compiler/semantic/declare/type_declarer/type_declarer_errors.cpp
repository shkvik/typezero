#include "compiler/semantic/declare/type_declarer/type_declarer_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextBuiltinAlias::builtin(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is a built-in type and cannot be redefined").str();
}

std::string ErrorTextBuiltinAlias::number(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is a built-in type and cannot be redefined; only 'type " +
          name + " = number', for tsc, may declare it")
      .str();
}

std::string ErrorTextFreeTypeName::previous() {
  return "previously declared here";
}

std::string ErrorTextFreeTypeName::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate declaration of type '") + name + "'").str();
}

std::string ErrorTextAliasCycle::itself(llvm::StringRef name) {
  return (llvm::Twine("type alias '") + name + "' refers to itself").str();
}

std::string ErrorTextRecursiveUnion::notUnion(llvm::StringRef name) {
  return (llvm::Twine("type alias '") + name +
          "' refers to itself, and only a union of several types without undefined can: make a "
          "field optional instead, e.g. 'next?: " +
          name + "'")
      .str();
}

std::string ErrorTextEnumCycle::itself(llvm::StringRef name) {
  return (llvm::Twine("the members of enum '") + name + "' depend on themselves").str();
}

std::string ErrorTextUniqueMember::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate enum member '") + name + "'").str();
}

std::string ErrorTextMemberConstant::notConst() {
  return "the value of an enum member must be a constant: a number, a string, or an expression "
         "of them and of other members";
}

std::string ErrorTextCountedMember::noValue(llvm::StringRef name) {
  return (llvm::Twine("enum member '") + name +
          "' needs a value, since the member before it is not a number")
      .str();
}

std::string ErrorTextWholeMember::notWhole(llvm::StringRef name, llvm::StringRef value) {
  return (llvm::Twine("the value of enum member '") + name +
          "' must be a whole number from -2^53 to 2^53, found " + value)
      .str();
}

std::string ErrorTextEnumKinds::mixed(llvm::StringRef name) {
  return (llvm::Twine("enum '") + name + "' cannot mix numbers and strings").str();
}

std::string ErrorTextEnumMember::missing(llvm::StringRef property, llvm::StringRef name) {
  return (llvm::Twine("property '") + property + "' does not exist on enum '" + name + "'").str();
}

std::string ErrorTextEnumIndex::strings(llvm::StringRef name) {
  return (llvm::Twine("enum '") + name + "' has string values, and only numbers map back to names")
      .str();
}

std::string ErrorTextEnumIndex::notInteger(llvm::StringRef type) {
  return (llvm::Twine("the value of an enum member is an integer, found '") + type + "'").str();
}

}
