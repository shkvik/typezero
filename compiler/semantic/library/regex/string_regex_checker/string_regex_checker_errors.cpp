#include "compiler/semantic/library/regex/string_regex_checker/string_regex_checker_errors.h"
#include "compiler/semantic/library/regex/match_checker/match_checker_errors.h"

namespace tz {

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
