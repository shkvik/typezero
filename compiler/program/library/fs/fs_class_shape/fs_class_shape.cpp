#include "compiler/program/library/fs/fs_class_shape/fs_class_shape.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const FsClassInfo kClasses[] = {
    {
        .fsClass = FsClass::STATS,
        .name = "Stats",
        .fields = {{"dev", FsFieldType::INT},
                   {"mode", FsFieldType::INT},
                   {"nlink", FsFieldType::INT},
                   {"uid", FsFieldType::INT},
                   {"gid", FsFieldType::INT},
                   {"rdev", FsFieldType::INT},
                   {"blksize", FsFieldType::INT},
                   {"ino", FsFieldType::INT},
                   {"size", FsFieldType::INT},
                   {"blocks", FsFieldType::INT},
                   {"atimeMs", FsFieldType::DOUBLE},
                   {"mtimeMs", FsFieldType::DOUBLE},
                   {"ctimeMs", FsFieldType::DOUBLE},
                   {"birthtimeMs", FsFieldType::DOUBLE}},
        .privateFrom = 14,
    },
    {
        .fsClass = FsClass::DIRENT,
        .name = "Dirent",
        .fields = {{"name", FsFieldType::STRING},
                   {"parentPath", FsFieldType::STRING},
                   {"Symbol(type)", FsFieldType::INT}},
        .privateFrom = 2,
    },
};

const llvm::StringLiteral kStatsDates[] = {"atime", "mtime", "ctime", "birthtime"};

const FileKindInfo kFileKinds[] = {
    {.method = "isFile", .fileKind = 0100000},
    {.method = "isDirectory", .fileKind = 0040000},
    {.method = "isSymbolicLink", .fileKind = 0120000},
    {.method = "isFIFO", .fileKind = 0010000},
    {.method = "isSocket", .fileKind = 0140000},
    {.method = "isBlockDevice", .fileKind = 0060000},
    {.method = "isCharacterDevice", .fileKind = 0020000},
};

}

const FsClassInfo &FsClassShape::find(FsClass fsClass) {
  for (const FsClassInfo &info : kClasses) {
    if (info.fsClass == fsClass) {
      return info;
    }
  }
  llvm_unreachable("fs class missing from the table");
}

bool FsClassShape::isStatsDate(llvm::StringRef name) {
  return llvm::is_contained(kStatsDates, name);
}

std::optional<int> FsClassShape::fileKindOf(llvm::StringRef method) {
  for (const FileKindInfo &info : kFileKinds) {
    if (info.method == method) {
      return info.fileKind;
    }
  }

  return std::nullopt;
}

}
