#include "compiler/semantic/library_v2/json/library_json_registry_errors.h"
#include "llvm/ADT/Twine.h"

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

std::string ErrorTextJsonStatic::reviver() {
  return "the reviver of JSON.parse is not supported";
}

std::string ErrorTextJsonStatic::parseTypes() {
  return "JSON.parse takes no type arguments";
}

std::string ErrorTextJsonStatic::typeArgs(llvm::StringRef fn) {
  return "'JSON." + fn.str() + "' takes no type arguments";
}

std::string ErrorTextJsonStatic::noMember(llvm::StringRef fn) {
  return "'JSON." + fn.str() + "' does not exist; JSON has parse and stringify";
}

std::string ErrorTextJsonStatic::voidValue() {
  return "an expression of type 'void' has no value to write as JSON";
}

std::string ErrorTextJsonStatic::toJson(llvm::StringRef cls) {
  return "JSON.stringify does not call the method toJSON of class '" + cls.str() +
         "': tz calls no code by itself; call it, e.g. "
         "'JSON.stringify(x.toJSON())'";
}

std::string ErrorTextJsonStatic::replacer() {
  return "a replacer is not supported: tz calls no code by itself; "
         "build the value to write first, and pass 'null' here";
}

std::string ErrorTextJsonStatic::space(llvm::StringRef type) {
  return "the space of JSON.stringify is a number of spaces or a "
         "string, found '" +
         type.str() + "'";
}

std::string ErrorTextEmbed::example(llvm::StringRef fn) {
  return (fn + (fn == "embedText" ? "(\"./page.html\")" : "(\"./data.json\")")).str();
}

std::string ErrorTextEmbed::literal(llvm::StringRef fn, llvm::StringRef example) {
  return "the path of " + fn.str() +
         " must be a string literal: the file is read "
         "when the program is compiled, e.g. '" +
         example.str() + "'";
}

std::string ErrorTextEmbed::backslash() {
  return "write '/' in the path, not '\\', as on every system";
}

std::string ErrorTextEmbed::relative(llvm::StringRef fn, llvm::StringRef example) {
  return "the path of " + fn.str() +
         " is relative to the file it is written in, "
         "e.g. '" +
         example.str() + "'";
}

std::string ErrorTextEmbed::unread(llvm::StringRef shown, llvm::StringRef why) {
  return "cannot read '" + shown.str() + "': " + why.str();
}

std::string ErrorTextEmbed::typeArgs(llvm::StringRef fn) {
  return "'" + fn.str() + "' takes no type arguments";
}

std::string ErrorTextEmbed::notUtf8(llvm::StringRef shown) {
  return "'" + shown.str() + "' is not text in UTF-8";
}

std::string ErrorTextEmbed::tooLarge(llvm::StringRef shown) {
  return "'" + shown.str() +
         "' is larger than the 1 GiB a Uint8Array "
         "may have";
}

std::string ErrorTextConstantJson::bom() {
  return "this text is not JSON, so JSON.parse would always fail: it "
         "starts with a byte order mark (BOM)";
}

std::string ErrorTextConstantJson::invalid(llvm::StringRef problem, size_t line, size_t column) {
  return "this text is not JSON, so JSON.parse would always fail: " + problem.str() + " (line " +
         std::to_string(line) + ", column " + std::to_string(column) + ")";
}

}
