#include "compiler/semantic/bodies/expr/name_checker/name_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

namespace {

const std::pair<GlobalName, llvm::StringLiteral> kObjectExamples[] = {
    {GlobalName::PROCESS, "process.argv"},
    {GlobalName::CONSOLE, "console.log(x)"},
    {GlobalName::DATE, "Date.now()"},
    {GlobalName::PERFORMANCE, "performance.now()"},
    {GlobalName::REG_EXP, "new RegExp(pattern)"},
    {GlobalName::JSON, "JSON.parse(text)"},
};

}

std::optional<llvm::StringRef> ErrorTextNameUse::objectExample(std::optional<GlobalName> global) {
  for (const auto &[object, example] : kObjectExamples) {
    if (object == global) {
      return example;
    }
  }

  return std::nullopt;
}

std::string ErrorTextNameUse::staticUse(llvm::StringRef name) {
  return (llvm::Twine("a class expression cannot use '") + name +
          "', a variable of the function around it: the class is made "
          "once, not at each call, and its static fields get their "
          "values the first time; pass the value to its constructor, "
          "or keep it in a global")
      .str();
}

std::string ErrorTextNameUse::laterUse(llvm::StringRef name) {
  return (llvm::Twine("a class expression cannot use '") + name +
          "', a variable of the function around it: the class is made "
          "once, not at each call, and its code runs later; pass the "
          "value to its constructor, or keep it in a global")
      .str();
}

std::string ErrorTextNameUse::okValue(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' can be used as a value only where a function "
          "type is expected, e.g. 'xs.map(" +
          name + ")'")
      .str();
}

std::string ErrorTextNameUse::resultUse(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' can only be used through its functions, e.g. '" + name +
          ".combine(results)'")
      .str();
}

std::string ErrorTextNameUse::typeOnly(llvm::StringRef name, llvm::StringRef make,
                                       llvm::StringRef test) {
  return (llvm::Twine("'") + name + "' can be used only as a type; make a Result with '" + make +
          "', and test one with '" + test + "'")
      .str();
}

std::string ErrorTextNameUse::noThis() {
  return "'this' exists only in the methods of classes, which are not supported yet";
}

std::string ErrorTextNameUse::builtin(llvm::StringRef name, llvm::StringRef returns) {
  return (llvm::Twine("'") + name +
          "' can be used as a value only where a function type "
          "is expected, e.g. 'const f: (s: string) => " +
          returns + " = " + name + "'")
      .str();
}

std::string ErrorTextNameUse::superUse() {
  return "'super' is not a value: call the constructor of the class "
         "extended, 'super(...)', or a method of it, 'super.m(...)'";
}

std::string ErrorTextNameUse::arguments() {
  return "'arguments' is not supported; use a rest parameter, e.g. '...args: int[]'";
}

std::string ErrorTextNameUse::decimal() {
  return "'decimal' is used as 'decimal(x)', which makes a decimal of x, or "
         "through its functions, e.g. 'decimal.max(a, b)'";
}

std::string ErrorTextNameUse::nullUse() {
  return "'null' is not supported: the only value for nothing is 'undefined'; "
         "write 'undefined' (null exists only in Json values)";
}

std::string ErrorTextNameUse::enumUse(llvm::StringRef name) {
  return (llvm::Twine("enum '") + name + "' can only be used as '" + name + ".Member' or '" + name +
          "[value]'")
      .str();
}

std::string ErrorTextNameUse::afterCall(llvm::StringRef name, llvm::StringRef early) {
  return (llvm::Twine("'") + name + "' is declared after the code that first calls '" + early +
          "', which needs to know what '" + early +
          "' returns; move the declaration up, or declare the return type")
      .str();
}

std::string ErrorTextNameUse::laterBlock(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is declared later in the block, after this function "
          "is created; move the declaration up, or declare the "
          "function with 'function', which can use it")
      .str();
}

std::string ErrorTextNameUse::beforeDecl(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is used before it is declared").str();
}

std::string ErrorTextNameUse::ownInit(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is used in its own initializer, before it has a value")
      .str();
}

std::string ErrorTextNameUse::objectUse(llvm::StringRef name, llvm::StringRef example) {
  return (llvm::Twine("'") + name + "' can only be used through its properties, e.g. '" + example +
          "'")
      .str();
}

std::string ErrorTextNameUse::notFound(llvm::StringRef name) {
  return (llvm::Twine("cannot find name '") + name + "'").str();
}

}
