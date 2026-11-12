#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class DateMethodForm {
  GET_TIME,
  GET_FIELD,
  SET_TIME,
  SET_FIELDS,
  TO_JSON,
  TO_TEXT,
  TO_FIELD_TEXT,
  TO_LOCALE,
};

struct DateMethodInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  DateMethodForm form;
  int field;
  unsigned min;
  unsigned max;
};

class DateMethodTable {
public:
  static const DateMethodInfo *find(llvm::StringRef name);
  static llvm::StringRef getName(Builtin builtin, int field);
};

}
