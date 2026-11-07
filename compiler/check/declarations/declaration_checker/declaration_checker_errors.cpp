#include "compiler/check/declarations/declaration_checker/declaration_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextExports::unexported(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' is declared here, but '" + file + "' does not export it")
      .str();
}

std::string ErrorTextExports::undeclared(llvm::StringRef name, llvm::StringRef declarations) {
  return (llvm::Twine("'") + name + "' is exported, but '" + declarations +
          "' does not declare it: a declaration file declares all that its "
          "module exports")
      .str();
}

std::string ErrorTextDeclaration::quoted(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextDeclaration::memberOf(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' of '" + cls + "'").str();
}

std::string ErrorTextDeclaration::fieldOf(llvm::StringRef field, llvm::StringRef what) {
  return (llvm::Twine("field '") + field + "' of " + what).str();
}

std::string ErrorTextDeclaration::paramOf(llvm::StringRef param, llvm::StringRef what) {
  return (llvm::Twine("parameter '") + param + "' of " + what).str();
}

std::string ErrorTextDeclaration::typeOf(llvm::StringRef what) {
  return (llvm::Twine("the type of ") + what).str();
}

std::string ErrorTextDeclaration::gives(llvm::StringRef what) {
  return (llvm::Twine("what ") + what + " gives").str();
}

std::string ErrorTextDeclaration::notType(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' is not a type in '" + file + "'").str();
}

std::string ErrorTextDeclaration::typeParams(llvm::StringRef name, size_t here, size_t there,
                                             llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' takes " + std::to_string(here) +
          " type parameters here, and " + std::to_string(there) + " in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaration::extraField(llvm::StringRef what, llvm::StringRef field,
                                             llvm::StringRef file) {
  return (llvm::Twine(what) + " has field '" + field + "' here, and not in '" + file + "'").str();
}

std::string ErrorTextDeclaration::absentHere(llvm::StringRef what, llvm::StringRef field,
                                             llvm::StringRef file) {
  return (llvm::Twine(what) + " has field '" + field + "' in '" + file +
          "', which is not declared here")
      .str();
}

std::string ErrorTextDeclaration::readonly(llvm::StringRef what, bool readonly,
                                           llvm::StringRef file) {
  return (llvm::Twine(what) + " is " + (readonly ? "readonly" : "not readonly") + " in '" + file +
          "'")
      .str();
}

std::string ErrorTextDeclaration::optional(llvm::StringRef what, bool optional,
                                           llvm::StringRef file) {
  return (llvm::Twine(what) + " is " + (optional ? "optional" : "not optional") + " in '" + file +
          "'")
      .str();
}

std::string ErrorTextDeclaration::mismatch(llvm::StringRef what, llvm::StringRef here,
                                           llvm::StringRef there, llvm::StringRef file) {
  return (llvm::Twine(what) + " is '" + here + "' here, and '" + there + "' in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaration::paramCount(llvm::StringRef what, size_t here, size_t there,
                                             llvm::StringRef file) {
  return (llvm::Twine(what) + " takes " + std::to_string(here) + " parameters here, and " +
          std::to_string(there) + " in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaration::paramKind(llvm::StringRef param, bool optional,
                                            llvm::StringRef file) {
  return (llvm::Twine(param) + " is " + (optional ? "optional" : "required") +
          " here, and not in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaration::noFunction(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' is not a function in '" + file + "'").str();
}

std::string ErrorTextDeclaration::otherCount(llvm::StringRef what, llvm::StringRef file) {
  return (llvm::Twine(what) + " takes another number of type parameters in '" + file + "'").str();
}

std::string ErrorTextDeclaration::returnType(llvm::StringRef what) {
  return (llvm::Twine("write what ") + what + " gives: ': T' after its parameters").str();
}

std::string ErrorTextDeclaredClass::notClass(llvm::StringRef name, llvm::StringRef file) {
  return (llvm::Twine("'") + name + "' is not a class in '" + file + "'").str();
}

std::string ErrorTextDeclaredClass::noAccess(llvm::StringRef what) {
  return (llvm::Twine("write the access of ") + what + ": public, protected or private").str();
}

std::string ErrorTextDeclaredClass::isField(llvm::StringRef what, llvm::StringRef file) {
  return (llvm::Twine(what) + " is a field in '" + file + "'").str();
}

std::string ErrorTextDeclaredClass::isMethod(llvm::StringRef what, llvm::StringRef file) {
  return (llvm::Twine(what) + " is a method in '" + file + "'").str();
}

std::string ErrorTextDeclaredClass::noCallable(llvm::StringRef cls, llvm::StringRef file) {
  return (llvm::Twine("'") + cls + "' has no constructor that programs can call in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaredClass::newAccess(llvm::StringRef cls, llvm::StringRef access,
                                              llvm::StringRef file) {
  return (llvm::Twine("the constructor of '") + cls + "' is " + access + " in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaredClass::noMember(llvm::StringRef cls, llvm::StringRef name,
                                             bool isStatic, llvm::StringRef file) {
  return (llvm::Twine("'") + cls + "' has no " + (isStatic ? "static member '" : "member '") +
          name + "' in '" + file + "'")
      .str();
}

std::string ErrorTextDeclaredClass::isPrivate(llvm::StringRef what, llvm::StringRef file) {
  return (llvm::Twine(what) + " is private in '" + file +
          "': a declaration file declares only what programs can use")
      .str();
}

std::string ErrorTextDeclaredClass::accessIs(llvm::StringRef what, llvm::StringRef access,
                                             llvm::StringRef file) {
  return (llvm::Twine(what) + " is " + access + " in '" + file + "'").str();
}

std::string ErrorTextDeclaredClass::undeclared(llvm::StringRef name, llvm::StringRef cls,
                                               bool isStatic, llvm::StringRef declarations) {
  return (llvm::Twine("'") + name + "' of '" + cls + "' is " +
          (isStatic ? "a static member" : "a member") + " that programs can use, but '" +
          declarations + "' does not declare it")
      .str();
}

}
