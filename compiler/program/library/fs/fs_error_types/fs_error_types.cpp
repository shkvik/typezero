#include "compiler/program/library/fs/fs_error_types/fs_error_types.h"

namespace tz {

namespace {

struct ErrorVerb {
  llvm::StringLiteral type;
  llvm::StringLiteral verb;
  NodeModule home;
};

const ErrorVerb kVerbs[] = {
    {"ReadFileError", "read", NodeModule::Fs},
    {"WriteFileError", "write", NodeModule::Fs},
    {"WriteNewFileError", "write", NodeModule::Fs},
    {"AppendFileError", "append to", NodeModule::Fs},
    {"AppendNewFileError", "append to", NodeModule::Fs},
    {"TruncateError", "truncate", NodeModule::Fs},
    {"AccessError", "check", NodeModule::Fs},
    {"ChangeModeError", "change the mode of", NodeModule::Fs},
    {"StatusError", "get the info of", NodeModule::Fs},
    {"StatusIfExistsError", "get the info of", NodeModule::Fs},
    {"LinkStatusError", "get the info of", NodeModule::Fs},
    {"LinkStatusIfExistsError", "get the info of", NodeModule::Fs},
    {"ReadDirectoryError", "list", NodeModule::Fs},
    {"RealPathError", "resolve", NodeModule::Fs},
    {"MakeDirectoryError", "create the folder", NodeModule::Fs},
    {"MakeTemporaryDirectoryError", "create a temporary folder at", NodeModule::Fs},
    {"RemoveDirectoryError", "remove the folder", NodeModule::Fs},
    {"UnlinkError", "remove", NodeModule::Fs},
    {"RemoveError", "remove", NodeModule::Fs},
    {"ForceRemoveError", "remove", NodeModule::Fs},
    {"RenameError", "move", NodeModule::Fs},
    {"ReplaceError", "move", NodeModule::TzFs},
    {"CopyFileError", "copy", NodeModule::Fs},
    {"ReadStreamError", "read", NodeModule::Fs},
    {"WriteStreamError", "write", NodeModule::Fs},
    {"WriteNewStreamError", "write", NodeModule::Fs},
};

const llvm::StringLiteral kStreamErrorVariants[] = {
    "ReadStreamErrorNotFound",       "ReadStreamErrorWrongType",  "ReadStreamErrorPermission",
    "ReadStreamErrorBusy",           "ReadStreamErrorPath",       "ReadStreamErrorSystem",
    "ReadStreamErrorInvalidRange",   "WriteStreamErrorNotFound",  "WriteStreamErrorWrongType",
    "WriteStreamErrorPermission",    "WriteStreamErrorBusy",      "WriteStreamErrorFull",
    "WriteStreamErrorPath",          "WriteStreamErrorSystem",    "WriteStreamErrorInvalidRange",
    "WriteNewStreamErrorNotFound",   "WriteNewStreamErrorExists", "WriteNewStreamErrorWrongType",
    "WriteNewStreamErrorPermission", "WriteNewStreamErrorBusy",   "WriteNewStreamErrorFull",
    "WriteNewStreamErrorPath",       "WriteNewStreamErrorSystem", "WriteNewStreamErrorInvalidRange",
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

std::vector<llvm::StringRef> FsErrorTypes::getNodeFsNames() {
  std::vector<llvm::StringRef> names = FsErrorTypes::getPromiseNames();
  for (llvm::StringLiteral variant : kStreamErrorVariants) {
    names.push_back(variant);
  }

  return names;
}

std::vector<llvm::StringRef> FsErrorTypes::getPromiseNames() {
  std::vector<llvm::StringRef> names;
  for (const ErrorVerb &entry : kVerbs) {
    if (entry.home == NodeModule::Fs) {
      names.push_back(entry.type);
    }
  }

  return names;
}

std::vector<llvm::StringRef> FsErrorTypes::getTzFsNames() {
  std::vector<llvm::StringRef> names;
  for (const ErrorVerb &entry : kVerbs) {
    if (entry.home == NodeModule::TzFs) {
      names.push_back(entry.type);
    }
  }

  return names;
}

}
