#include "compiler/semantic/library/json/json_checker/json_checker_errors.h"

namespace tz {

std::string ErrorTextJsonTest::neverNull(llvm::StringRef x, bool isAnd, llvm::StringRef name) {
  return "after 'typeof " + x.str() + (isAnd ? " ==" : " !=") + " \"" + name.str() + "\"' " +
         x.str() +
         " is never null here: the typeof of null is "
         "\"null\"; drop this check";
}

std::string ErrorTextJsonTest::neverArray(llvm::StringRef x, bool isAnd, llvm::StringRef name) {
  return "after 'typeof " + x.str() + (isAnd ? " ==" : " !=") + " \"" + name.str() + "\"' " +
         x.str() +
         " is never an array here: the typeof of an array is "
         "\"array\"; drop this check";
}

std::string ErrorTextJsonKey::index(llvm::StringRef type) {
  return "a key of a Json is a string or a number, found '" + type.str() + "'";
}

std::string ErrorTextJsonKey::literal(llvm::StringRef type) {
  return "a key of a Json is a string, found '" + type.str() + "'";
}

std::string ErrorTextJsonKey::label(llvm::StringRef name) {
  return "key '" + name.str() + "'";
}

std::string ErrorTextJsonMethod::none(llvm::StringRef name, llvm::StringRef variable) {
  std::string message =
      "a Json has no method '" + name.str() + "' until a check shows what it holds";
  std::string call = "." + name.str() + "(...)";
  if (!variable.empty()) {
    std::string x = variable.str();
    message += ", e.g. 'if (Array.isArray(" + x + ")) " + x + call + "' or 'if (typeof " + x +
               " == \"string\") " + x + call + "'";
  } else {
    message += "; put it in a variable and check that, e.g. 'const items = j.items; if "
               "(Array.isArray(items)) items" +
               call + "'";
  }

  return message;
}

std::string ErrorTextJsonLiteral::spread() {
  return "a spread is not supported in a Json literal; write the "
         "keys one by one";
}

std::string ErrorTextJsonLiteral::function() {
  return "a Json holds no functions";
}

std::string ErrorTextJsonLiteral::defaulted(llvm::StringRef name) {
  return "'" + name.str() + " = ...' belongs to a destructuring pattern; write '" + name.str() +
         ": ...'";
}

std::string ErrorTextJsonNull::never(llvm::StringRef type, llvm::StringRef op) {
  return "'null' exists only in Json values, and a value of type '" + type.str() +
         "' is never null; the one value for nothing is 'undefined': '" + op.str() + " undefined'";
}

std::string ErrorTextJsonIsJson::known(llvm::StringRef type, bool json) {
  return "Json.isJson tells whether an 'unknown' value holds a Json; a value of "
         "type '" +
         type.str() + "' is known when the program is built" +
         (json ? ", and it is a Json already" : "");
}

}
