#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class IntlTypeName {
  DATE_TIME_FORMAT,
  SEGMENTS,
  SEGMENT_DATA,
  DATE_TIME_FORMAT_PART,
  NUMBER_FORMAT_PART,
  RELATIVE_TIME_FORMAT_PART,
  RESOLVED_DATE_TIME_FORMAT_OPTIONS,
  DATE_TIME_FORMAT_OPTIONS,
  LDML_PLURAL_RULE,
  RELATIVE_TIME_FORMAT_UNIT,
  UNICODE_BCP47_LOCALE_IDENTIFIER,
  BCP47_LANGUAGE_TAG,
  LOCALE,
};

struct IntlTypeNameInfo {
  llvm::StringLiteral name;
  IntlTypeName value;
};

class IntlTypeNameTable {
public:
  static std::optional<IntlTypeName> find(llvm::StringRef name);
  static llvm::StringRef getName(IntlTypeName value);
};

}
