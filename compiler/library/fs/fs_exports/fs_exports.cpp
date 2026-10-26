#include "compiler/library/fs/fs_exports/fs_exports.h"
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

std::string fsExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  if (llvm::is_contained(kFsFunctions, name) || name == "constants" || name == "Stats" ||
      name == "Dirent") {
    return "";
  }
  std::string sync = (name + "Sync").str();
  if (llvm::is_contained(kFsFunctions, sync)) {
    if (llvm::is_contained(kFspFunctions, name)) {
      return what + " takes a callback, which tz does not have; import '" + name.str() +
             "' from \"node:fs/promises\", which gives a Promise, or '" + sync +
             "', which gives what it reads" +
             (name == "readFile" ? ", e.g. 'const text = await readFile(path, \"utf8\")'" : "");
    }
    return what + " takes a callback, which tz does not have; import '" + sync +
           "' and use what it returns";
  }
  if (name == "promises") {
    return "";
  }
  if (llvm::is_contained(kFsExports, name)) {
    return what + " is not supported yet";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

std::string fsPromisesExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  if (llvm::is_contained(kFspFunctions, name) || name == "constants") {
    return "";
  }
  if (llvm::is_contained(kFspExports, name)) {
    return what + " is not supported yet";
  }
  if (llvm::is_contained(kFsFunctions, name)) {
    return quoted + " has no export named '" + name.str() + "'; the function that waits is '" +
           name.drop_back(4).str() + "', or import '" + name.str() + "' from \"node:fs\"";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

std::string readlineExportProblem(llvm::StringRef name, llvm::StringRef path, bool hasPromises) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  if (name == "createInterface" || name == "Interface") {
    return "";
  }
  if (name == "clearLine" || name == "clearScreenDown" || name == "cursorTo" ||
      name == "emitKeypressEvents" || name == "moveCursor" || name == "Readline" ||
      (hasPromises && name == "promises")) {
    return what + " is not supported yet; tz has createInterface";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

std::string tzIoExportProblem(llvm::StringRef name, llvm::StringRef path) {
  if (name == "readLine") {
    return "";
  }

  return "'" + path.str() + "' has no export named '" + name.str() + "'; it has readLine";
}

}
