#pragma once

#include "compiler/program/types/intl_kind/intl_kind.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class IntlMethod {
  RESOLVED_OPTIONS,
  NUMBER_FORMAT,
  NUMBER_FORMAT_TO_PARTS,
  NUMBER_FORMAT_RANGE,
  COLLATOR_COMPARE,
  PLURAL_SELECT,
  PLURAL_SELECT_RANGE,
  RELATIVE_FORMAT,
  RELATIVE_FORMAT_TO_PARTS,
  LIST_FORMAT,
  LIST_FORMAT_TO_PARTS,
  DISPLAY_NAME_OF,
  SEGMENTER_SEGMENT,
  SEGMENTS_CONTAINING,
};

struct IntlMethodInfo {
  llvm::StringLiteral name;
  IntlKind owner;
  IntlMethod method;
};

class IntlMethodTable {
public:
  static const IntlMethodInfo *find(IntlKind owner, llvm::StringRef name);
};

}
