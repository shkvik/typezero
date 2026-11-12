#include "compiler/program/library/builtin_names/library_type_table/library_type_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const LibraryTypeInfo kRows[] = {
    {
        .name = "EventListenerObject",
        .value = LibraryType::EVENT_LISTENER_OBJECT,
    },
    {
        .name = "FinishedOptions",
        .value = LibraryType::FINISHED_OPTIONS,
    },
    {
        .name = "HookCallback",
        .value = LibraryType::HOOK_CALLBACK,
    },
    {
        .name = "NativeError",
        .value = LibraryType::NATIVE_ERROR,
    },
    {
        .name = "ReadableFromOptions",
        .value = LibraryType::READABLE_FROM_OPTIONS,
    },
    {
        .name = "StreamCallback",
        .value = LibraryType::STREAM_CALLBACK,
    },
    {
        .name = "TransformHookCallback",
        .value = LibraryType::TRANSFORM_HOOK_CALLBACK,
    },
    {
        .name = "WriteChunk",
        .value = LibraryType::WRITE_CHUNK,
    },
    {
        .name = "ReadStreamOptions",
        .value = LibraryType::READ_STREAM_OPTIONS,
    },
    {
        .name = "WriteStreamOptions",
        .value = LibraryType::WRITE_STREAM_OPTIONS,
    },
    {
        .name = "HttpStatus",
        .value = LibraryType::HTTP_STATUS,
    },
    {
        .name = "RequestInit",
        .value = LibraryType::REQUEST_INIT,
    },
    {
        .name = "RequestOptions",
        .value = LibraryType::REQUEST_OPTIONS,
    },
};

}

std::optional<LibraryType> LibraryTypeTable::find(llvm::StringRef name) {
  for (const LibraryTypeInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef LibraryTypeTable::getName(LibraryType value) {
  for (const LibraryTypeInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("library type missing from the table");
}

}
