// The built-in modules (docs/io.md): node:fs, node:path, node:os,
// node:process, node:buffer, node:child_process (its functions are in
// sema_child.cpp) and node:crypto (sema_crypto.cpp), the same names without
// "node:", and tz:io. The API is
// Node's: a file imports the names it uses (`import { join } from
// "node:path"`), and each becomes a built-in call where it is called. What
// Node has and tz does not yet, and what needs async (functions with a
// callback), are errors that say so at the import.
//
// The functions of node:path take the style of the paths, win32 or posix:
// the system's for node:path, and the other one through `win32` and `posix`.
// The functions of node:fs take their options as an object literal written
// in the call: the type of the result may depend on them (an encoding gives
// a string; `withFileTypes: true` gives Dirents), so those that decide it
// must be literals. What fails in the system is a SystemError
// (sema_error.cpp), which runtime/fs.c makes as Node does.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The functions of node:fs that tz has, and every name node:fs exports in
// Node 24.
const llvm::StringLiteral kFsFunctions[] = {
    "readFileSync", "writeFileSync", "appendFileSync", "existsSync", "accessSync",
    "chmodSync",    "truncateSync",  "statSync",       "lstatSync",  "readdirSync",
    "mkdirSync",    "mkdtempSync",   "rmdirSync",      "unlinkSync", "rmSync",
    "renameSync",   "copyFileSync",  "realpathSync"};
const llvm::StringLiteral kFsExports[] = {
    "Dir", "Dirent", "FileReadStream", "FileWriteStream", "ReadStream", "Stats", "Utf8Stream",
    "WriteStream", "_toUnixTimestamp", "access", "accessSync", "appendFile", "appendFileSync",
    "chmod", "chmodSync", "chown", "chownSync", "close", "closeSync", "constants", "copyFile",
    "copyFileSync", "cp", "cpSync", "createReadStream", "createWriteStream", "exists", "existsSync",
    "fchmod", "fchmodSync", "fchown", "fchownSync", "fdatasync", "fdatasyncSync", "fstat",
    "fstatSync", "fsync", "fsyncSync", "ftruncate", "ftruncateSync", "futimes", "futimesSync",
    "glob", "globSync", "lchmod", "lchmodSync", "lchown", "lchownSync", "link", "linkSync", "lstat",
    "lstatSync", "lutimes", "lutimesSync", "mkdir", "mkdirSync", "mkdtemp", "mkdtempDisposableSync",
    "mkdtempSync", "open", "openAsBlob", "openSync", "opendir", "opendirSync", "promises", "read",
    "readFile", "readFileSync", "readSync", "readdir", "readdirSync", "readlink", "readlinkSync",
    "readv", "readvSync", "realpath", "realpathSync", "rename", "renameSync", "rm", "rmSync",
    "rmdir", "rmdirSync", "stat", "statSync", "statfs", "statfsSync", "symlink", "symlinkSync",
    "truncate", "truncateSync", "unlink", "unlinkSync", "unwatchFile", "utimes", "utimesSync",
    "watch", "watchFile", "write", "writeFile", "writeFileSync", "writeSync", "writev",
    "writevSync"};
const llvm::StringLiteral kPathFunctions[] = {
    "join",    "resolve",  "normalize", "isAbsolute", "dirname",         "basename",
    "extname", "relative", "parse",     "format",     "toNamespacedPath"};
const llvm::StringLiteral kPathExports[] = {"sep",   "delimiter",   "posix",
                                            "win32", "matchesGlob", "_makeLong"};
const llvm::StringLiteral kOsFunctions[] = {
    "platform", "homedir", "tmpdir",     "hostname", "cpus",    "availableParallelism",
    "totalmem", "freemem", "arch",       "type",     "release", "version",
    "machine",  "uptime",  "endianness", "loadavg",  "userInfo"};
const llvm::StringLiteral kOsExports[] = {"EOL",         "devNull",           "getPriority",
                                          "setPriority", "networkInterfaces", "constants"};
// What node:buffer exports in Node 24 besides Buffer, which is global too.
const llvm::StringLiteral kBufferExports[] = {
    "Blob", "File", "SlowBuffer", "atob", "btoa", "constants", "isAscii", "isUtf8",
    "kMaxLength", "kStringMaxLength", "resolveObjectURL", "transcode", "INSPECT_MAX_BYTES"};
// The functions of node:fs/promises that tz has, and every name it exports
// in Node 24.
const llvm::StringLiteral kFspFunctions[] = {
    "readFile", "writeFile", "appendFile", "access", "chmod",  "truncate", "stat",    "lstat",
    "readdir",  "mkdir",     "mkdtemp",    "rmdir",  "unlink", "rm",       "rename",  "copyFile",
    "realpath", "open"};
const llvm::StringLiteral kFspExports[] = {
    "access",    "copyFile",   "cp",       "glob",    "open",     "opendir", "rename",
    "truncate",  "rm",         "rmdir",    "mkdir",   "readdir",  "readlink", "symlink",
    "lstat",     "stat",       "statfs",   "link",    "unlink",   "chmod",   "lchmod",
    "lchown",    "chown",      "utimes",   "lutimes", "realpath", "mkdtemp", "mkdtempDisposable",
    "writeFile", "appendFile", "readFile", "watch",   "constants"};
// Every name node:child_process exports in Node 24.
const llvm::StringLiteral kChildExports[] = {"ChildProcess", "_forkChild",   "exec",
                                             "execFile",     "execFileSync", "execSync",
                                             "fork",         "spawn",        "spawnSync"};
// What node:process gives that tz has: the properties of process it has.
const llvm::StringLiteral kProcessNames[] = {"argv",     "env",      "exit",   "exitCode", "cwd",
                                             "platform", "execPath", "stdout", "stderr"};

// The modules tz has, by the name after "node:".
std::optional<NodeModule> moduleOf(llvm::StringRef path) {
  if (path == "tz:io")
    return NodeModule::TzIo;
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  if (bare == "fs")
    return NodeModule::Fs;
  if (bare == "fs/promises")
    return NodeModule::FsPromises;
  if (bare == "timers/promises")
    return NodeModule::TimersPromises;
  if (bare == "readline")
    return NodeModule::Readline;
  if (bare == "readline/promises")
    return NodeModule::ReadlinePromises;
  if (bare == "path")
    return NodeModule::Path;
  if (bare == "path/posix")
    return NodeModule::PathPosix;
  if (bare == "path/win32")
    return NodeModule::PathWin32;
  if (bare == "os")
    return NodeModule::Os;
  if (bare == "process")
    return NodeModule::Process;
  if (bare == "buffer")
    return NodeModule::Buffer;
  if (bare == "child_process")
    return NodeModule::ChildProcess;
  if (bare == "crypto")
    return NodeModule::Crypto;
  return std::nullopt;
}

// Why a built-in module that tz does not have is not there.
std::string missingModule(llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (path.starts_with("tz:")) {
    if (path == "tz:string")
      return "built-in module " + quoted + " is not supported yet";
    return "there is no built-in module " + quoted + "; tz has 'tz:io' and 'tz:http'";
  }
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  if (!isBuiltinModule(bare) && !path.starts_with("node:sea") && path != "node:sqlite" &&
      !path.starts_with("node:test"))
    return "there is no built-in module " + quoted;
  std::string message = "built-in module " + quoted + " is not supported yet";
  if (bare == "timers")
    return message + "; setTimeout and the other timers are global, as in Node: drop the import";
  if (bare == "http" || bare == "https")
    return message + " (docs/net.md, section 7); serve requests with 'serve' from \"tz:http\", "
                     "and make them with the global 'fetch'";
  static const llvm::StringLiteral network[] = {"http2", "net", "tls", "dgram", "dns",
                                                "dns/promises"};
  if (llvm::is_contained(network, bare))
    return message + " (docs/net.md, section 7)";
  if (bare == "events")
    return message + " (docs/events.md)";
  if (bare.starts_with("stream"))
    return message + " (docs/streams.md)";
  return message;
}

// A typical name of each module, for hints.
llvm::StringRef typicalName(NodeModule module) {
  switch (module) {
  case NodeModule::Fs:
    return "readFileSync";
  case NodeModule::FsPromises:
    return "readFile";
  case NodeModule::TimersPromises:
    return "setTimeout";
  case NodeModule::Readline:
  case NodeModule::ReadlinePromises:
    return "createInterface";
  case NodeModule::Path:
  case NodeModule::PathPosix:
  case NodeModule::PathWin32:
    return "join";
  case NodeModule::Os:
    return "homedir";
  case NodeModule::Process:
    return "argv";
  case NodeModule::Buffer:
    return "Buffer";
  case NodeModule::TzIo:
    return "readLine";
  case NodeModule::ChildProcess:
    return "execFileSync";
  case NodeModule::Crypto:
    return "createHash";
  }
  return "x";
}

bool isPathModule(NodeModule module) {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

// What is wrong with importing `name` from the module `path` names, or
// nothing.
std::string exportProblem(NodeModule module, llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  std::string what = "'" + name.str() + "' of " + quoted;
  switch (module) {
  case NodeModule::Fs: {
    if (llvm::is_contained(kFsFunctions, name) || name == "constants" || name == "Stats" ||
        name == "Dirent")
      return "";
    std::string sync = (name + "Sync").str();
    if (llvm::is_contained(kFsFunctions, sync)) {
      // The one of node:fs/promises gives a Promise instead.
      if (llvm::is_contained(kFspFunctions, name))
        return what + " takes a callback, which tz does not have; import '" + name.str() +
               "' from \"node:fs/promises\", which gives a Promise, or '" + sync +
               "', which gives what it reads" +
               (name == "readFile" ? ", e.g. 'const text = await readFile(path, \"utf8\")'" : "");
      return what + " takes a callback, which tz does not have; import '" + sync +
             "' and use what it returns";
    }
    if (name == "promises")
      return "";
    if (llvm::is_contained(kFsExports, name))
      return what + " is not supported yet";
    return quoted + " has no export named '" + name.str() + "'";
  }
  case NodeModule::FsPromises:
    if (llvm::is_contained(kFspFunctions, name) || name == "constants")
      return "";
    if (llvm::is_contained(kFspExports, name))
      return what + " is not supported yet";
    if (llvm::is_contained(kFsFunctions, name))
      return quoted + " has no export named '" + name.str() + "'; the function that waits is '" +
             name.drop_back(4).str() + "', or import '" + name.str() + "' from \"node:fs\"";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::TimersPromises:
    if (name == "setTimeout" || name == "setImmediate" || name == "setInterval" ||
        name == "scheduler")
      return "";
    return quoted + " has no export named '" + name.str() +
           "'; it has setTimeout, setImmediate, setInterval and scheduler";
  case NodeModule::Readline:
  case NodeModule::ReadlinePromises:
    if (name == "createInterface" || name == "Interface")
      return "";
    if (name == "clearLine" || name == "clearScreenDown" || name == "cursorTo" ||
        name == "emitKeypressEvents" || name == "moveCursor" || name == "Readline" ||
        (module == NodeModule::Readline && name == "promises"))
      return what + " is not supported yet; tz has createInterface";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Path:
  case NodeModule::PathPosix:
  case NodeModule::PathWin32:
    if (llvm::is_contained(kPathFunctions, name) || name == "sep" || name == "delimiter" ||
        name == "posix" || name == "win32")
      return "";
    if (llvm::is_contained(kPathExports, name))
      return what + " is not supported yet";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Os:
    if (llvm::is_contained(kOsFunctions, name) || name == "EOL" || name == "devNull")
      return "";
    if (llvm::is_contained(kOsExports, name))
      return what + " is not supported yet";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Process:
    if (llvm::is_contained(kProcessNames, name))
      return "";
    return what + " is not supported yet; tz has: argv, env, exit, exitCode, cwd, platform, "
                  "execPath, stdout and stderr";
  case NodeModule::Buffer:
    if (name == "Buffer")
      return "";
    if (llvm::is_contained(kBufferExports, name))
      return what + " is not supported yet; tz has Buffer";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::TzIo:
    if (name == "readLine")
      return "";
    return quoted + " has no export named '" + name.str() + "'; it has readLine";
  case NodeModule::ChildProcess:
    if (name == "spawnSync" || name == "execFileSync" || name == "execSync")
      return "";
    if (name == "spawn" || name == "exec" || name == "execFile")
      return what +
             " is not supported yet: it comes with EventEmitter and the streams of Node "
             "(docs/process.md, step 2); use '" +
             name.str() + "Sync', which waits for the program and gives what it printed";
    if (name == "fork")
      return what + " is not possible: it starts another Node with a channel to it, and tz has "
                    "no Node (docs/process.md, section 4); for parallel work use async functions "
                    "(docs/async.md), and to run a program 'spawnSync(process.execPath, [...])'";
    if (llvm::is_contained(kChildExports, name))
      return what + " is not supported yet; tz has spawnSync, execFileSync and execSync";
    return quoted + " has no export named '" + name.str() + "'";
  case NodeModule::Crypto:
    return cryptoExportProblem(name, path);
  }
  return "";
}

// The name of a module in messages.
std::string moduleLabel(NodeModule module) {
  switch (module) {
  case NodeModule::Fs:
    return "node:fs";
  case NodeModule::FsPromises:
    return "node:fs/promises";
  case NodeModule::TimersPromises:
    return "node:timers/promises";
  case NodeModule::Readline:
    return "node:readline";
  case NodeModule::ReadlinePromises:
    return "node:readline/promises";
  case NodeModule::Path:
    return "node:path";
  case NodeModule::PathPosix:
    return "node:path/posix";
  case NodeModule::PathWin32:
    return "node:path/win32";
  case NodeModule::Os:
    return "node:os";
  case NodeModule::Process:
    return "node:process";
  case NodeModule::Buffer:
    return "node:buffer";
  case NodeModule::TzIo:
    return "tz:io";
  case NodeModule::ChildProcess:
    return "node:child_process";
  case NodeModule::Crypto:
    return "node:crypto";
  }
  return "";
}

bool targetIsWindows() { return llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows(); }

const Type *parsedPathType() {
  const Type *s = Type::string();
  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "ParsedPath");
}

const Type *formatInputType() {
  const Type *s = Type::optional(Type::string());
  return Type::object({{"root", s, false},
                       {"dir", s, false},
                       {"base", s, false},
                       {"ext", s, false},
                       {"name", s, false}},
                      "FormatInputPathObject");
}

const Type *cpuInfoType() {
  const Type *d = Type::doubleTy();
  const Type *times = Type::object({{"user", d, false},
                                    {"nice", d, false},
                                    {"sys", d, false},
                                    {"idle", d, false},
                                    {"irq", d, false}});
  return Type::object(
      {{"model", Type::string(), false}, {"speed", d, false}, {"times", times, false}}, "CpuInfo");
}

const Type *userInfoType() {
  return Type::object({{"uid", Type::intTy(), false},
                       {"gid", Type::intTy(), false},
                       {"username", Type::string(), false},
                       {"homedir", Type::string(), false},
                       {"shell", Type::optional(Type::string()), false}},
                      "UserInfo");
}

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);
  return out;
}

// The kinds of files the tests of Stats and Dirent ask about, as the bits of
// a mode (S_IFREG...).
std::optional<int> fileKind(llvm::StringRef method) {
  if (method == "isFile")
    return 0100000;
  if (method == "isDirectory")
    return 0040000;
  if (method == "isSymbolicLink")
    return 0120000;
  if (method == "isFIFO")
    return 0010000;
  if (method == "isSocket")
    return 0140000;
  if (method == "isBlockDevice")
    return 0060000;
  if (method == "isCharacterDevice")
    return 0020000;
  return std::nullopt;
}

} // namespace

//===----------------------------------------------------------------------===//
// Imports
//===----------------------------------------------------------------------===//

std::string builtinModuleProblem(llvm::StringRef path) {
  return moduleOf(path) ? std::string() : missingModule(path);
}

void Sema::importBuiltinModule(Module *module, const ImportDecl &decl) {
  ModuleState &here = state(module);
  std::optional<NodeModule> which = moduleOf(decl.path);
  if (!which) {
    // Reported by the loader.
    for (const ModuleName &name : decl.names)
      here.badImports.insert(name.as);
    if (!decl.whole.empty())
      here.badImports.insert(decl.whole);
    return;
  }
  auto bind = [&](llvm::StringRef alias, llvm::SMLoc loc, NodeName n, llvm::StringRef name) {
    if (auto own = here.declared.find(alias); own != here.declared.end()) {
      diags_.error(loc, "import '" + alias + "' conflicts with the declaration of '" + alias +
                            "' in this file; import it under another name, e.g. 'import { " + name +
                            " as " + name + "2 } from \"" + decl.path + "\"'");
      diags_.note(own->second, "'" + alias + "' is declared here");
      return;
    }
    if (here.imports.count(alias) || !here.nodeNames.try_emplace(alias, n).second) {
      diags_.error(loc, "duplicate import of '" + alias + "'");
      return;
    }
  };

  // `import fs from "node:fs"`: an error, with the names the file uses of it
  // as the hint; its uses are checked as if they were imported.
  if (!decl.whole.empty()) {
    std::vector<llvm::StringRef> used;
    for (Stmt *item : module->file.items)
      visitExprs(item, [&](const Expr *e) {
        auto *member = dyn_cast<MemberExpr>(e);
        auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
        if (object && object->name == decl.whole && !llvm::is_contained(used, member->name))
          used.push_back(member->name);
      });
    std::string names;
    for (llvm::StringRef name : used)
      names += (names.empty() ? "" : ", ") + name.str();
    std::string form = decl.wholeIsDefault
                           ? "default imports are not supported: there is no 'export default'"
                           : "'import * as' is not supported";
    if (*which == NodeModule::Process && decl.whole == "process")
      diags_.error(decl.wholeLoc, form + "; 'process' is global, as in Node, so the import is "
                                         "not needed: drop it");
    else if (*which == NodeModule::Buffer)
      diags_.error(decl.wholeLoc, form + "; 'Buffer' is global, as in Node: import it as "
                                         "'import { Buffer } from \"" +
                                      decl.path + "\"', or drop the import");
    else if (!names.empty())
      diags_.error(decl.wholeLoc, form + "; import the names this file uses: 'import { " + names +
                                      " } from \"" + decl.path + "\"'");
    else
      diags_.error(decl.wholeLoc, form + "; import the names you need, e.g. 'import { " +
                                      typicalName(*which).str() + " } from \"" + decl.path + "\"'");
    bind(decl.whole, decl.wholeLoc, NodeName{*which, {}}, typicalName(*which));
  }

  for (const ModuleName &name : decl.names) {
    std::string problem = exportProblem(*which, name.name, decl.path);
    if (!problem.empty()) {
      diags_.error(name.loc, problem);
      here.badImports.insert(name.as);
      continue;
    }
    NodeName n{*which, name.name};
    // The namespaces of node:path: their functions take that style.
    if (isPathModule(*which) && (name.name == "posix" || name.name == "win32"))
      n = NodeName{name.name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
    // `promises` of node:fs is node:fs/promises.
    if (*which == NodeModule::Fs && name.name == "promises")
      n = NodeName{NodeModule::FsPromises, {}};
    bind(name.as, name.asLoc, n, name.name);
  }
}

const Sema::NodeName *Sema::nodeName(llvm::StringRef name) const {
  if (!module_ || module_->order >= moduleStates_.size())
    return nullptr;
  const llvm::StringMap<NodeName> &names = moduleStates_[module_->order].nodeNames;
  auto found = names.find(name);
  if (found == names.end())
    return nullptr;
  // `import { Buffer } from "node:buffer"` names the global Buffer itself.
  if (found->second.module == NodeModule::Buffer && name == "Buffer")
    return nullptr;
  // A name the code declares hides the import.
  ScopeEntry entry = lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls)
    return nullptr;
  return &found->second;
}

//===----------------------------------------------------------------------===//
// Names, members and calls
//===----------------------------------------------------------------------===//

namespace {

// `process.name`, of the global process.
MemberExpr *processMember(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef name) {
  auto *process = ctx.make<NameExpr>(loc, "process");
  return ctx.make<MemberExpr>(loc, process, name);
}

} // namespace

Expr *Sema::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const NodeName *n = name ? nodeName(name->name) : nullptr;
  // Buffer of node:buffer is the global one.
  if (n && n->module == NodeModule::Buffer && n->name == "Buffer")
    return ctx_.make<NameExpr>(name->loc(), "Buffer");
  if (!n || n->module != NodeModule::Process)
    return expr;
  if (n->name.empty())
    return ctx_.make<NameExpr>(name->loc(), "process");
  return processMember(ctx_, name->loc(), n->name);
}

Expr *Sema::nodeValue(const NodeName &n, Expr *expr) {
  expr->type = Type::error();
  std::string label = moduleLabel(n.module);
  if (n.name.empty()) {
    diags_.error(expr->loc(),
                 "this is not a value: the functions of '" + label +
                     "' are used by their names, e.g. '" +
                     (isPathModule(n.module) ? "posix.join(a, b)'"
                                             : typicalName(n.module).str() + "(...)'"));
    return expr;
  }
  auto literal = [&](llvm::StringRef text) {
    auto *lit = ctx_.make<StringLitExpr>(expr->loc(), utf16(text));
    lit->type = Type::string();
    return lit;
  };
  bool win32 =
      n.module == NodeModule::PathWin32 || (n.module == NodeModule::Path && targetIsWindows());
  if (isPathModule(n.module)) {
    if (n.name == "sep")
      return literal(win32 ? "\\" : "/");
    if (n.name == "delimiter")
      return literal(win32 ? ";" : ":");
    if (n.name == "posix" || n.name == "win32") {
      diags_.error(expr->loc(), "'" + n.name + "' of '" + label +
                                    "' is used through its functions, e.g. '" + n.name +
                                    ".join(a, b)'");
      return expr;
    }
  }
  if (n.module == NodeModule::Os) {
    if (n.name == "EOL")
      return literal(targetIsWindows() ? "\r\n" : "\n");
    if (n.name == "devNull")
      return literal(targetIsWindows() ? "\\\\.\\nul" : "/dev/null");
  }
  if ((n.module == NodeModule::Fs || n.module == NodeModule::FsPromises) &&
      n.name == "constants") {
    diags_.error(expr->loc(), "'constants' of 'node:fs' is used as 'constants.F_OK' and the like");
    return expr;
  }
  if (n.module == NodeModule::Crypto && n.name == "constants") {
    diags_.error(expr->loc(), "'constants' of 'node:crypto' is used as "
                              "'constants.RSA_PKCS1_PSS_PADDING' and the like");
    return expr;
  }
  if (n.module == NodeModule::Crypto && cryptoTypeNamed(n.name)) {
    diags_.error(expr->loc(), "'" + n.name + "' of 'node:crypto' is a type: its objects come from "
                                             "the functions of 'node:crypto'");
    return expr;
  }
  diags_.error(expr->loc(), "'" + n.name + "' of '" + label +
                                "' can only be called; where a function is expected, pass an "
                                "arrow that calls it, e.g. 'x => " +
                                n.name + "(x)'");
  return expr;
}

Expr *Sema::checkNodeName(NameExpr *name) {
  const NodeName *n = nodeName(name->name);
  if (!n)
    return nullptr;
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty()))
    return checkExpr(processAlias(name));
  return nodeValue(*n, name);
}

Expr *Sema::checkNodeMember(MemberExpr *member) {
  // `constants.F_OK` and `fs.constants.F_OK`.
  auto isFs = [](const NodeName *n) {
    return n && (n->module == NodeModule::Fs || n->module == NodeModule::FsPromises);
  };
  auto constantOf = [&](const Expr *object) {
    if (auto *name = dyn_cast<NameExpr>(object)) {
      const NodeName *n = nodeName(name->name);
      return isFs(n) && n->name == "constants";
    }
    auto *inner = dyn_cast<MemberExpr>(object);
    auto *name = inner ? dyn_cast<NameExpr>(inner->object) : nullptr;
    const NodeName *n = name ? nodeName(name->name) : nullptr;
    return isFs(n) && n->name.empty() && inner->name == "constants";
  };
  if (constantOf(member->object)) {
    // As Node has them on Windows, which are those of POSIX too.
    static const std::pair<llvm::StringLiteral, int> constants[] = {
        {"F_OK", 0}, {"R_OK", 4}, {"W_OK", 2}, {"X_OK", 1}, {"COPYFILE_EXCL", 1},
        {"COPYFILE_FICLONE", 2}, {"COPYFILE_FICLONE_FORCE", 4}};
    for (const auto &[name, value] : constants)
      if (name == member->name) {
        auto *lit = ctx_.make<IntLitExpr>(member->loc(), uint64_t(value));
        lit->type = Type::intTy();
        return lit;
      }
    diags_.error(member->loc(), "'constants." + member->name +
                                    "' of 'node:fs' is not supported yet; tz has F_OK, R_OK, "
                                    "W_OK, X_OK and the COPYFILE_ ones");
    member->type = Type::error();
    return member;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *n = object ? nodeName(object->name) : nullptr;
  if (!n)
    return nullptr;
  if (n->module == NodeModule::Crypto && n->name == "constants")
    return cryptoConstant(member);
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    member->object = processAlias(object);
    return nullptr;
  }
  if (!n->name.empty())
    return nullptr; // a member of a function: reported where the name is checked
  std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
  if (!problem.empty()) {
    diags_.error(member->loc(), problem);
    member->type = Type::error();
    return member;
  }
  NodeName inner{n->module, member->name};
  if (isPathModule(n->module) && (member->name == "posix" || member->name == "win32"))
    inner = {member->name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
  if (n->module == NodeModule::Fs && member->name == "promises")
    inner = {NodeModule::FsPromises, {}};
  return nodeValue(inner, member);
}

Expr *Sema::checkNodeCall(CallExpr *call, const Type *expected) {
  if (call->optional)
    return nullptr;
  NodeName target{NodeModule::Fs, {}};
  llvm::SMLoc loc;
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    const NodeName *n = nodeName(name->name);
    if (!n)
      return nullptr;
    if (n->module == NodeModule::Process ||
        (n->module == NodeModule::Buffer && !n->name.empty())) {
      call->callee = processAlias(name);
      return nullptr;
    }
    if (n->name.empty()) {
      diags_.error(name->loc(), "'" + name->name + "' is not a function: call the functions of '" +
                                    moduleLabel(n->module) + "' by their names");
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      call->type = Type::error();
      return call;
    }
    target = *n;
    loc = name->loc();
  } else if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !member->optional) {
    auto *object = dyn_cast<NameExpr>(member->object);
    const NodeName *n = object ? nodeName(object->name) : nullptr;
    if (!n)
      return nullptr;
    if (n->module == NodeModule::Process ||
        (n->module == NodeModule::Buffer && !n->name.empty())) {
      member->object = processAlias(object);
      return nullptr;
    }
    if (n->module == NodeModule::TimersPromises && n->name == "scheduler")
      return checkSchedulerCall(call, member);
    if (!n->name.empty())
      return nullptr; // a method of what the name is: reported there
    std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
    if (!problem.empty()) {
      diags_.error(member->loc(), problem);
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      call->type = Type::error();
      return call;
    }
    target = {n->module, member->name};
    loc = member->loc();
  } else {
    return nullptr;
  }

  switch (target.module) {
  case NodeModule::Path:
  case NodeModule::PathPosix:
  case NodeModule::PathWin32:
    return checkPathCall(call, target.module, target.name, loc);
  case NodeModule::Os:
    return checkOsCall(call, target.name, loc);
  case NodeModule::Fs:
    return checkFsCall(call, target.name, loc);
  case NodeModule::FsPromises:
    return checkFsPromisesCall(call, target.name, loc);
  case NodeModule::TimersPromises:
    return checkTimersPromisesCall(call, target.name, loc, expected);
  case NodeModule::Readline:
  case NodeModule::ReadlinePromises:
    return checkReadlineCall(call, target.module, target.name, loc);
  case NodeModule::ChildProcess:
    return checkChildCall(call, target.name, loc);
  case NodeModule::Crypto:
    return checkCryptoCall(call, target.name, loc);
  case NodeModule::Process:
  case NodeModule::Buffer:
    break;
  case NodeModule::TzIo: {
    // readLine(question?): the line, or undefined at the end of the input.
    if (rejectSpread(call) || !checkArity(call, loc, 0, 1))
      return call;
    if (!call->args.empty())
      call->args[0] = checkArg(call->args[0], Type::string());
    call->builtin = Builtin::ReadLine;
    call->type = Type::optional(Type::string());
    return call;
  }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// node:path and node:os
//===----------------------------------------------------------------------===//

Expr *Sema::checkPathCall(CallExpr *call, NodeModule module, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (fn == "sep" || fn == "delimiter") {
    diags_.error(loc, "'" + fn + "' is a string, not a function");
    return loosely();
  }
  if (fn == "posix" || fn == "win32") {
    diags_.error(loc, "'" + fn + "' is not a function: call its functions, e.g. '" + fn +
                          ".join(a, b)'");
    return loosely();
  }
  bool win32 = module == NodeModule::PathWin32 || (module == NodeModule::Path && targetIsWindows());
  auto *style = ctx_.make<IntLitExpr>(loc, win32 ? 1 : 0);
  style->type = Type::intTy();
  const Type *string = Type::string();

  if (fn == "join" || fn == "resolve") {
    // The paths become one array, spread ones too: `join(...parts)`.
    auto *paths = ctx_.make<ArrayLitExpr>(call->args.empty() ? loc : call->args[0]->loc());
    for (size_t i = 0; i < call->args.size(); ++i)
      paths->elements.push_back({call->args[i], call->spreads(i)});
    Expr *array = checkArg(paths, Type::array(string));
    call->args = {style, array};
    call->spread.clear();
    call->builtin = fn == "join" ? Builtin::PathJoin : Builtin::PathResolve;
    call->type = string;
    return call;
  }
  if (rejectSpread(call))
    return call;
  struct Shape {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned min, max;
  };
  static const Shape shapes[] = {
      {"normalize", Builtin::PathNormalize, 1, 1},
      {"isAbsolute", Builtin::PathIsAbsolute, 1, 1},
      {"dirname", Builtin::PathDirname, 1, 1},
      {"basename", Builtin::PathBasename, 1, 2},
      {"extname", Builtin::PathExtname, 1, 1},
      {"relative", Builtin::PathRelative, 2, 2},
      {"parse", Builtin::PathParse, 1, 1},
      {"format", Builtin::PathFormat, 1, 1},
      {"toNamespacedPath", Builtin::PathToNamespacedPath, 1, 1},
  };
  const Shape *shape = llvm::find_if(shapes, [&](const Shape &s) { return s.name == fn; });
  if (shape == std::end(shapes)) {
    diags_.error(loc, "'" + fn + "' of '" + moduleLabel(module) + "' is not supported yet");
    return loosely();
  }
  if (!checkArity(call, loc, shape->min, shape->max))
    return call;
  if (shape->builtin == Builtin::PathFormat) {
    // An object with some of root, dir, base, ext and name: a literal takes
    // the type of format's parameter; path.parse() gives all five.
    Expr *arg = checkExpr(call->args[0], formatInputType());
    const Type *type = arg->type;
    bool fits = type->isObject() && !type->isClass() && !type->isTuple() &&
                llvm::all_of(type->fields(), [&](const Field &f) {
                  return formatInputType()->field(f.name) &&
                         (f.type == string || f.type == Type::optional(string));
                });
    if (!fits && !type->isError())
      arg = coerce(arg, formatInputType());
    call->args = {style, arg};
    call->builtin = Builtin::PathFormat;
    call->type = string;
    return call;
  }
  for (Expr *&arg : call->args)
    arg = checkArg(arg, string);
  call->args.insert(call->args.begin(), style);
  call->builtin = shape->builtin;
  call->type = shape->builtin == Builtin::PathIsAbsolute ? Type::boolTy()
               : shape->builtin == Builtin::PathParse    ? parsedPathType()
                                                         : string;
  return call;
}

Expr *Sema::checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn == "EOL" || fn == "devNull") {
    diags_.error(loc, "'" + fn + "' is a string, not a function");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, loc, 0, 0))
    return call;
  auto which = [&](int value) {
    auto *lit = ctx_.make<IntLitExpr>(loc, uint64_t(value));
    lit->type = Type::intTy();
    call->args = {lit};
  };
  static const llvm::StringLiteral strings[] = {"homedir", "tmpdir",  "hostname", "arch",
                                                "type",    "release", "version",  "machine"};
  if (const auto *s = llvm::find(strings, fn); s != std::end(strings)) {
    which(int(s - std::begin(strings)));
    call->builtin = Builtin::OsString;
    call->type = Type::string();
    return call;
  }
  static const llvm::StringLiteral numbers[] = {"totalmem", "freemem", "uptime"};
  if (const auto *n = llvm::find(numbers, fn); n != std::end(numbers)) {
    which(int(n - std::begin(numbers)));
    call->builtin = Builtin::OsNumber;
    call->type = Type::doubleTy();
    return call;
  }
  if (fn == "platform") {
    call->builtin = Builtin::ProcessPlatform;
    call->type = Type::string();
    return call;
  }
  if (fn == "endianness") {
    // Every system tz builds for is little-endian.
    auto *lit = ctx_.make<StringLitExpr>(loc, u"LE");
    lit->type = Type::string();
    return lit;
  }
  if (fn == "availableParallelism") {
    call->builtin = Builtin::OsAvailableParallelism;
    call->type = Type::intTy();
    return call;
  }
  if (fn == "cpus") {
    call->builtin = Builtin::OsCpus;
    call->type = Type::array(cpuInfoType());
    return call;
  }
  if (fn == "loadavg") {
    call->builtin = Builtin::OsLoadavg;
    call->type = Type::array(Type::doubleTy());
    return call;
  }
  if (fn == "userInfo") {
    call->builtin = Builtin::OsUserInfo;
    call->type = userInfoType();
    return call;
  }
  diags_.error(loc, "'" + fn + "' of 'node:os' is not supported yet");
  return call;
}

//===----------------------------------------------------------------------===//
// node:fs
//===----------------------------------------------------------------------===//

bool Sema::nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                       llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options) {
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index]))
    return true;
  std::string list;
  for (size_t i = 0; i < allowed.size(); ++i)
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  Expr *arg = call->args[index];
  auto *lit = dyn_cast<ObjectLitExpr>(arg);
  if (!lit) {
    diags_.error(arg->loc(), "the options of '" + fn +
                                 "' are written in the call, as an object literal, e.g. '{ " +
                                 allowed[0].str() +
                                 ": ... }': the type of what it gives may depend on them");
    call->args[index] = checkExpr(arg);
    return false;
  }
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread || prop.key || prop.method || prop.name.empty()) {
      diags_.error(prop.loc, "write each option of '" + fn + "' by its name, e.g. '{ " +
                                 allowed[0].str() + ": ... }'");
      return false;
    }
    if (!llvm::is_contained(allowed, prop.name)) {
      diags_.error(prop.loc, "'" + fn + "' has no option '" + prop.name + "'; it takes " + list);
      return false;
    }
    options[prop.name] = prop.value;
  }
  return true;
}

Expr *Sema::checkEncoding(Expr *encoding, llvm::StringRef fn, bool any) {
  auto *lit = dyn_cast<StringLitExpr>(encoding);
  if (!lit) {
    diags_.error(encoding->loc(), "the encoding of '" + fn + "' is written as a literal, 'utf8'");
    checkExpr(encoding);
    return nullptr;
  }
  lit->type = Type::string();
  Expr *number = checkBufferEncoding(lit);
  if (number->type->isError())
    return nullptr;
  if (!any && cast<IntLitExpr>(number)->value != 0) {
    diags_.error(encoding->loc(), "'" + fn + "' gives names in utf8 only; drop the encoding '" +
                                      utf8(lit->value) + "'");
    return nullptr;
  }
  return number;
}

Expr *Sema::checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  // node:fs/promises checks its functions as those of node:fs that end in
  // Sync: `op` is that name, `fn` the one written.
  std::string syncName = fsAsync_ ? (fn + "Sync").str() : fn.str();
  llvm::StringRef op = syncName;
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (op == "constants" || op == "Stats" || op == "Dirent") {
    diags_.error(loc, "'" + fn + "' of 'node:fs' is not a function");
    return loosely();
  }
  if (rejectSpread(call))
    return call;
  const Type *string = Type::string();
  auto boolLit = [&](bool value) {
    auto *lit = ctx_.make<BoolLitExpr>(loc, value);
    lit->type = Type::boolTy();
    return lit;
  };
  auto intLit = [&](uint64_t value) {
    auto *lit = ctx_.make<IntLitExpr>(loc, value);
    lit->type = Type::intTy();
    return lit;
  };
  auto stringLit = [&](llvm::StringRef text) {
    auto *lit = ctx_.make<StringLitExpr>(loc, utf16(text));
    lit->type = Type::string();
    return lit;
  };
  // A path, or with `fd`, an int file descriptor too (0 is stdin); those of
  // node:fs/promises take a FileHandle instead.
  auto path = [&](Expr *arg, bool fd) {
    arg = checkExpr(arg, string);
    if (fd && !fsAsync_ && arg->type->isInt())
      return arg;
    return coerce(arg, string);
  };
  // A boolean option that decides the type, which must be a literal.
  auto literalFlag = [&](Expr *value, llvm::StringRef option, bool &out) {
    if (auto *lit = dyn_cast<BoolLitExpr>(value)) {
      lit->type = Type::boolTy();
      out = lit->value;
      return true;
    }
    diags_.error(value->loc(), "the option '" + option + "' of '" + fn +
                                   "' is written as a literal, true or false: the type of what "
                                   "it gives depends on it");
    checkExpr(value);
    return false;
  };
  llvm::StringMap<Expr *> options;
  // The argument at `index`: the encoding alone, or the options.
  auto encodingOrOptions = [&](size_t index, llvm::ArrayRef<llvm::StringRef> allowed) {
    if (index >= call->args.size())
      return true;
    Expr *arg = call->args[index];
    if (isa<StringLitExpr>(arg)) {
      options["encoding"] = arg;
      return true;
    }
    if (isa<ObjectLitExpr>(arg) || isa<UndefinedExpr>(arg))
      return nodeOptions(call, index, fn, allowed, options);
    arg = checkExpr(arg);
    if (arg->type->isString())
      diags_.error(arg->loc(), "the encoding of '" + fn +
                                   "' is written as a literal, 'utf8': the type of what it gives "
                                   "depends on it");
    else if (!arg->type->isError())
      diags_.error(arg->loc(), "the options of '" + fn +
                                   "' are written in the call, as an object literal, e.g. '{ " +
                                   allowed[0].str() +
                                   ": ... }': the type of what it gives may depend on them");
    return false;
  };

  if (op == "readFileSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *file = path(call->args[0], /*fd=*/true);
    using Names = llvm::ArrayRef<llvm::StringRef>;
    if (!encodingOrOptions(1, fsAsync_ ? Names{"encoding", "flag", "signal"}
                                       : Names{"encoding", "flag"}))
      return call;
    Expr *encoding = options.lookup("encoding");
    Expr *number = nullptr;
    if (encoding && !isa<UndefinedExpr>(encoding) && !(number = checkEncoding(encoding, fn, true)))
      return call;
    Expr *flag = options.lookup("flag");
    call->args = {file, flag ? checkArg(flag, string) : stringLit("r")};
    if (fsAsync_)
      call->args.push_back(signalOption(options.lookup("signal"), loc));
    call->builtin = Builtin::FsReadFile;
    // Without an encoding, the bytes, as Node gives them: a Buffer.
    call->type = number ? string : Type::bytesType(Type::Bytes::Buffer);
    if (number)
      call->args.push_back(number);
    return call;
  }
  if (op == "writeFileSync" || op == "appendFileSync") {
    if (!checkArity(call, loc, 2, 3))
      return call;
    Expr *file = path(call->args[0], /*fd=*/true);
    Expr *data = checkExpr(call->args[1], string);
    // Text, or the bytes of a typed array, a Buffer or a DataView (or of a
    // union of them, a pointer to one as well).
    auto view = [](const Type *t) { return t->isByteView(); };
    bool bytes = view(data->type) ||
                 (data->type->isUnion() && llvm::all_of(data->type->members(), view));
    if (!data->type->isString() && !bytes && !data->type->isError()) {
      std::string hint = data->type->isNumeric() || data->type->isBool()
                             ? "; convert it first, e.g. 'String(x)'"
                         : data->type->isBytes()
                             ? "; write a view of it, e.g. 'new Uint8Array(buffer)'"
                             : "";
      diags_.error(data->loc(), "'" + fn + "' writes a string or bytes (a Uint8Array or a "
                                           "Buffer); found '" +
                                    data->type->name() + "'" + hint);
      return loosely();
    }
    using Names = llvm::ArrayRef<llvm::StringRef>;
    if (!encodingOrOptions(2, fsAsync_ ? Names{"encoding", "flag", "mode", "signal"}
                                       : Names{"encoding", "flag", "mode"}))
      return call;
    Expr *number = nullptr;
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding)) {
      if (bytes) {
        // Node ignores it: the bytes are written as they are.
        diags_.error(encoding->loc(), "an encoding is for text; '" + fn +
                                          "' writes the bytes as they are: drop the encoding");
        return call;
      }
      if (!(number = checkEncoding(encoding, fn, true)))
        return call;
    }
    Expr *flag = options.lookup("flag");
    Expr *mode = options.lookup("mode");
    call->args = {file, data,
                  flag ? checkArg(flag, string) : stringLit(op == "writeFileSync" ? "w" : "a"),
                  mode ? checkArg(mode, Type::intTy()) : intLit(0666)};
    if (!bytes)
      call->args.push_back(number ? number : intLit(0)); // utf8
    if (fsAsync_)
      call->args.push_back(signalOption(options.lookup("signal"), loc));
    call->builtin = Builtin::FsWriteFile;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "existsSync" || op == "unlinkSync" || op == "realpathSync" || op == "mkdtempSync") {
    unsigned max = op == "existsSync" || op == "unlinkSync" ? 1 : 2;
    if (!checkArity(call, loc, 1, max))
      return call;
    Expr *file = path(call->args[0], false);
    if (max == 2 && !encodingOrOptions(1, {"encoding"}))
      return call;
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding))
      if (!checkEncoding(encoding, fn, false))
        return call;
    call->args = {file};
    call->builtin = op == "existsSync"     ? Builtin::FsExists
                    : op == "unlinkSync"   ? Builtin::FsUnlink
                    : op == "realpathSync" ? Builtin::FsRealpath
                                           : Builtin::FsMkdtemp;
    call->type = op == "existsSync" ? Type::boolTy() : op == "unlinkSync" ? Type::voidTy() : string;
    return call;
  }
  if (op == "accessSync" || op == "chmodSync" || op == "truncateSync") {
    if (!checkArity(call, loc, op == "chmodSync" ? 2 : 1, 2))
      return call;
    Expr *file = path(call->args[0], false);
    Expr *value;
    if (op == "truncateSync") {
      value = call->args.size() > 1 ? checkArg(call->args[1], Type::longTy()) : nullptr;
      if (!value) {
        auto *zero = ctx_.make<IntLitExpr>(loc, 0);
        zero->type = Type::longTy();
        value = zero;
      }
    } else if (op == "chmodSync" && isa<StringLitExpr>(call->args[1])) {
      // An octal string, as Node takes it: "755".
      std::string text = utf8(cast<StringLitExpr>(call->args[1])->value);
      unsigned mode = 0;
      if (text.empty() || llvm::StringRef(text).getAsInteger(8, mode)) {
        diags_.error(call->args[1]->loc(), "the mode of 'chmodSync' is a number, e.g. 0o644, or "
                                           "octal digits in a string, e.g. \"644\"");
        return call;
      }
      value = intLit(mode);
    } else {
      value = call->args.size() > 1 ? checkArg(call->args[1], Type::intTy()) : intLit(0);
    }
    call->args = {file, value};
    call->builtin = op == "accessSync"  ? Builtin::FsAccess
                    : op == "chmodSync" ? Builtin::FsChmod
                                        : Builtin::FsTruncate;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "statSync" || op == "lstatSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *file = path(call->args[0], false);
    // Only statSync has throwIfNoEntry.
    if (!nodeOptions(call, 1, fn,
                     fsAsync_ ? llvm::ArrayRef<llvm::StringRef>{"bigint"}
                              : llvm::ArrayRef<llvm::StringRef>{"throwIfNoEntry", "bigint"},
                     options))
      return call;
    bool throwIfMissing = true, big = false;
    if (Expr *value = options.lookup("throwIfNoEntry");
        value && !literalFlag(value, "throwIfNoEntry", throwIfMissing))
      return call;
    if (Expr *value = options.lookup("bigint"); value && !literalFlag(value, "bigint", big))
      return call;
    if (big) {
      diags_.error(options.lookup("bigint")->loc(), "'bigint' is not supported: tz has no bigint; "
                                                    "the numbers of Stats are doubles");
      return call;
    }
    call->args = {file, boolLit(op == "statSync"), boolLit(throwIfMissing)};
    call->builtin = Builtin::FsStat;
    call->type = throwIfMissing ? statsClass_->type : Type::optional(statsClass_->type);
    return call;
  }
  if (op == "readdirSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *dir = path(call->args[0], false);
    if (!encodingOrOptions(1, {"encoding", "withFileTypes", "recursive"}))
      return call;
    if (Expr *encoding = options.lookup("encoding"); encoding && !isa<UndefinedExpr>(encoding))
      if (!checkEncoding(encoding, fn, false))
        return call;
    bool types = false;
    if (Expr *value = options.lookup("withFileTypes");
        value && !literalFlag(value, "withFileTypes", types))
      return call;
    Expr *recursive = options.lookup("recursive");
    call->args = {dir, recursive ? checkArg(recursive, Type::boolTy()) : boolLit(false)};
    call->builtin = Builtin::FsReaddir;
    call->type = Type::array(types ? direntClass_->type : string);
    return call;
  }
  if (op == "mkdirSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *dir = path(call->args[0], false);
    Expr *mode = nullptr;
    if (call->args.size() > 1 && !isa<ObjectLitExpr>(call->args[1]) &&
        !isa<UndefinedExpr>(call->args[1])) {
      mode = checkArg(call->args[1], Type::intTy()); // mkdirSync(path, 0o755)
    } else {
      if (!nodeOptions(call, 1, fn, {"recursive", "mode"}, options))
        return call;
      if (Expr *value = options.lookup("mode"))
        mode = checkArg(value, Type::intTy());
    }
    Expr *recursive = options.lookup("recursive");
    // The first directory made, with `recursive`; undefined otherwise, as
    // in Node.
    call->args = {dir, recursive ? checkArg(recursive, Type::boolTy()) : boolLit(false),
                  mode ? mode : intLit(0777)};
    call->builtin = Builtin::FsMkdir;
    call->type = Type::optional(string);
    return call;
  }
  if (op == "rmdirSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *dir = path(call->args[0], false);
    if (call->args.size() > 1 && !isa<UndefinedExpr>(call->args[1])) {
      diags_.error(call->args[1]->loc(), "the options of 'rmdirSync' are deprecated in Node; to "
                                         "delete a directory with what is in it, use 'rmSync(path, "
                                         "{ recursive: true })'");
      return loosely();
    }
    call->args = {dir};
    call->builtin = Builtin::FsRmdir;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "rmSync") {
    if (!checkArity(call, loc, 1, 2))
      return call;
    Expr *target = path(call->args[0], false);
    if (!nodeOptions(call, 1, fn, {"recursive", "force", "maxRetries", "retryDelay"}, options))
      return call;
    Expr *recursive = options.lookup("recursive");
    Expr *force = options.lookup("force");
    // Retries are for systems where another program may hold a file: the
    // values are checked, and nothing waits.
    for (llvm::StringRef retry : {"maxRetries", "retryDelay"})
      if (Expr *value = options.lookup(retry))
        checkArg(value, Type::intTy());
    call->args = {target, recursive ? checkArg(recursive, Type::boolTy()) : boolLit(false),
                  force ? checkArg(force, Type::boolTy()) : boolLit(false)};
    call->builtin = Builtin::FsRm;
    call->type = Type::voidTy();
    return call;
  }
  if (op == "renameSync" || op == "copyFileSync") {
    if (!checkArity(call, loc, 2, op == "renameSync" ? 2 : 3))
      return call;
    Expr *from = path(call->args[0], false);
    Expr *to = path(call->args[1], false);
    if (op == "renameSync") {
      call->args = {from, to};
      call->builtin = Builtin::FsRename;
    } else {
      Expr *mode = call->args.size() > 2 ? checkArg(call->args[2], Type::intTy()) : intLit(0);
      call->args = {from, to, mode};
      call->builtin = Builtin::FsCopyFile;
    }
    call->type = Type::voidTy();
    return call;
  }
  diags_.error(loc, exportProblem(NodeModule::Fs, fn, "node:fs"));
  return loosely();
}

//===----------------------------------------------------------------------===//
// Stats and Dirent
//===----------------------------------------------------------------------===//

void Sema::declareFsClasses() {
  auto declare = [&](llvm::StringRef name,
                     llvm::ArrayRef<std::pair<llvm::StringRef, const Type *>> fields) {
    auto *cls = ctx_.make<ClassDecl>(llvm::SMLoc(), name);
    cls->builtin = true;
    cls->resolved = cls->declared = true;
    classes_.push_back(cls);
    Type *type = Type::classType(name, cls);
    cls->type = type;
    std::vector<Field> typeFields;
    for (const auto &[fieldName, fieldType] : fields) {
      ClassField f;
      f.name = fieldName;
      f.resolved = fieldType;
      f.access = fieldName.starts_with("#") ? Access::Private : Access::Public;
      cls->fields.push_back(f);
      typeFields.push_back({fieldName.str(), fieldType, false});
    }
    type->defineFields(std::move(typeFields));
    return cls;
  };
  // The numbers of a Stats, in the order Node prints them. Its dates
  // (`mtime`...) are made from them when read, as Node's getters make them,
  // and so are no fields: Node prints none (see checkStatsDate).
  const Type *d = Type::doubleTy();
  statsClass_ = declare("Stats", {{"dev", d}, {"mode", d}, {"nlink", d}, {"uid", d}, {"gid", d},
                                  {"rdev", d}, {"blksize", d}, {"ino", d}, {"size", d},
                                  {"blocks", d}, {"atimeMs", d}, {"mtimeMs", d}, {"ctimeMs", d},
                                  {"birthtimeMs", d}});
  // A Dirent keeps the kind of the file as the bits of a mode, which only
  // its tests read.
  direntClass_ = declare("Dirent", {{"name", Type::string()},
                                    {"parentPath", Type::string()},
                                    {"#type", Type::intTy()}});
}

// `st.atime`, `st.mtime`, `st.ctime` and `st.birthtime`: a new date from
// the number of milliseconds next to it; null for another name.
Expr *Sema::checkStatsDate(MemberExpr *member, ClassDecl *cls) {
  static const llvm::StringLiteral dates[] = {"atime", "mtime", "ctime", "birthtime"};
  if (cls != statsClass_ || !llvm::is_contained(dates, member->name))
    return nullptr;
  return builtinCall(member->loc(), member, Builtin::FsStatsDate, {}, Type::date());
}

bool Sema::isRuntimeClass(const ClassDecl *cls) const {
  return cls && (cls == systemErrorClass_ || cls == statsClass_ || cls == direntClass_ ||
                 cls == execSyncErrorClass_);
}

Expr *Sema::checkFsClassMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls) {
  if (cls != statsClass_ && cls != direntClass_)
    return nullptr;
  std::optional<int> kind = fileKind(member->name);
  if (!kind)
    return nullptr;
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 0))
    return call;
  auto *lit = ctx_.make<IntLitExpr>(member->loc(), uint64_t(*kind));
  lit->type = Type::intTy();
  call->args = {lit};
  call->builtin = Builtin::FsIsKind;
  call->type = Type::boolTy();
  member->type = Type::function({}, 0, false, Type::boolTy());
  return call;
}

} // namespace tz
