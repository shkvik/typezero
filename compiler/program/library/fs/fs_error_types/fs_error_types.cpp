#include "compiler/program/library/fs/fs_error_types/fs_error_types.h"

namespace tz {

namespace {

struct ErrorVerb {
  llvm::StringLiteral type;
  llvm::StringLiteral verb;
};

const ErrorVerb kVerbs[] = {
    {"ReadFileError", "read"},
    {"WriteFileError", "write"},
    {"WriteNewFileError", "write"},
    {"AppendFileError", "append to"},
    {"AppendNewFileError", "append to"},
    {"TruncateError", "truncate"},
    {"AccessError", "check"},
    {"ChangeModeError", "change the mode of"},
    {"StatusError", "get the info of"},
    {"StatusIfExistsError", "get the info of"},
    {"LinkStatusError", "get the info of"},
    {"LinkStatusIfExistsError", "get the info of"},
    {"ReadDirectoryError", "list"},
    {"RealPathError", "resolve"},
    {"MakeDirectoryError", "create the folder"},
    {"MakeTemporaryDirectoryError", "create a temporary folder at"},
    {"RemoveDirectoryError", "remove the folder"},
    {"UnlinkError", "remove"},
    {"RemoveError", "remove"},
    {"ForceRemoveError", "remove"},
    {"RenameError", "move"},
    {"ReplaceError", "move"},
    {"CopyFileError", "copy"},
    {"ReadStreamError", "read"},
    {"WriteStreamError", "write"},
    {"WriteNewStreamError", "write"},
};

}

llvm::StringRef FsErrorTypes::verbOf(llvm::StringRef name) {
  for (const ErrorVerb &entry : kVerbs) {
    if (entry.type == name) {
      return entry.verb;
    }
  }

  return "";
}

std::vector<llvm::StringRef> FsErrorTypes::getTypeNames() {
  std::vector<llvm::StringRef> names;
  for (const ErrorVerb &entry : kVerbs) {
    names.push_back(entry.type);
  }

  return names;
}

}
