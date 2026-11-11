#include "compiler/program/library/bytes/buffer_method_table/buffer_method_table.h"

namespace tz {

namespace {

const BufferMethodInfo kBufferMethods[] = {
    {
        .method = BytesMethod::SLICE,
        .form = BufferSliceMethod{},
    },
    {
        .method = BytesMethod::TO_STRING,
        .form = BufferToStringMethod{},
    },
    {
        .method = BytesMethod::TO_LOCALE_STRING,
        .form = BufferToStringMethod{},
    },
    {
        .method = BytesMethod::TO_JSON,
        .form = BufferToJsonMethod{},
    },
    {
        .method = BytesMethod::EQUALS,
        .form = BufferEqualsMethod{},
    },
    {
        .method = BytesMethod::COMPARE,
        .form = BufferCompareMethod{},
    },
    {
        .method = BytesMethod::COPY,
        .form = BufferCopyMethod{},
    },
    {
        .method = BytesMethod::WRITE,
        .form = BufferWriteMethod{},
    },
    {
        .method = BytesMethod::FILL,
        .form = BufferFillMethod{},
    },
    {
        .method = BytesMethod::INDEX_OF,
        .form = BufferSearchMethod{.mode = BytesSearchMode::INDEX_OF},
    },
    {
        .method = BytesMethod::LAST_INDEX_OF,
        .form = BufferSearchMethod{.mode = BytesSearchMode::LAST_INDEX_OF},
    },
    {
        .method = BytesMethod::INCLUDES,
        .form = BufferSearchMethod{.mode = BytesSearchMode::INCLUDES},
    },
    {
        .method = BytesMethod::SWAP16,
        .form = BufferSwapMethod{.width = 2},
    },
    {
        .method = BytesMethod::SWAP32,
        .form = BufferSwapMethod{.width = 4},
    },
    {
        .method = BytesMethod::SWAP64,
        .form = BufferSwapMethod{.width = 8},
    },
};

}

const BufferMethodInfo *BufferMethodTable::find(BytesMethod method) {
  for (const BufferMethodInfo &info : kBufferMethods) {
    if (info.method == method) {
      return &info;
    }
  }

  return nullptr;
}

}
