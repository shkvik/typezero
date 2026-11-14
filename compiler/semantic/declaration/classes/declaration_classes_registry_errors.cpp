#include "compiler/semantic/declaration/classes/declaration_classes_registry_errors.h"
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

std::string ErrorTextExtends::byName() {
  return "a class can extend only a class, by its name";
}

std::string ErrorTextExtends::itself(llvm::StringRef cls, llvm::StringRef through) {
  std::string via =
      through.empty() ? std::string() : (llvm::Twine(" through '") + through + "'").str();

  return (llvm::Twine("class '") + cls + "' cannot extend itself" + via).str();
}

std::string ErrorTextExtends::runtime(llvm::StringRef cls, llvm::StringRef parent) {
  return (llvm::Twine("class '") + cls + "' cannot extend '" + parent +
          "': only the functions of node:fs make its objects")
      .str();
}

std::string ErrorTextExtends::valueBase(llvm::StringRef cls, llvm::StringRef parent) {
  return (llvm::Twine("class '") + cls + "' cannot extend '" + parent +
          "', whose base is a value, made where its declaration runs; "
          "write it as an expression there: 'const " +
          cls + " = class extends " + parent + " { ... }'")
      .str();
}

std::string ErrorTextExtends::errorClass(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("there is no '") + name + "' in tz: an error is a value; make '" + cls +
          "' a type of object with a tag, e.g. 'type " + cls + " = { code: \"" + cls +
          "\"; message: string }'")
      .str();
}

std::string ErrorTextExtends::notClass(llvm::StringRef name, llvm::StringRef cls, bool alias) {
  std::string message =
      (llvm::Twine("'") + name + "' is not a class, and a class can extend only a class").str();
  if (alias) {
    message += (llvm::Twine("; to say that '") + cls +
                "' has what an interface lists, write "
                "'implements " +
                name + "'")
                   .str();
  }

  return message;
}

std::string ErrorTextExtends::noClass(llvm::StringRef name) {
  return (llvm::Twine("cannot find class '") + name + "'").str();
}

std::string ErrorTextExtends::classIface(llvm::StringRef type, bool extends) {
  std::string hint =
      extends
          ? std::string()
          : (llvm::Twine("; to take its fields and methods, write 'extends ") + type + "'").str();

  return (llvm::Twine("'") + type +
          "' is a class, and a class implements only "
          "interfaces" +
          hint)
      .str();
}

std::string ErrorTextExtends::notIface(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is not an interface; a class implements object "
          "types, such as 'interface Named { name: string }'")
      .str();
}

std::string ErrorTextRedeclared::noField(llvm::StringRef name, llvm::StringRef parent) {
  return (llvm::Twine("'") + name + "' is marked 'override', but '" + parent + "' has no field '" +
          name + "'")
      .str();
}

std::string ErrorTextRedeclared::noParent(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'override' says that '") + name + "' is a field of the class that '" + cls +
          "' extends, and it extends none")
      .str();
}

std::string ErrorTextRedeclared::isPrivate(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("'") + name + "' is private in class '" + owner +
          "', so a class that extends it cannot declare it again")
      .str();
}

std::string ErrorTextRedeclared::narrower(llvm::StringRef name, llvm::StringRef baseAccess,
                                          llvm::StringRef owner, llvm::StringRef access,
                                          llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is " + baseAccess + " in '" + owner + "', so it cannot be " +
          access + " in '" + cls + "'")
      .str();
}

std::string ErrorTextRedeclared::noValue(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("'") + name + "' is a field of '" + owner +
          "' already; declared again in a class without a constructor, JavaScript would set "
          "it to undefined after the constructor of '" +
          owner + "' set it: drop the declaration, or set it in a constructor")
      .str();
}

std::string ErrorTextInterfaceBases::itself(llvm::StringRef alias, llvm::StringRef through) {
  std::string via =
      through.empty() ? std::string() : (llvm::Twine(" through '") + through + "'").str();

  return (llvm::Twine("interface '") + alias + "' cannot extend itself" + via).str();
}

std::string ErrorTextInterfaceBases::classBase(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is a class, and an interface extends only "
          "interfaces")
      .str();
}

std::string ErrorTextInterfaceBases::notIface(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is not an interface; an interface extends "
          "object types, such as 'interface Named { name: string }'")
      .str();
}

std::string ErrorTextInterfaceBases::twoMethods(llvm::StringRef alias, llvm::StringRef name) {
  return (llvm::Twine("interface '") + alias + "' gets method '" + name +
          "' from two interfaces with different signatures")
      .str();
}

std::string ErrorTextInterfaceBases::twoFields(llvm::StringRef alias, llvm::StringRef name,
                                               llvm::StringRef origin, llvm::StringRef type,
                                               llvm::StringRef base, llvm::StringRef other) {
  return (llvm::Twine("interface '") + alias + "' gets field '" + name + "' from '" + origin +
          "' as '" + type + "' and from '" + base + "' as '" + other + "'; a field has one type")
      .str();
}

std::string ErrorTextInterfaceBases::method(llvm::StringRef alias, llvm::StringRef name) {
  return (llvm::Twine("interface '") + alias + "' declares method '" + name +
          "' again; an interface cannot change the members of one "
          "it extends")
      .str();
}

std::string ErrorTextInterfaceBases::member(llvm::StringRef alias, llvm::StringRef name) {
  return (llvm::Twine("interface '") + alias + "' declares '" + name +
          "' again; an interface cannot change the members of one "
          "it extends")
      .str();
}

std::string ErrorTextInterfaceBases::narrowed(llvm::StringRef name, llvm::StringRef alias,
                                              llvm::StringRef type, llvm::StringRef origin,
                                              llvm::StringRef other) {
  return (llvm::Twine("field '") + name + "' of '" + alias + "' is '" + type + "', and '" + origin +
          "' declares it as '" + other +
          "'; an interface can narrow a field of one it extends only to objects "
          "of a type down the chain of its type, such as 'Dog' for 'Animal', "
          "which lie in memory as they do")
      .str();
}

}
