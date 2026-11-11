#pragma once

#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"

namespace tz {

enum class BufferStaticKind {
  FROM,
  ALLOC,
  ALLOC_UNSAFE,
  CONCAT,
  BYTE_LENGTH,
  COMPARE,
  IS_BUFFER,
  IS_ENCODING,
};

struct BufferStaticInfo {
  BytesMethod method;
  BufferStaticKind kind;
};

class BufferStaticTable {
public:
  static const BufferStaticInfo *find(BytesMethod method);
};

}
