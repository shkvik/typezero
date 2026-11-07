#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker_errors.h"
#include "compiler/program/types/types.h"

namespace tz {

std::string ErrorTextConstructible::builtin() {
  return "it is a class of JavaScript, which only "
         "Reflect.construct tries to make";
}

std::string ErrorTextConstructible::abstract() {
  return "it is abstract";
}

std::string ErrorTextConstructible::noArgs() {
  return "its constructor takes no arguments";
}

std::string ErrorTextConstructible::unknown() {
  return "its constructor is unknown";
}

std::string ErrorTextConstructible::takes(llvm::ArrayRef<const FuncSymbol *> constructors) {
  std::string takes;
  for (const FuncSymbol *sym : constructors) {
    std::string shown;
    for (const Type *param : sym->params) {
      shown += (shown.empty() ? "" : ", ") + (param ? param->name().str() : "?");
    }
    takes += (takes.empty() ? "(" : " or (") + shown + ")";
  }

  return "its constructor takes " + takes;
}

std::string ErrorTextConstructible::several() {
  return "the arguments may fit several of its constructors";
}

std::string ErrorTextReflectNew::mayFail(llvm::StringRef type, llvm::StringRef why) {
  return "'new' of a class known only when the program runs may fail: '" + type.str() +
         "' can be here, and " + why.str() +
         "; write 'Reflect.construct(cls, [...])', which gives a Result";
}

std::string ErrorTextReflectNew::third() {
  return "the third argument of Reflect.construct, the class whose "
         "prototype the object gets, is not supported: there are no "
         "prototypes in tz; make an object of that class";
}

std::string ErrorTextReflectNew::notClass(llvm::StringRef type) {
  return "Reflect.construct makes an object of a class, and this is a '" + type.str() + "'";
}

std::string ErrorTextReflectNew::javascript(llvm::StringRef name) {
  return "'" + name.str() +
         "' of JavaScript cannot be made through "
         "reflection";
}

std::string ErrorTextReflectGet::json() {
  return "Reflect.get takes an object of the language; read a key of a "
         "Json as JavaScript does, 'j[key]'";
}

std::string ErrorTextReflectGet::notObject(llvm::StringRef type) {
  return "Reflect.get takes an object, and this is a '" + type.str() + "'";
}

std::string ErrorTextReflectFill::notClass(llvm::StringRef type) {
  return "Reflect.fill makes an object of a class, and this is a '" + type.str() + "'";
}

std::string ErrorTextReflectFill::cannot(llvm::StringRef name, llvm::StringRef why) {
  return "Reflect.fill cannot fill '" + name.str() + "': " + why.str() +
         " (Reflect.fill gives the fields of a class without "
         "a constructor their values)";
}

std::string ErrorTextReflectFill::javascript() {
  return "it is a class of JavaScript";
}

std::string ErrorTextReflectFill::abstract() {
  return "it is abstract; fill a class that extends it";
}

std::string ErrorTextReflectFill::ownCtor(llvm::StringRef name) {
  return "it sets its fields in a constructor; make it with 'new " + name.str() + "(...)'";
}

std::string ErrorTextReflectApply::notFunc(llvm::StringRef type) {
  return "Reflect.apply calls a function, and this is a '" + type.str() + "'";
}

std::string ErrorTextReflectApply::voidThis() {
  return "an expression of type 'void' has no value to pass as 'this'";
}

}
