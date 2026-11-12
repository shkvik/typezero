#pragma once

#include "compiler/program/types/intl_kind/intl_kind.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

enum class IntlShape {
  RELATIVE_TIME_FORMAT_PART,
  SEGMENT_DATA,
  DATE_TIME_FORMAT_PART,
  RESOLVED_NUMBER_FORMAT,
  RESOLVED_COLLATOR,
  RESOLVED_PLURAL_RULES,
  RESOLVED_RELATIVE_TIME_FORMAT,
  RESOLVED_LIST_FORMAT,
  RESOLVED_DISPLAY_NAMES,
  RESOLVED_SEGMENTER,
  RESOLVED_DATE_TIME_FORMAT,
};

enum class IntlValueType {
  TEXT,
  MAYBE_TEXT,
  FLAG,
  MAYBE_FLAG,
  INT,
  MAYBE_INT,
  TEXT_OR_FLAG,
  TEXT_LIST,
};

struct IntlShapeField {
  llvm::StringLiteral name;
  IntlValueType type;
};

struct IntlShapeInfo {
  IntlShape shape;
  llvm::StringLiteral typeName;
  std::vector<IntlShapeField> fields;
};

class IntlShapeTable {
public:
  static const IntlShapeInfo &find(IntlShape shape);
  static IntlShape resolvedOf(IntlKind kind);
  static llvm::StringRef getLocaleTagField();
};

}
