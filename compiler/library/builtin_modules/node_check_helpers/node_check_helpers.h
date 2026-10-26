#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace tz {

inline const llvm::StringLiteral kFsFunctions[] = {
    "readFileSync", "writeFileSync", "appendFileSync", "existsSync", "accessSync",
    "chmodSync",    "truncateSync",  "statSync",       "lstatSync",  "readdirSync",
    "mkdirSync",    "mkdtempSync",   "rmdirSync",      "unlinkSync", "rmSync",
    "renameSync",   "copyFileSync",  "realpathSync"};

inline const llvm::StringLiteral kFsExports[] = {"Dir",
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

inline const llvm::StringLiteral kPathFunctions[] = {
    "join",    "resolve",  "normalize", "isAbsolute", "dirname",         "basename",
    "extname", "relative", "parse",     "format",     "toNamespacedPath"};

inline const llvm::StringLiteral kPathExports[] = {"sep",   "delimiter",   "posix",
                                                   "win32", "matchesGlob", "_makeLong"};

inline const llvm::StringLiteral kOsFunctions[] = {
    "platform", "homedir", "tmpdir",     "hostname", "cpus",    "availableParallelism",
    "totalmem", "freemem", "arch",       "type",     "release", "version",
    "machine",  "uptime",  "endianness", "loadavg",  "userInfo"};

inline const llvm::StringLiteral kOsExports[] = {"EOL",         "devNull",           "getPriority",
                                                 "setPriority", "networkInterfaces", "constants"};

inline const llvm::StringLiteral kBufferExports[] = {"Blob",
                                                     "File",
                                                     "SlowBuffer",
                                                     "atob",
                                                     "btoa",
                                                     "constants",
                                                     "isAscii",
                                                     "isUtf8",
                                                     "kMaxLength",
                                                     "kStringMaxLength",
                                                     "resolveObjectURL",
                                                     "transcode",
                                                     "INSPECT_MAX_BYTES"};

inline const llvm::StringLiteral kFspFunctions[] = {
    "readFile", "writeFile", "appendFile", "access",   "chmod",    "truncate",
    "stat",     "lstat",     "readdir",    "mkdir",    "mkdtemp",  "rmdir",
    "unlink",   "rm",        "rename",     "copyFile", "realpath", "open"};

inline const llvm::StringLiteral kFspExports[] = {
    "access",    "copyFile",   "cp",       "glob",    "open",     "opendir",  "rename",
    "truncate",  "rm",         "rmdir",    "mkdir",   "readdir",  "readlink", "symlink",
    "lstat",     "stat",       "statfs",   "link",    "unlink",   "chmod",    "lchmod",
    "lchown",    "chown",      "utimes",   "lutimes", "realpath", "mkdtemp",  "mkdtempDisposable",
    "writeFile", "appendFile", "readFile", "watch",   "constants"};

inline const llvm::StringLiteral kChildExports[] = {"ChildProcess", "_forkChild",   "exec",
                                                    "execFile",     "execFileSync", "execSync",
                                                    "fork",         "spawn",        "spawnSync"};

inline const llvm::StringLiteral kProcessNames[] = {
    "argv", "env", "exit", "exitCode", "cwd", "platform", "execPath", "stdin", "stdout", "stderr"};

std::string exportProblem(NodeModule module, llvm::StringRef name, llvm::StringRef path);

std::string moduleLabel(NodeModule module);

}
