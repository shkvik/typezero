#include "compiler/semantic/library/fs/fs_exports/fs_exports.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"

namespace tz {

namespace {

const llvm::StringLiteral kFsFunctions[] = {
    "readFileSync", "writeFileSync", "appendFileSync", "existsSync", "accessSync",
    "chmodSync",    "truncateSync",  "statSync",       "lstatSync",  "readdirSync",
    "mkdirSync",    "mkdtempSync",   "rmdirSync",      "unlinkSync", "rmSync",
    "renameSync",   "copyFileSync",  "realpathSync"};

const llvm::StringLiteral kFsExports[] = {"Dir",
                                          "Dirent",
                                          "FileReadStream",
                                          "FileWriteStream",
                                          "ReadStream",
                                          "Stats",
                                          "Utf8Stream",
                                          "WriteStream",
                                          "_toUnixTimestamp",
                                          "access",
                                          "accessSync",
                                          "appendFile",
                                          "appendFileSync",
                                          "chmod",
                                          "chmodSync",
                                          "chown",
                                          "chownSync",
                                          "close",
                                          "closeSync",
                                          "constants",
                                          "copyFile",
                                          "copyFileSync",
                                          "cp",
                                          "cpSync",
                                          "createReadStream",
                                          "createWriteStream",
                                          "exists",
                                          "existsSync",
                                          "fchmod",
                                          "fchmodSync",
                                          "fchown",
                                          "fchownSync",
                                          "fdatasync",
                                          "fdatasyncSync",
                                          "fstat",
                                          "fstatSync",
                                          "fsync",
                                          "fsyncSync",
                                          "ftruncate",
                                          "ftruncateSync",
                                          "futimes",
                                          "futimesSync",
                                          "glob",
                                          "globSync",
                                          "lchmod",
                                          "lchmodSync",
                                          "lchown",
                                          "lchownSync",
                                          "link",
                                          "linkSync",
                                          "lstat",
                                          "lstatSync",
                                          "lutimes",
                                          "lutimesSync",
                                          "mkdir",
                                          "mkdirSync",
                                          "mkdtemp",
                                          "mkdtempDisposableSync",
                                          "mkdtempSync",
                                          "open",
                                          "openAsBlob",
                                          "openSync",
                                          "opendir",
                                          "opendirSync",
                                          "promises",
                                          "read",
                                          "readFile",
                                          "readFileSync",
                                          "readSync",
                                          "readdir",
                                          "readdirSync",
                                          "readlink",
                                          "readlinkSync",
                                          "readv",
                                          "readvSync",
                                          "realpath",
                                          "realpathSync",
                                          "rename",
                                          "renameSync",
                                          "rm",
                                          "rmSync",
                                          "rmdir",
                                          "rmdirSync",
                                          "stat",
                                          "statSync",
                                          "statfs",
                                          "statfsSync",
                                          "symlink",
                                          "symlinkSync",
                                          "truncate",
                                          "truncateSync",
                                          "unlink",
                                          "unlinkSync",
                                          "unwatchFile",
                                          "utimes",
                                          "utimesSync",
                                          "watch",
                                          "watchFile",
                                          "write",
                                          "writeFile",
                                          "writeFileSync",
                                          "writeSync",
                                          "writev",
                                          "writevSync"};

const llvm::StringLiteral kFspFunctions[] = {
    "readFile", "writeFile", "appendFile", "access",   "chmod",    "truncate",
    "stat",     "lstat",     "readdir",    "mkdir",    "mkdtemp",  "rmdir",
    "unlink",   "rm",        "rename",     "copyFile", "realpath", "open"};

const llvm::StringLiteral kFspExports[] = {
    "access",    "copyFile",   "cp",       "glob",    "open",     "opendir",  "rename",
    "truncate",  "rm",         "rmdir",    "mkdir",   "readdir",  "readlink", "symlink",
    "lstat",     "stat",       "statfs",   "link",    "unlink",   "chmod",    "lchmod",
    "lchown",    "chown",      "utimes",   "lutimes", "realpath", "mkdtemp",  "mkdtempDisposable",
    "writeFile", "appendFile", "readFile", "watch",   "constants"};

}

FsExports::ResultExportFound FsExports::fsExport(llvm::StringRef name, llvm::StringRef path) {
  if (llvm::is_contained(kFsFunctions, name) || name == "constants" || name == "Stats" ||
      name == "Dirent" || name == "promises") {
    return ok(name);
  }
  if (name == "replaceSync" || name == "ReplaceError") {
    std::string message = ErrorTextFsExport::inTzFs(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  std::string sync = (name + "Sync").str();
  if (llvm::is_contained(kFsFunctions, sync) && llvm::is_contained(kFspFunctions, name)) {
    std::string message = ErrorTextFsExport::callback(name, path, sync);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (llvm::is_contained(kFsFunctions, sync)) {
    std::string message = ErrorTextFsExport::syncOnly(name, path, sync);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::CALLBACK_FORM,
        .message = message,
    });
  }
  if (llvm::is_contained(kFsExports, name)) {
    std::string message = ErrorTextModuleExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::promisesExport(llvm::StringRef name, llvm::StringRef path) {
  if (llvm::is_contained(kFspFunctions, name) || name == "constants") {
    return ok(name);
  }
  if (name == "replace" || name == "ReplaceError") {
    std::string message = ErrorTextFsExport::inTzFs(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  if (llvm::is_contained(kFspExports, name)) {
    std::string message = ErrorTextModuleExport::notYet(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  if (llvm::is_contained(kFsFunctions, name)) {
    std::string message = ErrorTextFsExport::waiting(path, name);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::ELSEWHERE,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::readlineExport(llvm::StringRef name, llvm::StringRef path,
                                                       bool hasPromises) {
  if (name == "createInterface" || name == "Interface") {
    return ok(name);
  }
  if (name == "clearLine" || name == "clearScreenDown" || name == "cursorTo" ||
      name == "emitKeypressEvents" || name == "moveCursor" || name == "Readline" ||
      (hasPromises && name == "promises")) {
    std::string message = ErrorTextFsExport::readline(name, path);

    return err(ModuleExportFoundError{
        .code = ModuleExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::tzFsExport(llvm::StringRef name, llvm::StringRef path) {
  if (name == "replaceSync" || name == "replace") {
    return ok(name);
  }
  bool isInNodeFs =
      llvm::is_contained(kFsFunctions, name) || llvm::is_contained(kFspFunctions, name);
  std::string message = ErrorTextFsExport::tzFs(path, name, isInNodeFs);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

FsExports::ResultExportFound FsExports::tzIoExport(llvm::StringRef name, llvm::StringRef path) {
  if (name == "readLine") {
    return ok(name);
  }
  std::string message = ErrorTextFsExport::tzIo(path, name);

  return err(ModuleExportFoundError{
      .code = ModuleExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
