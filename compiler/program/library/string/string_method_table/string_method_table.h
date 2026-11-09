#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class StringMethod {
  CHAR_CODE_AT,
  CHAR_AT,
  CODE_POINT_AT,
  AT,
  SLICE,
  SUBSTRING,
  INDEX_OF,
  LAST_INDEX_OF,
  INCLUDES,
  STARTS_WITH,
  ENDS_WITH,
  REPEAT,
  TO_STRING,
  CONCAT,
  SPLIT,
  TRIM,
  TRIM_START,
  TRIM_END,
  PAD_START,
  PAD_END,
  TO_UPPER_CASE,
  TO_LOWER_CASE,
  REPLACE,
  REPLACE_ALL,
  MATCH,
  MATCH_ALL,
  SEARCH,
  LOCALE_COMPARE,
  NORMALIZE,
};

enum class StringResult {
  INT,
  BOOL,
  STRING,
  STRINGS,
};

enum class StringMethodKind {
  TEXT,
  TEXT_OR_PATTERN,
  PATTERN,
  LOCALE,
};

struct StringCall {
  Builtin builtin;
  unsigned minArgs;
  unsigned maxArgs;
  llvm::StringLiteral args;
  StringResult result;
};

struct TextStringMethod {
  static constexpr StringMethodKind kind = StringMethodKind::TEXT;
  StringCall call;
};

struct TextOrPatternStringMethod {
  static constexpr StringMethodKind kind = StringMethodKind::TEXT_OR_PATTERN;
  StringCall call;
};

struct PatternStringMethod {
  static constexpr StringMethodKind kind = StringMethodKind::PATTERN;
};

struct LocaleStringMethod {
  static constexpr StringMethodKind kind = StringMethodKind::LOCALE;
};

using StringMethodForm =
    Union<TextStringMethod, TextOrPatternStringMethod, PatternStringMethod, LocaleStringMethod>;

struct StringMethodInfo {
  llvm::StringLiteral name;
  StringMethod method;
  StringMethodForm form;
};

class StringMethodTable {
public:
  static constexpr unsigned kAnyCount = ~0u;

  static const StringMethodInfo *find(llvm::StringRef name);
  static const StringCall *findCall(const StringMethodInfo &method);
  static llvm::ArrayRef<StringMethodInfo> getMethods();
};

}
