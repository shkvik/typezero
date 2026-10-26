#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                     std::vector<std::u16string> &parts);

bool numberString(const std::u16string &text, double &value);

bool holeAccepts(const Type *hole, const std::u16string &part);

bool matchesPattern(const std::u16string &text, const Type *pattern);

}
