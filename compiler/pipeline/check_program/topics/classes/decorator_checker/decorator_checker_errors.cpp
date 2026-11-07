#include "compiler/pipeline/check_program/topics/classes/decorator_checker/decorator_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDecoratedNames::shared(llvm::StringRef what, llvm::StringRef cls,
                                            llvm::StringRef records) {
  return (llvm::Twine(what) + " of '" + cls + "' are decorated, and " + records +
          " for both would be one: metadata is kept on the class "
          "itself, by the member's name (docs/decorators.md, section 3); "
          "give one of them another name")
      .str();
}

std::string ErrorTextDecoratedNames::bothKinds(llvm::StringRef name) {
  return (llvm::Twine("both the static member '") + name + "' and the member '" + name + "'").str();
}

std::string ErrorTextDecoratedNames::variants(llvm::StringRef name) {
  return (llvm::Twine("two variants of '") + name + "'").str();
}

std::string ErrorTextDecoratedNames::design() {
  return "the records 'design:*' the compiler writes";
}

std::string ErrorTextDecoratedNames::recordsOf(llvm::StringRef shared) {
  return (llvm::Twine("the records of '@") + shared + "'").str();
}

std::string ErrorTextDecoratedNames::other() {
  return "the other one is here";
}

std::string ErrorTextDecoratedNames::sameRecord(llvm::StringRef cls) {
  return (llvm::Twine("two constructors of '") + cls +
          "' have decorated parameters, and the record "
          "'design:paramtypes' the compiler writes for the class would be "
          "one: metadata is kept on the class itself (docs/decorators.md, "
          "section 3); decorate the parameters of one of them")
      .str();
}

std::string ErrorTextDecorate::accessor() {
  return "a decorator cannot decorate an 'accessor' field: in tz it is an ordinary "
         "field, and reading or writing it never runs code";
}

std::string ErrorTextDecorate::abstract() {
  return "a decorator cannot decorate an abstract method, which has no code; decorate "
         "the methods that implement it";
}

std::string ErrorTextDecorate::arguments(llvm::StringRef label, llvm::StringRef place,
                                         llvm::StringRef needs, llvm::StringRef given,
                                         llvm::StringRef args) {
  return (llvm::Twine(label) + " cannot decorate " + place + ": it takes " + needs +
          " arguments, and a decorator of " + place + " gets " + given + ", " + args)
      .str();
}

std::string ErrorTextDecorate::descriptor(llvm::StringRef label) {
  return (llvm::Twine(label) + " takes the descriptor of a method: it decorates methods, not "
                               "parameters")
      .str();
}

std::string ErrorTextDecorate::position(llvm::StringRef label) {
  return (llvm::Twine(label) + " takes the position of a parameter: it decorates parameters, not "
                               "methods")
      .str();
}

std::string ErrorTextDecorate::generic(llvm::StringRef cls) {
  return (llvm::Twine("a decorator that takes the descriptor of a method of a generic class is not "
                      "supported yet: each instance of '") +
          cls +
          "' has a method of its own; decorate a method of a class "
          "that is not generic, or take only the class and the name")
      .str();
}

std::string ErrorTextDecorate::metadata() {
  return "'Reflect.metadata' takes the key and the value of the record, e.g. "
         "'@Reflect.metadata(Role, \"admin\")'";
}

std::string ErrorTextDecoratorResult::runtime(llvm::StringRef label, llvm::StringRef result,
                                              llvm::StringRef current) {
  return (llvm::Twine(label) + " returns '" + result +
          "', some class known only when the program runs, and the class it "
          "gives in place of '" +
          current +
          "' must be known when compiling: make the decorator generic, "
          "'<T extends Constructor>(cls: T)', and return 'cls' or a class "
          "that extends it")
      .str();
}

std::string ErrorTextDecoratorResult::forClass(llvm::StringRef label, llvm::StringRef result) {
  return (llvm::Twine(label) + " returns '" + result +
          "': a class decorator gives back nothing, the class it got, or a "
          "class that extends it, which then replaces it")
      .str();
}

std::string ErrorTextDecoratorResult::forMethod(llvm::StringRef label, llvm::StringRef result) {
  return (llvm::Twine(label) + " returns '" + result +
          "': a decorator of a method returns nothing, or the descriptor "
          "it got")
      .str();
}

std::string ErrorTextDecoratorResult::other(llvm::StringRef label, llvm::StringRef result,
                                            llvm::StringRef place) {
  return (llvm::Twine(label) + " returns '" + result + "', which a decorator of " + place +
          " cannot give back; return nothing")
      .str();
}

std::string ErrorTextDecoratorPlace::aClass() {
  return "a class";
}

std::string ErrorTextDecoratorPlace::aField() {
  return "a field";
}

std::string ErrorTextDecoratorPlace::aMethod() {
  return "a method";
}

std::string ErrorTextDecoratorPlace::aParameter() {
  return "a parameter";
}

std::string ErrorTextDecoratorPlace::classArgs() {
  return "the class";
}

std::string ErrorTextDecoratorPlace::fieldArgs() {
  return "the class and the name of the field";
}

std::string ErrorTextDecoratorPlace::methodArgs() {
  return "the class, the name of the method and its descriptor";
}

std::string ErrorTextDecoratorPlace::paramArgs() {
  return "the class, the name of the method (undefined for a constructor) and the position of "
         "the parameter";
}

std::string ErrorTextDecoratorPlace::label(llvm::StringRef name) {
  if (name.empty()) {
    return "the decorator";
  }

  return (llvm::Twine("'") + name + "'").str();
}

}
