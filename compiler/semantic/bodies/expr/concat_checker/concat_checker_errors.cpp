#include "compiler/semantic/bodies/expr/concat_checker/concat_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextToString::voidValue() {
  return "an expression of type 'void' cannot be converted to a string";
}

std::string ErrorTextToString::function() {
  return "a function cannot be converted to a string; print it with "
         "console.log, or use its name";
}

std::string ErrorTextToString::tuple() {
  return "a tuple cannot be converted to a string; print it with console.log, or build "
         "the text from its elements";
}

std::string ErrorTextToString::result() {
  return "a Result cannot be converted to a string: JavaScript would give "
         "'[object Object]'; print it with console.log, or make the text with "
         "'match'";
}

std::string ErrorTextToString::ownString(llvm::StringRef text) {
  return (llvm::Twine("an object is not converted to a string by calling its toString(): "
                      "tz calls no code by itself; call it, e.g. '${") +
          text + ".toString()}'")
      .str();
}

std::string ErrorTextToString::dict(llvm::StringRef kind, llvm::StringRef shown) {
  return (llvm::Twine("a ") + kind +
          " cannot be converted to a string: JavaScript would give "
          "'[object " +
          shown +
          "]'; print it with console.log, or build the text from its "
          "entries")
      .str();
}

std::string ErrorTextToString::iterator() {
  return "an iterator cannot be converted to a string: JavaScript would give "
         "'[object Generator]'; collect its values first, e.g. "
         "'[...it].join()'";
}

std::string ErrorTextToString::unknown() {
  return "an unknown value cannot be converted to a string: it may hold an "
         "object or a function; check what it holds first, e.g. 'typeof x == "
         "\"string\"'";
}

std::string ErrorTextToString::json() {
  return "a Json cannot be converted to a string: it may hold an object; check what it "
         "holds first (e.g. 'typeof x == \"string\"'), or write it as JSON with "
         "'JSON.stringify(x)'";
}

std::string ErrorTextToString::object() {
  return "an object cannot be converted to a string: JavaScript would give "
         "'[object Object]'; print it with console.log, or build the text from "
         "its fields";
}

std::string ErrorTextToString::member(llvm::StringRef type, llvm::StringRef member) {
  return (llvm::Twine("a value of type '") + type +
          "' cannot be converted to a string: it may hold '" + member +
          "'; check what it holds first")
      .str();
}

std::string ErrorTextMixedConcat::operands(llvm::StringRef lhs, llvm::StringRef rhs) {
  return (llvm::Twine("operator '+' cannot be applied to types '") + lhs + "' and '" + rhs +
          "' (JavaScript would join them as text)")
      .str();
}

std::string ErrorTextMixedConcat::literal() {
  return "; use a template literal instead";
}

std::string ErrorTextMixedConcat::convert(llvm::StringRef only) {
  return (llvm::Twine("; convert it with 'String(") + only + ")'").str();
}

std::string ErrorTextMixedConcat::addOrJoin(llvm::StringRef sum, llvm::StringRef joined) {
  return (llvm::Twine("; to add, convert the string: '") + sum + "'; to join, use a template: '" +
          joined + "'")
      .str();
}

std::string ErrorTextMixedConcat::asTemplate(llvm::StringRef joined) {
  return (llvm::Twine("; use a template: '") + joined + "'").str();
}

std::string ErrorTextMixedConcat::addRight(llvm::StringRef first, llvm::StringRef second) {
  return (llvm::Twine(first) + " + Number(" + second + ")").str();
}

std::string ErrorTextMixedConcat::addLeft(llvm::StringRef first, llvm::StringRef second) {
  return (llvm::Twine("Number(") + first + ") + " + second).str();
}

}
