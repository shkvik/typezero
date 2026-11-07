#include "compiler/pipeline/check_program/topics/regex/match_checker/match_checker_errors.h"

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

}
