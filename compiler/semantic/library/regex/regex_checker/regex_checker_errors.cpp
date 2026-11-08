#include "compiler/semantic/library/regex/regex_checker/regex_checker_errors.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker_errors.h"

namespace tz {

std::string ErrorTextRegexFlags::invalid(llvm::StringRef flags) {
  return "invalid regular expression flags '" + flags.str() +
         "': the flags are d, g, i, m, s, u, v and y, each at most once, and not "
         "u with v";
}

std::string ErrorTextRegexLit::invalid(llvm::StringRef error) {
  std::string message = error.str();
  if (!message.empty() && message[0] >= 'A' && message[0] <= 'Z') {
    message[0] = static_cast<char>(message[0] - 'A' + 'a');
  }
  if (message == "nothing to repeat" || message == "lone quantifier brackets") {
    message += "; to match the character itself, escape it with '\\'";
  }

  return "invalid regular expression: " + message;
}

std::string ErrorTextRegexMethod::testG() {
  return "'test' of a regex with the 'g' flag starts where the previous match ended "
         "(lastIndex), so the same call can give true and then false; remove 'g'";
}

std::string ErrorTextRegexMethod::compile() {
  return "'compile' is a legacy method that changes the regex in place; "
         "make a new one instead: 'new RegExp(pattern, flags)'";
}

std::string ErrorTextRegexMethod::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on type 'RegExp'";
}

std::string ErrorTextRegexMethod::notCalled(llvm::StringRef name, bool method) {
  return ErrorTextRegexMethod::noMember(name) + (method ? "; methods must be called" : "");
}

std::string ErrorTextRegExpNew::arity(size_t count) {
  return "expected " + ErrorTextArity::wanted(1, 2) + ", but got " + std::to_string(count);
}

std::string ErrorTextRegExpNew::unknown(bool flags) {
  return "'new RegExp' of a pattern known only when the program runs may fail, "
         "and 'new' gives no Result; write 'RegExp.parse(pattern" +
         std::string(flags ? ", flags" : "") + ")', which gives a Result<RegExp, RegexError>";
}

std::string ErrorTextRegExpStatic::escape() {
  return "'RegExp.escape' can be used as a value only where a function "
         "type is expected, e.g. as a callback";
}

std::string ErrorTextRegExpStatic::legacy(llvm::StringRef name) {
  return "'RegExp." + name.str() +
         "' is a legacy property that holds what the last match of any regex "
         "found; read it from the match itself, e.g. 'm[1]'";
}

std::string ErrorTextRegExpStatic::noMember(llvm::StringRef name) {
  return "property '" + name.str() + "' does not exist on 'RegExp'";
}

}
