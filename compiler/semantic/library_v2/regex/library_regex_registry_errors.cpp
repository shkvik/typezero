#include "compiler/semantic/library_v2/regex/library_regex_registry_errors.h"

namespace tz {

std::string ErrorTextUnknownRegex::text() {
  return "the regex is known only when the program runs (it is not a literal, nor a variable that "
         "holds one all along)";
}

std::string ErrorTextMatch::groups(bool known) {
  if (!known) {
    return std::string("the named groups of a match are not supported "
                       "yet when ") +
           ErrorTextUnknownRegex::text() + "; read the groups by number: 'm[1]'";
  }

  return "the regex has no named groups; name one with '(?<name>...)'";
}

std::string ErrorTextMatch::indices(bool known) {
  return !known ? std::string("'indices' of a match needs the d flag, and ") +
                      ErrorTextUnknownRegex::text()
                : "a match has 'indices' only with the d flag: add 'd' to "
                  "the flags of the regex";
}

std::string ErrorTextMatch::noMember(llvm::StringRef name, llvm::StringRef type) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'";
}

std::string ErrorTextMatch::noGroup(int groups, int64_t index) {
  return "the regex has " + std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
         (groups ? " (m[1] to m[" + std::to_string(groups) + "]; m[0] is the match)"
                 : " (m[0] is the match)") +
         ", so there is no m[" + std::to_string(index) + "]";
}

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

std::string ErrorTextStringRegex::matchFlag() {
  return "what 'match' gives depends on the g flag (all the texts that "
         "match, or one match with its groups), and " +
         ErrorTextUnknownRegex::text() +
         "; for one match write 're.exec(s)', for all of them "
         "'s.matchAll(re)'";
}

std::string ErrorTextStringRegex::needsG(llvm::StringRef name, llvm::StringRef pattern,
                                         llvm::StringRef flags) {
  return "'" + name.str() +
         "' needs a regex with the g flag (JavaScript throws a "
         "TypeError without it), e.g. '/" +
         pattern.str() + "/" + flags.str() + "g'";
}

std::string ErrorTextStringRegex::escapedRe(llvm::StringRef text) {
  return "new RegExp(RegExp.escape(" + text.str() + "))";
}

std::string ErrorTextStringRegex::functionRe(llvm::StringRef escaped, llvm::StringRef text) {
  return "a function as the replacement needs a regex: 'replaceAll(" +
         (!escaped.empty() ? escaped.str() + "g"
                           : "new RegExp(RegExp.escape(" + text.str() + "), \"g\")") +
         ", ...)'";
}

std::string ErrorTextStringRegex::replaceStr(llvm::StringRef text, llvm::StringRef regex) {
  return "'replace' with a string replaces only the first occurrence (\"a-b-c\".replace(\"-\", "
         "\"+\") is \"a+b-c\" in JavaScript); to replace all of them write 'replaceAll(" +
         text.str() + ", ...)', for only the first one a regex: 'replace(" + regex.str() +
         ", ...)'";
}

std::string ErrorTextStringRegex::searchStr(llvm::StringRef text, llvm::StringRef regex,
                                            bool literal) {
  return "'search' takes a regex, and JavaScript makes one of a string, "
         "where '.' matches any character; to find the text write "
         "'indexOf(" +
         text.str() + ")', for a regex '" +
         (literal ? regex.str() : "search(" + regex.str() + ")") + "'";
}

std::string ErrorTextStringRegex::includeStr(llvm::StringRef name, llvm::StringRef text,
                                             llvm::StringRef regex, bool literal) {
  return "'" + name.str() +
         "' takes a regex, and JavaScript makes one of a string, where "
         "'.' matches any character; to check for the text write "
         "'includes(" +
         text.str() + ")', for a regex '" +
         (literal ? regex.str() + (name == "matchAll" ? "g" : "")
                  : name.str() + "(" + regex.str() + ")") +
         "'";
}

std::string ErrorTextStringRegex::notRegex(llvm::StringRef name, bool orString,
                                           llvm::StringRef type) {
  return "'" + name.str() + "' takes a regex" + (orString ? " or a string" : "") + ", found '" +
         type.str() + "'";
}

std::string ErrorTextStringRegex::noGroup(uint32_t groups, uint32_t one) {
  return "the regex has " + std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
         ", so there is no group " + std::to_string(one) + " for '$" + std::to_string(one) +
         "' (JavaScript would insert it as it is); write '$$' for '$'";
}

std::string ErrorTextStringRegex::noNamed(bool named, llvm::StringRef group) {
  return named ? "the regex has no group named '" + group.str() + "' for '$<" + group.str() + ">'"
               : "the regex has no named groups, so JavaScript would insert '$<" + group.str() +
                     ">' as it is; name a group with '(?<" + group.str() + ">...)'";
}

std::string ErrorTextStringRegex::onlyMatch() {
  return "the function gets only the match: " + ErrorTextUnknownRegex::text() +
         ", so its groups are not known when compiling";
}

std::string ErrorTextStringRegex::passes(llvm::StringRef name, size_t count, int groups,
                                         bool namedGroups) {
  return "'" + name.str() + "' passes " + std::to_string(count) + " arguments: the match, " +
         std::to_string(groups) + " group" + (groups == 1 ? "" : "s") +
         ", the offset and the string" + (namedGroups ? ", and the named groups" : "");
}

}
