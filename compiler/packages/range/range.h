#pragma once

#include "compiler/packages/version/version.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz::pkg {

// A range of versions as npm and JSR write them: "^1.2.3", "~1.2", "1.x",
// "1.2.3", ">=1.2.0 <2", "1.2 - 1.4", "^1 || ^2", "*". As in npm, a
// prerelease satisfies a range only if the range names a prerelease of the
// same major.minor.patch: "^1.2.0" does not take "1.3.0-beta".
class Range {
public:
  static bool parse(llvm::StringRef text, Range &out, std::string &error);
  bool satisfies(const Version &version) const;
  const std::string &text() const { return text_; }

private:
  struct Comparator {
    enum Op { Less, LessEq, Greater, GreaterEq, Equal } op;
    Version version;
    // "<2.0.0-0", the upper end of "^1.2": it names no prerelease of its own.
    bool synthetic = false;
  };
  using Set = std::vector<Comparator>; // all of them
  static bool parseSet(llvm::StringRef text, Set &out, std::string &error);
  static bool test(const Set &set, const Version &version);

  std::vector<Set> sets_; // any of them
  std::string text_;
};

} // namespace tz::pkg
