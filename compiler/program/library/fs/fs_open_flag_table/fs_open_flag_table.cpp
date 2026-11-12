#include "compiler/program/library/fs/fs_open_flag_table/fs_open_flag_table.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kOpenFlags[] = {"r",  "rs", "sr",  "r+",  "rs+", "sr+", "w",  "wx",
                                              "xw", "w+", "wx+", "xw+", "a",   "ax",  "xa", "as",
                                              "sa", "a+", "ax+", "xa+", "as+", "sa+"};

const FsDefaultFlagInfo kDefaults[] = {
    {.function = FsFunction::READ_FILE_SYNC, .flag = "r"},
    {.function = FsFunction::WRITE_FILE_SYNC, .flag = "w"},
    {.function = FsFunction::APPEND_FILE_SYNC, .flag = "a"},
};

}

bool FsOpenFlagTable::isKnown(llvm::StringRef flag) {
  return llvm::is_contained(kOpenFlags, flag);
}

llvm::StringRef FsOpenFlagTable::getDefault(FsFunction function) {
  for (const FsDefaultFlagInfo &info : kDefaults) {
    if (info.function == function) {
      return info.flag;
    }
  }
  llvm_unreachable("fs function has no default open flag");
}

}
