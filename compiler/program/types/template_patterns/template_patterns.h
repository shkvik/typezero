#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

class TemplatePatterns {
public:
  static bool numberString(const std::u16string &text, double &value);
  static bool holeAccepts(const Type *hole, const std::u16string &part);
  static bool matchesPattern(const std::u16string &text, const Type *pattern);
  static bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                              std::vector<std::u16string> &parts);

private:
};

}
