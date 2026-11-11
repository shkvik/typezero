#include "compiler/program/library/intrinsic/library_intrinsic_table/library_intrinsic_table.h"

namespace tz {

namespace {

const LibraryIntrinsicInfo kLibraryIntrinsics[] = {
    {
        .name = "asListener",
        .intrinsic = LibraryIntrinsic::AS_LISTENER,
        .builtin = Builtin::LibListener,
        .args = 1,
        .typeArgs = 1,
    },
    {
        .name = "asBoundListener",
        .intrinsic = LibraryIntrinsic::AS_BOUND_LISTENER,
        .builtin = Builtin::LibBoundListener,
        .args = 2,
        .typeArgs = 1,
    },
    {
        .name = "asClass",
        .intrinsic = LibraryIntrinsic::AS_CLASS,
        .builtin = Builtin::LibAs,
        .args = 1,
        .typeArgs = 1,
    },
    {
        .name = "chunkOf",
        .intrinsic = LibraryIntrinsic::CHUNK_OF,
        .builtin = Builtin::LibChunk,
        .args = 1,
        .typeArgs = 1,
    },
    {
        .name = "isChunk",
        .intrinsic = LibraryIntrinsic::IS_CHUNK,
        .builtin = Builtin::LibIsChunk,
        .args = 1,
        .typeArgs = 1,
    },
    {
        .name = "chunkKind",
        .intrinsic = LibraryIntrinsic::CHUNK_KIND,
        .builtin = Builtin::LibChunkKind,
        .args = 0,
        .typeArgs = 1,
    },
    {
        .name = "typeName",
        .intrinsic = LibraryIntrinsic::TYPE_NAME,
        .builtin = Builtin::LibTypeName,
        .args = 0,
        .typeArgs = 1,
    },
    {
        .name = "overrides",
        .intrinsic = LibraryIntrinsic::OVERRIDES,
        .builtin = Builtin::LibOverrides,
        .args = 2,
        .typeArgs = 0,
    },
    {
        .name = "fsOpen",
        .intrinsic = LibraryIntrinsic::FS_OPEN,
        .builtin = Builtin::LibFsOpen,
        .args = 3,
        .typeArgs = 1,
    },
    {
        .name = "fsRead",
        .intrinsic = LibraryIntrinsic::FS_READ,
        .builtin = Builtin::LibFsRead,
        .args = 5,
        .typeArgs = 1,
    },
    {
        .name = "fsWrite",
        .intrinsic = LibraryIntrinsic::FS_WRITE,
        .builtin = Builtin::LibFsWrite,
        .args = 5,
        .typeArgs = 1,
    },
    {
        .name = "fsClose",
        .intrinsic = LibraryIntrinsic::FS_CLOSE,
        .builtin = Builtin::LibFsClose,
        .args = 2,
        .typeArgs = 1,
    },
    {
        .name = "stdinChunk",
        .intrinsic = LibraryIntrinsic::STDIN_CHUNK,
        .builtin = Builtin::LibStdinChunk,
        .args = 1,
        .typeArgs = 0,
    },
    {
        .name = "stdWrite",
        .intrinsic = LibraryIntrinsic::STD_WRITE,
        .builtin = Builtin::LibStdWrite,
        .args = 2,
        .typeArgs = 0,
    },
    {
        .name = "stdinFailed",
        .intrinsic = LibraryIntrinsic::STDIN_FAILED,
        .builtin = Builtin::LibStdinFailed,
        .args = 0,
        .typeArgs = 0,
    },
    {
        .name = "ahead",
        .intrinsic = LibraryIntrinsic::AHEAD,
        .builtin = Builtin::LibAhead,
        .args = 1,
        .typeArgs = 0,
    },
    {
        .name = "order",
        .intrinsic = LibraryIntrinsic::ORDER,
        .builtin = Builtin::LibOrder,
        .args = 0,
        .typeArgs = 0,
    },
    {
        .name = "panic",
        .intrinsic = LibraryIntrinsic::PANIC,
        .builtin = Builtin::LibPanic,
        .args = 1,
        .typeArgs = 0,
    },
    {
        .name = "defer",
        .intrinsic = LibraryIntrinsic::DEFER,
        .builtin = Builtin::LibDefer,
        .args = 1,
        .typeArgs = 0,
    },
    {
        .name = "withContext",
        .intrinsic = LibraryIntrinsic::WITH_CONTEXT,
        .builtin = Builtin::LibWithContext,
        .args = 2,
        .typeArgs = 0,
    },
    {
        .name = "aheadMovable",
        .intrinsic = LibraryIntrinsic::AHEAD_MOVABLE,
        .builtin = Builtin::LibAheadMovable,
        .args = 1,
        .typeArgs = 0,
    },
};

}

const LibraryIntrinsicInfo *LibraryIntrinsicTable::find(llvm::StringRef name) {
  for (const LibraryIntrinsicInfo &info : kLibraryIntrinsics) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
