#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class LibraryIntrinsic {
  AS_LISTENER,
  AS_BOUND_LISTENER,
  AS_CLASS,
  CHUNK_OF,
  IS_CHUNK,
  CHUNK_KIND,
  TYPE_NAME,
  OVERRIDES,
  FS_OPEN,
  FS_READ,
  FS_WRITE,
  FS_CLOSE,
  STDIN_CHUNK,
  STD_WRITE,
  STDIN_FAILED,
  AHEAD,
  ORDER,
  PANIC,
  DEFER,
  WITH_CONTEXT,
  AHEAD_MOVABLE,
};

struct LibraryIntrinsicInfo {
  llvm::StringLiteral name;
  LibraryIntrinsic intrinsic;
  Builtin builtin;
  unsigned args;
  unsigned typeArgs;
};

class LibraryIntrinsicTable {
public:
  static const LibraryIntrinsicInfo *find(llvm::StringRef name);
};

}
