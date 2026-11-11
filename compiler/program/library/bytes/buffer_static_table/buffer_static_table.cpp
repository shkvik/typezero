#include "compiler/program/library/bytes/buffer_static_table/buffer_static_table.h"

namespace tz {

namespace {

const BufferStaticInfo kBufferStatics[] = {
    {
        .method = BytesMethod::FROM,
        .kind = BufferStaticKind::FROM,
    },
    {
        .method = BytesMethod::ALLOC,
        .kind = BufferStaticKind::ALLOC,
    },
    {
        .method = BytesMethod::ALLOC_UNSAFE,
        .kind = BufferStaticKind::ALLOC_UNSAFE,
    },
    {
        .method = BytesMethod::ALLOC_UNSAFE_SLOW,
        .kind = BufferStaticKind::ALLOC_UNSAFE,
    },
    {
        .method = BytesMethod::CONCAT,
        .kind = BufferStaticKind::CONCAT,
    },
    {
        .method = BytesMethod::BYTE_LENGTH,
        .kind = BufferStaticKind::BYTE_LENGTH,
    },
    {
        .method = BytesMethod::COMPARE,
        .kind = BufferStaticKind::COMPARE,
    },
    {
        .method = BytesMethod::IS_BUFFER,
        .kind = BufferStaticKind::IS_BUFFER,
    },
    {
        .method = BytesMethod::IS_ENCODING,
        .kind = BufferStaticKind::IS_ENCODING,
    },
};

}

const BufferStaticInfo *BufferStaticTable::find(BytesMethod method) {
  for (const BufferStaticInfo &info : kBufferStatics) {
    if (info.method == method) {
      return &info;
    }
  }

  return nullptr;
}

}
