#include "compiler/semantic/library/json/json_static_checker/json_static_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

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

std::string ErrorTextConstantJson::bom() {
  return "this text is not JSON, so JSON.parse would always fail: it "
         "starts with a byte order mark (BOM)";
}

std::string ErrorTextConstantJson::invalid(llvm::StringRef problem, size_t line, size_t column) {
  return "this text is not JSON, so JSON.parse would always fail: " + problem.str() + " (line " +
         std::to_string(line) + ", column " + std::to_string(column) + ")";
}

}
