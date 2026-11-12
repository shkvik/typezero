#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const LibraryFunctionInfo kRows[] = {
    {
        .name = "subtleGenerateHmac",
        .value = LibraryFunction::SUBTLE_GENERATE_HMAC,
    },
    {
        .name = "subtleGenerateAes",
        .value = LibraryFunction::SUBTLE_GENERATE_AES,
    },
    {
        .name = "subtleGenerateEc",
        .value = LibraryFunction::SUBTLE_GENERATE_EC,
    },
    {
        .name = "subtleGenerateRsa",
        .value = LibraryFunction::SUBTLE_GENERATE_RSA,
    },
    {
        .name = "subtleGenerateOkp",
        .value = LibraryFunction::SUBTLE_GENERATE_OKP,
    },
    {
        .name = "subtleImportHmac",
        .value = LibraryFunction::SUBTLE_IMPORT_HMAC,
    },
    {
        .name = "subtleImportAes",
        .value = LibraryFunction::SUBTLE_IMPORT_AES,
    },
    {
        .name = "subtleImportKdf",
        .value = LibraryFunction::SUBTLE_IMPORT_KDF,
    },
    {
        .name = "subtleImportEc",
        .value = LibraryFunction::SUBTLE_IMPORT_EC,
    },
    {
        .name = "subtleImportRsa",
        .value = LibraryFunction::SUBTLE_IMPORT_RSA,
    },
    {
        .name = "subtleImportOkp",
        .value = LibraryFunction::SUBTLE_IMPORT_OKP,
    },
    {
        .name = "subtleImportAny",
        .value = LibraryFunction::SUBTLE_IMPORT_ANY,
    },
    {
        .name = "subtleUnwrapHmac",
        .value = LibraryFunction::SUBTLE_UNWRAP_HMAC,
    },
    {
        .name = "subtleUnwrapAes",
        .value = LibraryFunction::SUBTLE_UNWRAP_AES,
    },
    {
        .name = "subtleUnwrapKdf",
        .value = LibraryFunction::SUBTLE_UNWRAP_KDF,
    },
    {
        .name = "subtleUnwrapEc",
        .value = LibraryFunction::SUBTLE_UNWRAP_EC,
    },
    {
        .name = "subtleUnwrapRsa",
        .value = LibraryFunction::SUBTLE_UNWRAP_RSA,
    },
    {
        .name = "subtleUnwrapOkp",
        .value = LibraryFunction::SUBTLE_UNWRAP_OKP,
    },
    {
        .name = "subtleExportJwk",
        .value = LibraryFunction::SUBTLE_EXPORT_JWK,
    },
    {
        .name = "subtleExportBytes",
        .value = LibraryFunction::SUBTLE_EXPORT_BYTES,
    },
    {
        .name = "subtleJwkOf",
        .value = LibraryFunction::SUBTLE_JWK_OF,
    },
    {
        .name = "fetch",
        .value = LibraryFunction::FETCH,
    },
    {
        .name = "onceTarget",
        .value = LibraryFunction::ONCE_TARGET,
    },
    {
        .name = "onceWithin",
        .value = LibraryFunction::ONCE_WITHIN,
    },
    {
        .name = "onceResult",
        .value = LibraryFunction::ONCE_RESULT,
    },
    {
        .name = "onWithin",
        .value = LibraryFunction::ON_WITHIN,
    },
    {
        .name = "onResult",
        .value = LibraryFunction::ON_RESULT,
    },
    {
        .name = "streamTransformStage",
        .value = LibraryFunction::STREAM_TRANSFORM_STAGE,
    },
    {
        .name = "streamSourceStage",
        .value = LibraryFunction::STREAM_SOURCE_STAGE,
    },
    {
        .name = "streamVoidSinkStage",
        .value = LibraryFunction::STREAM_VOID_SINK_STAGE,
    },
    {
        .name = "streamSinkStage",
        .value = LibraryFunction::STREAM_SINK_STAGE,
    },
    {
        .name = "streamAsyncIterableStage",
        .value = LibraryFunction::STREAM_ASYNC_ITERABLE_STAGE,
    },
    {
        .name = "streamIterableStage",
        .value = LibraryFunction::STREAM_ITERABLE_STAGE,
    },
    {
        .name = "streamWriteNewFile",
        .value = LibraryFunction::STREAM_WRITE_NEW_FILE,
    },
    {
        .name = "streamWriteFile",
        .value = LibraryFunction::STREAM_WRITE_FILE,
    },
    {
        .name = "streamReadFile",
        .value = LibraryFunction::STREAM_READ_FILE,
    },
    {
        .name = "streamFromValue",
        .value = LibraryFunction::STREAM_FROM_VALUE,
    },
    {
        .name = "streamFromAsyncIterator",
        .value = LibraryFunction::STREAM_FROM_ASYNC_ITERATOR,
    },
    {
        .name = "streamFromIterator",
        .value = LibraryFunction::STREAM_FROM_ITERATOR,
    },
};

}

std::optional<LibraryFunction> LibraryFunctionTable::find(llvm::StringRef name) {
  for (const LibraryFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef LibraryFunctionTable::getName(LibraryFunction value) {
  for (const LibraryFunctionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("library function missing from the table");
}

}
