#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

// Template literal types: which strings their patterns match.

// The parts of `text` the holes of a template literal type match, the
// template having `texts` around its holes, as TypeScript splits a string
// literal: each hole but the last takes the text up to where the text after
// it is found first (one character if that text is empty), the last one the
// rest. False if the texts are not there.
bool splitByTemplate(const std::u16string &text, llvm::ArrayRef<std::u16string> texts,
                     std::vector<std::u16string> &parts);

// Whether `${number}` of a template literal type matches `text`: what
// JavaScript's Number(text) reads as a finite number, as TypeScript checks;
// `value` is that number.
bool numberString(const std::u16string &text, double &value);

// Whether the part of a string that a hole of a pattern took fits the hole.
bool holeAccepts(const Type *hole, const std::u16string &part);

// Whether the string literal `text` is a string of `pattern`
// (Type::stringPattern).
bool matchesPattern(const std::u16string &text, const Type *pattern);

} // namespace tz
