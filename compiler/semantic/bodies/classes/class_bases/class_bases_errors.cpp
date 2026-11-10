#include "compiler/semantic/bodies/classes/class_bases/class_bases_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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
