#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const BuiltinPropertyInfo kBuiltinProperties[] = {
    {
        .name = "length",
        .property = BuiltinProperty::LENGTH,
    },
    {
        .name = "byteLength",
        .property = BuiltinProperty::BYTE_LENGTH,
    },
    {
        .name = "byteOffset",
        .property = BuiltinProperty::BYTE_OFFSET,
    },
    {
        .name = "buffer",
        .property = BuiltinProperty::BUFFER,
    },
    {
        .name = "BYTES_PER_ELEMENT",
        .property = BuiltinProperty::BYTES_PER_ELEMENT,
    },
    {
        .name = "maxByteLength",
        .property = BuiltinProperty::MAX_BYTE_LENGTH,
    },
    {
        .name = "resizable",
        .property = BuiltinProperty::RESIZABLE,
    },
    {
        .name = "detached",
        .property = BuiltinProperty::DETACHED,
    },
    {
        .name = "encoding",
        .property = BuiltinProperty::ENCODING,
    },
    {
        .name = "fatal",
        .property = BuiltinProperty::FATAL,
    },
    {
        .name = "ignoreBOM",
        .property = BuiltinProperty::IGNORE_BOM,
    },
    {
        .name = "parent",
        .property = BuiltinProperty::PARENT,
    },
    {
        .name = "offset",
        .property = BuiltinProperty::OFFSET,
    },
    {
        .name = "poolSize",
        .property = BuiltinProperty::POOL_SIZE,
    },
    {
        .name = "size",
        .property = BuiltinProperty::SIZE,
    },
    {
        .name = "value",
        .property = BuiltinProperty::RESULT_VALUE,
    },
    {
        .name = "error",
        .property = BuiltinProperty::RESULT_ERROR,
    },
};

}

bool BuiltinPropertyTable::isName(llvm::StringRef name, BuiltinProperty property) {
  llvm::StringRef propertyName = BuiltinPropertyTable::getName(property);

  return name == propertyName;
}

std::optional<BuiltinProperty> BuiltinPropertyTable::find(llvm::StringRef name) {
  for (const BuiltinPropertyInfo &info : kBuiltinProperties) {
    if (info.name == name) {
      return info.property;
    }
  }

  return std::nullopt;
}

llvm::StringRef BuiltinPropertyTable::getName(BuiltinProperty property) {
  for (const BuiltinPropertyInfo &info : kBuiltinProperties) {
    if (info.property == property) {
      return info.name;
    }
  }
  llvm_unreachable("builtin property missing from the table");
}

}
