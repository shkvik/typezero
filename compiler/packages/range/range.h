#pragma once

#include "compiler/packages/version/version.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz::pkg {

class Range {
public:
  static bool parse(llvm::StringRef text, Range &out, std::string &error);
  bool satisfies(const Version &version) const;
  const std::string &text() const { return text_; }

private:
  struct Comparator {
    enum Op { Less, LessEq, Greater, GreaterEq, Equal } op;
    Version version;
    bool synthetic = false;
  };
  using Set = std::vector<Comparator>;
  static bool parseSet(llvm::StringRef text, Set &out, std::string &error);
  static bool test(const Set &set, const Version &version);

  std::vector<Set> sets_;
  std::string text_;
};

}
