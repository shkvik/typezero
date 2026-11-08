#include "compiler/semantic/bodies/stmt/var_checker/var_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextVarInit::isVoid() {
  return "an expression of type 'void' cannot initialize a variable";
}

std::string ErrorTextVarInit::undefined(bool isConst, llvm::StringRef name) {
  return (llvm::Twine("cannot infer a type from 'undefined'; add one, e.g. '") +
          (isConst ? "const " : "let ") + name + ": int | undefined = undefined'")
      .str();
}

std::string ErrorTextConstValue::missing() {
  return "'const' declarations must be initialized";
}

std::string ErrorTextVarShape::notArray(llvm::StringRef type) {
  return (llvm::Twine("only arrays can be destructured, found '") + type + "'").str();
}

std::string ErrorTextVarShape::mayBeUndef(llvm::StringRef type) {
  return (llvm::Twine("only arrays can be destructured, found '") + type +
          "'; the array may be undefined: check it first")
      .str();
}

std::string ErrorTextResultUse::promise(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' holds a Promise of a Result that is never awaited, "
          "so an error in it would go unchecked; await it and "
          "check it, e.g. 'const r = await " +
          name + "; if (r.isErr()) ...'")
      .str();
}

std::string ErrorTextResultUse::unchecked(llvm::StringRef name) {
  return (llvm::Twine("the Result in '") + name +
          "' is never checked, so an error in it would go "
          "unchecked; check it, e.g. 'if (" +
          name + ".isErr()) ...'")
      .str();
}

std::string ErrorTextVarTyped::writeType(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("write the type: 'let ") + name + ": " + type + "'").str();
}

std::string ErrorTextVarTyped::addOne(llvm::StringRef name) {
  return (llvm::Twine("add one, e.g. 'let ") + name + ": int | undefined'").str();
}

std::string ErrorTextVarTyped::example(llvm::StringRef name) {
  return (llvm::Twine("e.g. 'let ") + name + ": int' or 'let " + name + " = 0'").str();
}

std::string ErrorTextVarTyped::undefined(llvm::StringRef hint) {
  return (llvm::Twine("cannot infer a type from 'undefined'; ") + hint).str();
}

std::string ErrorTextVarTyped::needsType(llvm::StringRef name, llvm::StringRef hint) {
  return (llvm::Twine("variable '") + name + "' needs a type annotation or an initializer; " + hint)
      .str();
}

}
