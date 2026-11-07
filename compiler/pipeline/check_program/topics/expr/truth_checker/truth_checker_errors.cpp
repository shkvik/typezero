#include "compiler/pipeline/check_program/topics/expr/truth_checker/truth_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextTruthy::unionTrue(llvm::StringRef type) {
  return (llvm::Twine("a value of type '") + type +
          "' is always truthy, so the condition is always true")
      .str();
}

std::string ErrorTextTruthy::unionNum(llvm::StringRef type, llvm::StringRef number,
                                      llvm::StringRef value) {
  return (llvm::Twine("a value of type '") + type + "' cannot be a condition: '" + number +
          "' is a number, and 0 would count as false; check what it "
          "holds, e.g. 'typeof " +
          value + " == \"string\"'")
      .str();
}

std::string ErrorTextTruthy::cannotTest(llvm::StringRef op, llvm::StringRef what) {
  if (op.empty()) {
    return (llvm::Twine(what) + " cannot be a condition").str();
  }

  return (llvm::Twine("'") + op + "' cannot test " + what).str();
}

std::string ErrorTextTruthy::typeValue(llvm::StringRef type) {
  return (llvm::Twine("a value of type '") + type + "'").str();
}

std::string ErrorTextTruthy::voidValue() {
  return "an expression of type 'void' has no value to test";
}

std::string ErrorTextTruthy::undefined() {
  return "the value is always undefined, which is always falsy";
}

std::string ErrorTextTruthy::mayBeNum(llvm::StringRef base) {
  return (llvm::Twine(": '") + base + "' may be a number, and 0 would count as false; ").str();
}

std::string ErrorTextTruthy::orDefault(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("give a default with '??': '") + x + " ?? " + y + "'").str();
}

std::string ErrorTextTruthy::orAssign(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("give a default with '??=': '") + x + " ??= " + y + "'").str();
}

std::string ErrorTextTruthy::andAssign(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("check for undefined: 'if (") + x + " != undefined) " + x + " = " + y + "'")
      .str();
}

std::string ErrorTextTruthy::undefTest(llvm::StringRef x, bool negated) {
  return (llvm::Twine("check for undefined: '") + x + (negated ? " == " : " != ") + "undefined'")
      .str();
}

std::string ErrorTextTruthy::jsonHead(llvm::StringRef op) {
  return ErrorTextTruthy::cannotTest(op, "a Json");
}

std::string ErrorTextTruthy::jsonMay() {
  return ": it may hold 0, \"\", false or null, which would count as false; ";
}

std::string ErrorTextTruthy::jsonOr(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("give a default with '??', which replaces null and undefined: '") + x +
          " ?? " + y + "'")
      .str();
}

std::string ErrorTextTruthy::jsonTest(llvm::StringRef x, bool negated) {
  return (llvm::Twine("compare it as you mean, e.g. '") + x +
          (negated ? " == null'" : " != null'") + ", '" + x +
          (negated ? " != true'" : " == true'") + " or 'typeof " + x + " == \"string\"'")
      .str();
}

std::string ErrorTextTruthy::unknownHd(llvm::StringRef op) {
  return ErrorTextTruthy::cannotTest(op, "an unknown value");
}

std::string ErrorTextTruthy::unknownMay() {
  return ": it may hold 0, \"\" or false, which would count as false; ";
}

std::string ErrorTextTruthy::unknownTst(llvm::StringRef x, bool negated) {
  return (llvm::Twine("check what it holds, e.g. '") + x +
          (negated ? " == undefined'" : " != undefined'") + " or 'typeof " + x + " == \"string\"'")
      .str();
}

std::string ErrorTextTruthy::result(llvm::StringRef x, bool negated) {
  return (llvm::Twine(
              "a Result is always truthy, an Err too, as both are objects in JavaScript; test "
              "it: '") +
          x + (negated ? ".isErr()'" : ".isOk()'"))
      .str();
}

std::string ErrorTextTruthy::number(bool decimal, bool optional) {
  std::string noun = decimal ? "a decimal" : "a number";

  return optional ? noun + " that may be undefined" : noun;
}

std::string ErrorTextTruthy::zero() {
  return ": 0 would count as false";
}

std::string ErrorTextTruthy::too() {
  return " too; ";
}

std::string ErrorTextTruthy::lengthTest(llvm::StringRef x, bool negated) {
  return (llvm::Twine("compare it: '(") + x + " ?? 0) " + (negated ? "== 0'" : "> 0'")).str();
}

std::string ErrorTextTruthy::floatTest(llvm::StringRef x, bool negated) {
  return (llvm::Twine("; compare it as you mean: '") + x +
          (negated ? " == 0' or '" : " != 0' or '") + x + (negated ? " <= 0'" : " > 0'"))
      .str();
}

std::string ErrorTextTruthy::zeroTest(llvm::StringRef x, bool negated, bool length) {
  if (negated) {
    return (llvm::Twine(x) + " == 0").str();
  }

  return (llvm::Twine(x) + (length ? " > 0" : " != 0")).str();
}

std::string ErrorTextTruthy::orCompare(llvm::StringRef test, llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("; compare it: '") + test + " ? " + x + " : " + y + "'").str();
}

std::string ErrorTextTruthy::orAssignIf(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("; compare it: 'if (") + x + " == 0) " + x + " = " + y + "'").str();
}

std::string ErrorTextTruthy::andIf(llvm::StringRef test, llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("; compare it: 'if (") + test + ") " + x + " = " + y + "'").str();
}

std::string ErrorTextTruthy::compare(llvm::StringRef test) {
  return (llvm::Twine("; compare it: '") + test + "'").str();
}

std::string ErrorTextTruthy::kind(bool array, bool promise, bool cls, bool object) {
  if (array) {
    return "an array";
  }
  if (promise) {
    return "a Promise";
  }
  if (cls) {
    return "a class";
  }
  if (object) {
    return "an object";
  }

  return "a function";
}

std::string ErrorTextTruthy::holds(llvm::StringRef name, llvm::StringRef what) {
  return (llvm::Twine("'") + name + "' always holds " + what + " at this point").str();
}

std::string ErrorTextTruthy::arrayTrue() {
  return "an array is always truthy, even when empty";
}

std::string ErrorTextTruthy::alwaysTrue(llvm::StringRef what) {
  return (llvm::Twine(what) + " is always truthy").str();
}

std::string ErrorTextTruthy::soAlways() {
  return ", so the condition is always true";
}

std::string ErrorTextTruthy::soNot() {
  return ", so '!' always gives false";
}

std::string ErrorTextTruthy::soOr() {
  return ", so '||' never gives its right side";
}

std::string ErrorTextTruthy::soAnd() {
  return ", so '&&' always gives its right side";
}

std::string ErrorTextTruthy::soOrSet() {
  return ", so '||=' never assigns";
}

std::string ErrorTextTruthy::soAndSet() {
  return ", so '&&=' always assigns";
}

std::string ErrorTextTruthy::lengthCmp(llvm::StringRef x, bool negated) {
  return (llvm::Twine(x) + (negated ? ".length == 0" : ".length > 0")).str();
}

std::string ErrorTextTruthy::callOf(llvm::StringRef x, bool negated, bool noArgs) {
  return (llvm::Twine((negated ? "!" : "")) + x + (noArgs ? "()" : "(...)")).str();
}

std::string ErrorTextTruthy::useAssign() {
  return "; use '='";
}

std::string ErrorTextTruthy::dropOr(llvm::StringRef y) {
  return (llvm::Twine("; drop '|| ") + y + "'").str();
}

std::string ErrorTextTruthy::arrayLen(llvm::StringRef x, llvm::StringRef y) {
  return (llvm::Twine("; check its length: 'if (") + x + ".length == 0) " + x + " = " + y + "'")
      .str();
}

std::string ErrorTextTruthy::dropAssign() {
  return "; drop the assignment";
}

std::string ErrorTextTruthy::dropAnd(llvm::StringRef x) {
  return (llvm::Twine("; drop '") + x + " &&'").str();
}

std::string ErrorTextTruthy::dropOrLen(llvm::StringRef length) {
  return (llvm::Twine("; drop the check, or check the length: '") + length + "'").str();
}

std::string ErrorTextTruthy::dropCheck() {
  return "; drop the check";
}

std::string ErrorTextTruthy::checkLen(llvm::StringRef length) {
  return (llvm::Twine("; check its length: '") + length + "'").str();
}

std::string ErrorTextTruthy::allowUndef(llvm::StringRef base) {
  return (llvm::Twine("; drop the check, or give it a type that allows undefined: '") + base +
          " | undefined'")
      .str();
}

std::string ErrorTextTruthy::callIt(llvm::StringRef call) {
  return (llvm::Twine("; call it: '") + call + "'").str();
}

std::string ErrorTextLogical::rightVoid(llvm::StringRef op, bool isAnd, llvm::StringRef test,
                                        llvm::StringRef rhs) {
  return (llvm::Twine("the right side of '") + op +
          "' returns nothing, so there is no value to give; call it with 'if': 'if (" +
          (isAnd ? "" : "!") + test + ") " + rhs + "'")
      .str();
}

std::string ErrorTextLogical::noCommon(llvm::StringRef op, llvm::StringRef kept, bool isAnd,
                                       llvm::StringRef other) {
  return (llvm::Twine("'") + op + "' gives '" + kept + "' when its left side is " +
          (isAnd ? "falsy" : "truthy") + " and '" + other +
          "' otherwise, and no type holds both; say what each case gives with '?:'")
      .str();
}

std::string ErrorTextAssignTest::compare(llvm::StringRef eq, llvm::StringRef target,
                                         llvm::StringRef value) {
  return (llvm::Twine("an assignment cannot be a condition; to compare, use '") + eq + "': '" +
          target + " " + eq + " " + value + "'")
      .str();
}

std::string ErrorTextAssignTest::testValue(llvm::StringRef target, llvm::StringRef value,
                                           bool negated, bool optional, bool text) {
  std::string empty = optional ? "undefined" : text ? "\"\"" : "0";

  return (llvm::Twine(
              "an assignment cannot be a condition; to test the assigned value, compare it: '(") +
          target + " = " + value + ") " + (negated ? "== " : "!= ") + empty + "'")
      .str();
}

}
