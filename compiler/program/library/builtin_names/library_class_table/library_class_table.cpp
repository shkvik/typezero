#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"

namespace tz {

namespace {

const LibraryClassInfo kRows[] = {
    {
        .name = "ConcurrentMap",
        .value = LibraryClass::CONCURRENT_MAP,
    },
    {
        .name = "CryptoKey",
        .value = LibraryClass::CRYPTO_KEY,
    },
    {
        .name = "CustomEvent",
        .value = LibraryClass::CUSTOM_EVENT,
    },
    {
        .name = "Event",
        .value = LibraryClass::EVENT,
    },
    {
        .name = "EventEmitter",
        .value = LibraryClass::EVENT_EMITTER,
    },
    {
        .name = "EventEmitterBase",
        .value = LibraryClass::EVENT_EMITTER_BASE,
    },
    {
        .name = "EventTarget",
        .value = LibraryClass::EVENT_TARGET,
    },
    {
        .name = "Headers",
        .value = LibraryClass::HEADERS,
    },
    {
        .name = "Locale",
        .value = LibraryClass::LOCALE,
    },
    {
        .name = "PipelineStage",
        .value = LibraryClass::PIPELINE_STAGE,
    },
    {
        .name = "Request",
        .value = LibraryClass::REQUEST,
    },
    {
        .name = "Response",
        .value = LibraryClass::RESPONSE,
    },
    {
        .name = "Server",
        .value = LibraryClass::SERVER,
    },
    {
        .name = "Socket",
        .value = LibraryClass::SOCKET,
    },
    {
        .name = "Stream",
        .value = LibraryClass::STREAM,
    },
    {
        .name = "StreamEmitter",
        .value = LibraryClass::STREAM_EMITTER,
    },
    {
        .name = "SubtleCrypto",
        .value = LibraryClass::SUBTLE_CRYPTO,
    },
    {
        .name = "URL",
        .value = LibraryClass::URL,
    },
};

}

llvm::StringRef LibraryClassTable::getName(LibraryClass value) {
  for (const LibraryClassInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }

  return "";
}

bool LibraryClassTable::isName(llvm::StringRef name, LibraryClass value) {
  llvm::StringRef expected = LibraryClassTable::getName(value);

  return name == expected;
}

}
