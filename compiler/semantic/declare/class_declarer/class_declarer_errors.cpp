#include "compiler/semantic/declare/class_declarer/class_declarer_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUniqueField::previous() {
  return "previously declared here";
}

std::string ErrorTextUniqueField::duplicate(llvm::StringRef field, llvm::StringRef className) {
  return (llvm::Twine("duplicate member '") + field + "' in class '" + className + "'").str();
}

std::string ErrorTextFieldOrMethod::field() {
  return "the field is declared here";
}

std::string ErrorTextFieldOrMethod::shared(llvm::StringRef field, llvm::StringRef className) {
  return (llvm::Twine("duplicate member '") + field + "' in class '" + className +
          "': a field and a method cannot share a name")
      .str();
}

std::string ErrorTextSetLater::definite(llvm::StringRef field) {
  return (llvm::Twine("'") + field +
          "!' (\"set later\") is not supported: a field gets its value in the constructor; if it "
          "may have none, make it optional: '" +
          field + "?: T'")
      .str();
}

std::string ErrorTextTypedField::missing(llvm::StringRef field) {
  return (llvm::Twine("field '") + field + "' needs a type, e.g. '" + field + ": int'").str();
}

std::string ErrorTextTypedField::property(llvm::StringRef field) {
  return (llvm::Twine("parameter '") + field +
          "' declares a field and needs a type, e.g. 'public " + field + ": int'")
      .str();
}

std::string ErrorTextDeclaredValue::constructor(llvm::StringRef field) {
  return (llvm::Twine("field '") + field +
          "' cannot get a value where it is declared: set it in the constructor, 'this." + field +
          " = ...'")
      .str();
}

std::string ErrorTextDeclaredValue::literal(llvm::StringRef field) {
  return (llvm::Twine("field '") + field +
          "' cannot get a value where it is declared: in a class without a constructor it can "
          "be only a literal next to a written type, e.g. '" +
          field + ": int = 0'; set other values in a constructor")
      .str();
}

std::string ErrorTextTypeSeen::shared(llvm::StringRef field) {
  return (llvm::Twine("static field '") + field +
          "' needs a type: only a literal shows it without checking the value, e.g. 'static " +
          field + ": int = ...'")
      .str();
}

std::string ErrorTextTypeSeen::function(llvm::StringRef field) {
  return (llvm::Twine("field '") + field +
          "' holds a function that code sets again, so it is a field, not a method, and needs "
          "a type, e.g. '" +
          field + ": () => void = ...'")
      .str();
}

std::string ErrorTextStaticParams::shared(llvm::StringRef field, llvm::StringRef className) {
  return (llvm::Twine("static field '") + field + "' cannot use the type parameters of '" +
          className + "': every instance of the class shares it")
      .str();
}

std::string ErrorTextStaticValue::missing(llvm::StringRef field, llvm::StringRef type) {
  return (llvm::Twine("static field '") + field + "' needs a value, e.g. 'static " + field + ": " +
          type + " = ...', or make it optional: 'static " + field + "?: " + type + "'")
      .str();
}

std::string ErrorTextParamType::extra(llvm::StringRef param, llvm::StringRef type) {
  return (llvm::Twine("parameter '") + param + "' takes more arguments than '" + type + "' passes")
      .str();
}

std::string ErrorTextAbstractReturn::missing(llvm::StringRef method, llvm::StringRef className) {
  return (llvm::Twine("abstract method '") + method + "' of generic class '" + className +
          "' needs a return type, e.g. '): T'")
      .str();
}

std::string ErrorTextGenericMethod::abstract() {
  return "a generic method cannot be abstract: each call runs a copy of it made for its types";
}

std::string ErrorTextGenericMethod::overrides() {
  return "a generic method cannot be an override: each call runs a copy of it made for its "
         "types";
}

std::string ErrorTextVariantPair::other() {
  return "the other variant is here";
}

std::string ErrorTextVariantPair::duplicate(llvm::StringRef variant) {
  return (llvm::Twine("duplicate variant ") + variant +
          ": variants must differ in their parameters")
      .str();
}

std::string ErrorTextVariantPair::returnOnly(bool constructor, llvm::StringRef name) {
  std::string what =
      constructor ? std::string("the constructor") : (llvm::Twine("'") + name + "'").str();

  return (llvm::Twine("variants of ") + what +
          " cannot differ only in what they return: a call chooses a variant by its arguments")
      .str();
}

std::string ErrorTextVariantPair::ambiguous(llvm::StringRef first, llvm::StringRef second,
                                            int count) {
  llvm::StringRef plural = count == 1 ? "" : "s";

  return (llvm::Twine("variants ") + first + " and " + second + " fit a call with " +
          llvm::Twine(count) + " argument" + plural +
          " equally well; give them parameters of different types")
      .str();
}

std::string ErrorTextParentMethod::clash(llvm::StringRef field, llvm::StringRef parent) {
  return (llvm::Twine("'") + field + "' is a method of '" + parent +
          "', so a class that extends it cannot have a field of that name")
      .str();
}

std::string ErrorTextNarrowedField::layout(llvm::StringRef field, llvm::StringRef className,
                                           llvm::StringRef type, llvm::StringRef owner,
                                           llvm::StringRef inherited) {
  return (llvm::Twine("field '") + field + "' of '" + className + "' is '" + type + "', and '" +
          owner + "' declares it as '" + inherited +
          "'; a class can narrow a field of the class it extends only to objects of a type down "
          "the chain of its type, such as 'Dog' for 'Animal', which lie in memory as they do")
      .str();
}

}
