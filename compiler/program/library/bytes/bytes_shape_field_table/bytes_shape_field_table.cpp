#include "compiler/program/library/bytes/bytes_shape_field_table/bytes_shape_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const BytesShapeFieldInfo kRows[] = {
    {
        .name = "type",
        .value = BytesShapeField::JSON_TYPE,
    },
    {
        .name = "data",
        .value = BytesShapeField::JSON_DATA,
    },
    {
        .name = "read",
        .value = BytesShapeField::ENCODE_READ,
    },
    {
        .name = "written",
        .value = BytesShapeField::ENCODE_WRITTEN,
    },
    {
        .name = "stream",
        .value = BytesShapeField::DECODE_STREAM,
    },
    {
        .name = "fatal",
        .value = BytesShapeField::DECODER_FATAL,
    },
    {
        .name = "ignoreBOM",
        .value = BytesShapeField::DECODER_IGNORE_BOM,
    },
};

}

std::optional<BytesShapeField> BytesShapeFieldTable::find(llvm::StringRef name) {
  for (const BytesShapeFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef BytesShapeFieldTable::getName(BytesShapeField value) {
  for (const BytesShapeFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("bytes shape field missing from the table");
}

}
