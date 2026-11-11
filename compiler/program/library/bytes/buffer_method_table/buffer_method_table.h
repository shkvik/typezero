#pragma once

#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/union/union/union.h"

namespace tz {

enum class BufferMethodKind {
  SLICE,
  TO_STRING,
  TO_JSON,
  EQUALS,
  COMPARE,
  COPY,
  WRITE,
  FILL,
  SEARCH,
  SWAP,
};

struct BufferSliceMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::SLICE;
};

struct BufferToStringMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::TO_STRING;
};

struct BufferToJsonMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::TO_JSON;
};

struct BufferEqualsMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::EQUALS;
};

struct BufferCompareMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::COMPARE;
};

struct BufferCopyMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::COPY;
};

struct BufferWriteMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::WRITE;
};

struct BufferFillMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::FILL;
};

struct BufferSearchMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::SEARCH;
  BytesSearchMode mode;
};

struct BufferSwapMethod {
  static constexpr BufferMethodKind kind = BufferMethodKind::SWAP;
  int width;
};

using BufferMethodForm =
    Union<BufferSliceMethod, BufferToStringMethod, BufferToJsonMethod, BufferEqualsMethod,
          BufferCompareMethod, BufferCopyMethod, BufferWriteMethod, BufferFillMethod,
          BufferSearchMethod, BufferSwapMethod>;

struct BufferMethodInfo {
  BytesMethod method;
  BufferMethodForm form;
};

class BufferMethodTable {
public:
  static const BufferMethodInfo *find(BytesMethod method);
};

}
