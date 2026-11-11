#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"

namespace tz {

namespace {

const ReflectFunctionInfo kRows[] = {
    {
        .name = "get",
        .value = ReflectFunction::GET,
    },
    {
        .name = "apply",
        .value = ReflectFunction::APPLY,
    },
    {
        .name = "construct",
        .value = ReflectFunction::CONSTRUCT,
    },
    {
        .name = "fill",
        .value = ReflectFunction::FILL,
    },
    {
        .name = "defineMetadata",
        .value = ReflectFunction::DEFINE_METADATA,
    },
    {
        .name = "getMetadata",
        .value = ReflectFunction::GET_METADATA,
    },
    {
        .name = "getOwnMetadata",
        .value = ReflectFunction::GET_OWN_METADATA,
    },
    {
        .name = "hasMetadata",
        .value = ReflectFunction::HAS_METADATA,
    },
    {
        .name = "hasOwnMetadata",
        .value = ReflectFunction::HAS_OWN_METADATA,
    },
    {
        .name = "deleteMetadata",
        .value = ReflectFunction::DELETE_METADATA,
    },
    {
        .name = "metadata",
        .value = ReflectFunction::METADATA,
    },
    {
        .name = "getMetadataKeys",
        .value = ReflectFunction::GET_METADATA_KEYS,
    },
    {
        .name = "getOwnMetadataKeys",
        .value = ReflectFunction::GET_OWN_METADATA_KEYS,
    },
};

}

std::optional<ReflectFunction> ReflectFunctionTable::find(llvm::StringRef name) {
  for (const ReflectFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
