#include "compiler/program/library/reflect/builtin_constructor_table/builtin_constructor_table.h"

namespace tz {

namespace {

const BuiltinConstructorInfo kRows[] = {
    {
        .name = "Number",
        .value = BuiltinConstructor::NUMBER,
    },
    {
        .name = "String",
        .value = BuiltinConstructor::STRING,
    },
    {
        .name = "Boolean",
        .value = BuiltinConstructor::BOOLEAN,
    },
    {
        .name = "Object",
        .value = BuiltinConstructor::OBJECT,
    },
    {
        .name = "Array",
        .value = BuiltinConstructor::ARRAY,
    },
    {
        .name = "Function",
        .value = BuiltinConstructor::FUNCTION,
    },
    {
        .name = "Date",
        .value = BuiltinConstructor::DATE,
    },
    {
        .name = "RegExp",
        .value = BuiltinConstructor::REG_EXP,
    },
    {
        .name = "Map",
        .value = BuiltinConstructor::MAP,
    },
    {
        .name = "Set",
        .value = BuiltinConstructor::SET,
    },
    {
        .name = "Promise",
        .value = BuiltinConstructor::PROMISE,
    },
    {
        .name = "Uint8Array",
        .value = BuiltinConstructor::UINT8_ARRAY,
    },
    {
        .name = "Int8Array",
        .value = BuiltinConstructor::INT8_ARRAY,
    },
    {
        .name = "Uint8ClampedArray",
        .value = BuiltinConstructor::UINT8_CLAMPED_ARRAY,
    },
    {
        .name = "Int16Array",
        .value = BuiltinConstructor::INT16_ARRAY,
    },
    {
        .name = "Uint16Array",
        .value = BuiltinConstructor::UINT16_ARRAY,
    },
    {
        .name = "Int32Array",
        .value = BuiltinConstructor::INT32_ARRAY,
    },
    {
        .name = "Uint32Array",
        .value = BuiltinConstructor::UINT32_ARRAY,
    },
    {
        .name = "Float16Array",
        .value = BuiltinConstructor::FLOAT16_ARRAY,
    },
    {
        .name = "Float32Array",
        .value = BuiltinConstructor::FLOAT32_ARRAY,
    },
    {
        .name = "Float64Array",
        .value = BuiltinConstructor::FLOAT64_ARRAY,
    },
    {
        .name = "Buffer",
        .value = BuiltinConstructor::BUFFER,
    },
    {
        .name = "ArrayBuffer",
        .value = BuiltinConstructor::ARRAY_BUFFER,
    },
    {
        .name = "DataView",
        .value = BuiltinConstructor::DATA_VIEW,
    },
    {
        .name = "TextEncoder",
        .value = BuiltinConstructor::TEXT_ENCODER,
    },
    {
        .name = "TextDecoder",
        .value = BuiltinConstructor::TEXT_DECODER,
    },
};

}

llvm::StringRef BuiltinConstructorTable::getName(BuiltinConstructor value) {
  for (const BuiltinConstructorInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }

  return "";
}

std::optional<BuiltinConstructor> BuiltinConstructorTable::find(llvm::StringRef name) {
  for (const BuiltinConstructorInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
