#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/path/path_checker/path_checker.h"
#include "compiler/library/os/os_checker/os_checker.h"
#include "compiler/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/library/fs/readline_checker/readline_checker.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The modules tz has, by the name after "node:".
std::optional<NodeModule> moduleOf(llvm::StringRef path) {
  if (path == "tz:io") {
    return NodeModule::TzIo;
  }
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  if (bare == "fs") {
    return NodeModule::Fs;
  }
  if (bare == "fs/promises") {
    return NodeModule::FsPromises;
  }
  if (bare == "timers/promises") {
    return NodeModule::TimersPromises;
  }
  if (bare == "readline") {
    return NodeModule::Readline;
  }
  if (bare == "readline/promises") {
    return NodeModule::ReadlinePromises;
  }
  if (bare == "path") {
    return NodeModule::Path;
  }
  if (bare == "path/posix") {
    return NodeModule::PathPosix;
  }
  if (bare == "path/win32") {
    return NodeModule::PathWin32;
  }
  if (bare == "os") {
    return NodeModule::Os;
  }
  if (bare == "process") {
    return NodeModule::Process;
  }
  if (bare == "buffer") {
    return NodeModule::Buffer;
  }
  if (bare == "child_process") {
    return NodeModule::ChildProcess;
  }
  if (bare == "crypto") {
    return NodeModule::Crypto;
  }

  return std::nullopt;
}

// Why a built-in module that tz does not have is not there.
std::string missingModule(llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (path.starts_with("tz:")) {
    if (path == "tz:string") {
      return "built-in module " + quoted + " is not supported yet";
    }
    return "there is no built-in module " + quoted + "; tz has 'tz:io' and 'tz:http'";
  }
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  if (!isBuiltinModule(bare) && !path.starts_with("node:sea") && path != "node:sqlite" &&
      !path.starts_with("node:test")) {
    return "there is no built-in module " + quoted;
  }
  std::string message = "built-in module " + quoted + " is not supported yet";
  if (bare == "timers") {
    return message + "; setTimeout and the other timers are global, as in Node: drop the import";
  }
  if (bare == "http" || bare == "https") {
    return message + " (docs/net.md, section 7); serve requests with 'serve' from \"tz:http\", "
                     "and make them with the global 'fetch'";
  }
  static const llvm::StringLiteral network[] = {"http2", "net", "tls",
                                                "dgram", "dns", "dns/promises"};
  if (llvm::is_contained(network, bare)) {
    return message + " (docs/net.md, section 7)";
  }
  if (bare == "events") {
    return message + " (docs/events.md)";
  }
  if (bare.starts_with("stream")) {
    return message + " (docs/streams.md)";
  }

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

bool targetIsWindows() { return llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows(); }

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);

  return std::u16string(units.begin(), units.end());
}

} // namespace

std::string builtinModuleProblem(llvm::StringRef path) {
  return moduleOf(path) ? std::string() : missingModule(path);
}

namespace {

// `process.name`, of the global process.
MemberExpr *processMember(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef name) {
  auto *process = ctx.make<NameExpr>(loc, "process");

  return ctx.make<MemberExpr>(loc, process, name);
}

} // namespace

BuiltinModuleChecker::BuiltinModuleChecker(Sema &sema) : sema_(sema) {}

void BuiltinModuleChecker::importBuiltinModule(Module *module, const ImportDecl &decl) {
  Sema::ModuleState &here = sema_.state(module);
  std::optional<NodeModule> which = moduleOf(decl.path);
  if (!which) {
    // Reported by the loader.
    for (const ModuleName &name : decl.names) {
      here.badImports.insert(name.as);
    }
    if (!decl.whole.empty()) {
      here.badImports.insert(decl.whole);
    }
    return;
  }
  auto bind = [&](llvm::StringRef alias, llvm::SMLoc loc, Sema::NodeName n, llvm::StringRef name) {
    if (auto own = here.declared.find(alias); own != here.declared.end()) {
      sema_.diags_.error(loc, "import '" + alias + "' conflicts with the declaration of '" + alias +
                            "' in this file; import it under another name, e.g. 'import { " + name +
                            " as " + name + "2 } from \"" + decl.path + "\"'");
      sema_.diags_.note(own->second, "'" + alias + "' is declared here");
      return;
    }
    if (here.imports.count(alias) || !here.nodeNames.try_emplace(alias, n).second) {
      sema_.diags_.error(loc, "duplicate import of '" + alias + "'");
      return;
    }
  };

  // `import fs from "node:fs"`: an error, with the names the file uses of it
  // as the hint; its uses are checked as if they were imported.
  if (!decl.whole.empty()) {
    std::vector<llvm::StringRef> used;
    for (Stmt *item : module->file.items) {
      visitExprs(item, [&](const Expr *e) {
        auto *member = dyn_cast<MemberExpr>(e);
        auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
        if (object && object->name == decl.whole && !llvm::is_contained(used, member->name)) {
          used.push_back(member->name);
        }
      });
    }
    std::string names;
    for (llvm::StringRef name : used) {
      names += (names.empty() ? "" : ", ") + name.str();
    }
    std::string form = decl.wholeIsDefault
                           ? "default imports are not supported: there is no 'export default'"
                           : "'import * as' is not supported";
    if (*which == NodeModule::Process && decl.whole == "process") {
      sema_.diags_.error(decl.wholeLoc, form + "; 'process' is global, as in Node, so the import is "
                                         "not needed: drop it");
    } else if (*which == NodeModule::Buffer) {
      sema_.diags_.error(decl.wholeLoc, form +
                                      "; 'Buffer' is global, as in Node: import it as "
                                      "'import { Buffer } from \"" +
                                      decl.path + "\"', or drop the import");
    } else if (!names.empty()) {
      sema_.diags_.error(decl.wholeLoc, form + "; import the names this file uses: 'import { " + names +
                                      " } from \"" + decl.path + "\"'");
    } else {
      sema_.diags_.error(decl.wholeLoc, form + "; import the names you need, e.g. 'import { " +
                                      typicalName(*which).str() + " } from \"" + decl.path + "\"'");
    }
    bind(decl.whole, decl.wholeLoc, Sema::NodeName{*which, {}}, typicalName(*which));
  }

  for (const ModuleName &name : decl.names) {
    std::string problem = exportProblem(*which, name.name, decl.path);
    if (!problem.empty()) {
      sema_.diags_.error(name.loc, problem);
      here.badImports.insert(name.as);
      continue;
    }
    Sema::NodeName n{*which, name.name};
    // The namespaces of node:path: their functions take that style.
    if (isPathModule(*which) && (name.name == "posix" || name.name == "win32")) {
      n = Sema::NodeName{name.name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
    }
    // `promises` of node:fs is node:fs/promises.
    if (*which == NodeModule::Fs && name.name == "promises") {
      n = Sema::NodeName{NodeModule::FsPromises, {}};
    }
    bind(name.as, name.asLoc, n, name.name);
  }
}

const Sema::NodeName *BuiltinModuleChecker::nodeName(llvm::StringRef name) const {
  if (!sema_.module_ || sema_.module_->order >= sema_.moduleStates_.size()) {
    return nullptr;
  }
  const llvm::StringMap<Sema::NodeName> &names = sema_.moduleStates_[sema_.module_->order].nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  // `import { Buffer } from "node:buffer"` names the global Buffer itself.
  if (found->second.module == NodeModule::Buffer && name == "Buffer") {
    return nullptr;
  }
  // A name the code declares hides the import.
  Sema::ScopeEntry entry = sema_.scopes().lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls) {
    return nullptr;
  }

  return &found->second;
}

Expr *BuiltinModuleChecker::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const Sema::NodeName *n = name ? nodeName(name->name) : nullptr;
  // Buffer of node:buffer is the global one.
  if (n && n->module == NodeModule::Buffer && n->name == "Buffer") {
    return sema_.ctx_.make<NameExpr>(name->loc(), "Buffer");
  }
  if (!n || n->module != NodeModule::Process) {
    return expr;
  }
  if (n->name.empty()) {
    return sema_.ctx_.make<NameExpr>(name->loc(), "process");
  }

  return processMember(sema_.ctx_, name->loc(), n->name);
}

Expr *BuiltinModuleChecker::nodeValue(const Sema::NodeName &n, Expr *expr) {
  expr->type = Type::error();
  std::string label = moduleLabel(n.module);
  if (n.name.empty()) {
    sema_.diags_.error(expr->loc(),
                 "this is not a value: the functions of '" + label +
                     "' are used by their names, e.g. '" +
                     (isPathModule(n.module) ? "posix.join(a, b)'"
                                             : typicalName(n.module).str() + "(...)'"));
    return expr;
  }
  auto literal = [&](llvm::StringRef text) {
    auto *lit = sema_.ctx_.make<StringLitExpr>(expr->loc(), utf16(text));
    lit->type = Type::string();

    return lit;
  };
  bool win32 =
      n.module == NodeModule::PathWin32 || (n.module == NodeModule::Path && targetIsWindows());
  if (isPathModule(n.module)) {
    if (n.name == "sep") {
      return literal(win32 ? "\\" : "/");
    }
    if (n.name == "delimiter") {
      return literal(win32 ? ";" : ":");
    }
    if (n.name == "posix" || n.name == "win32") {
      sema_.diags_.error(expr->loc(), "'" + n.name + "' of '" + label +
                                    "' is used through its functions, e.g. '" + n.name +
                                    ".join(a, b)'");
      return expr;
    }
  }
  if (n.module == NodeModule::Os) {
    if (n.name == "EOL") {
      return literal(targetIsWindows() ? "\r\n" : "\n");
    }
    if (n.name == "devNull") {
      return literal(targetIsWindows() ? "\\\\.\\nul" : "/dev/null");
    }
  }
  if ((n.module == NodeModule::Fs || n.module == NodeModule::FsPromises) && n.name == "constants") {
    sema_.diags_.error(expr->loc(), "'constants' of 'node:fs' is used as 'constants.F_OK' and the like");
    return expr;
  }
  if (n.module == NodeModule::Crypto && n.name == "constants") {
    sema_.diags_.error(expr->loc(), "'constants' of 'node:crypto' is used as "
                              "'constants.RSA_PKCS1_PSS_PADDING' and the like");
    return expr;
  }
  if (n.module == NodeModule::Crypto && sema_.cryptoChecker().cryptoTypeNamed(n.name)) {
    sema_.diags_.error(expr->loc(), "'" + n.name +
                                  "' of 'node:crypto' is a type: its objects come from "
                                  "the functions of 'node:crypto'");
    return expr;
  }
  sema_.diags_.error(expr->loc(), "'" + n.name + "' of '" + label +
                                "' can only be called; where a function is expected, pass an "
                                "arrow that calls it, e.g. 'x => " +
                                n.name + "(x)'");

  return expr;
}

Expr *BuiltinModuleChecker::checkNodeName(NameExpr *name) {
  const Sema::NodeName *n = nodeName(name->name);
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    return sema_.exprChecker().checkExpr(processAlias(name));
  }

  return nodeValue(*n, name);
}

Expr *BuiltinModuleChecker::checkNodeMember(MemberExpr *member) {
  // `constants.F_OK` and `fs.constants.F_OK`.
  auto isFs = [](const Sema::NodeName *n) {
    return n && (n->module == NodeModule::Fs || n->module == NodeModule::FsPromises);
  };
  auto constantOf = [&](const Expr *object) {
    if (auto *name = dyn_cast<NameExpr>(object)) {
      const Sema::NodeName *n = nodeName(name->name);
      return isFs(n) && n->name == "constants";
    }
    auto *inner = dyn_cast<MemberExpr>(object);
    auto *name = inner ? dyn_cast<NameExpr>(inner->object) : nullptr;
    const Sema::NodeName *n = name ? nodeName(name->name) : nullptr;

    return isFs(n) && n->name.empty() && inner->name == "constants";
  };
  if (constantOf(member->object)) {
    // As Node has them on Windows, which are those of POSIX too.
    static const std::pair<llvm::StringLiteral, int> constants[] = {{"F_OK", 0},
                                                                    {"R_OK", 4},
                                                                    {"W_OK", 2},
                                                                    {"X_OK", 1},
                                                                    {"COPYFILE_EXCL", 1},
                                                                    {"COPYFILE_FICLONE", 2},
                                                                    {"COPYFILE_FICLONE_FORCE", 4}};
    for (const auto &[name, value] : constants) {
      if (name == member->name) {
        auto *lit = sema_.ctx_.make<IntLitExpr>(member->loc(), uint64_t(value));
        lit->type = Type::intTy();
        return lit;
      }
    }
    sema_.diags_.error(member->loc(), "'constants." + member->name +
                                    "' of 'node:fs' is not supported yet; tz has F_OK, R_OK, "
                                    "W_OK, X_OK and the COPYFILE_ ones");
    member->type = Type::error();
    return member;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const Sema::NodeName *n = object ? nodeName(object->name) : nullptr;
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Crypto && n->name == "constants") {
    return sema_.cryptoChecker().cryptoConstant(member);
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    member->object = processAlias(object);
    return nullptr;
  }
  if (!n->name.empty()) {
    return nullptr; // a member of a function: reported where the name is checked
  }
  std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
  if (!problem.empty()) {
    sema_.diags_.error(member->loc(), problem);
    member->type = Type::error();
    return member;
  }
  Sema::NodeName inner{n->module, member->name};
  if (isPathModule(n->module) && (member->name == "posix" || member->name == "win32")) {
    inner = {member->name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
  }
  if (n->module == NodeModule::Fs && member->name == "promises") {
    inner = {NodeModule::FsPromises, {}};
  }

  return nodeValue(inner, member);
}

Expr *BuiltinModuleChecker::checkNodeCall(CallExpr *call, const Type *expected) {
  if (call->optional) {
    return nullptr;
  }
  Sema::NodeName target{NodeModule::Fs, {}};
  llvm::SMLoc loc;
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    const Sema::NodeName *n = nodeName(name->name);
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      call->callee = processAlias(name);
      return nullptr;
    }
    if (n->name.empty()) {
      sema_.diags_.error(name->loc(), "'" + name->name + "' is not a function: call the functions of '" +
                                    moduleLabel(n->module) + "' by their names");
      for (Expr *&arg : call->args) {
        arg = sema_.exprChecker().checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = *n;
    loc = name->loc();
  } else if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !member->optional) {
    auto *object = dyn_cast<NameExpr>(member->object);
    const Sema::NodeName *n = object ? nodeName(object->name) : nullptr;
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      member->object = processAlias(object);
      return nullptr;
    }
    if (n->module == NodeModule::TimersPromises && n->name == "scheduler") {
      return sema_.timerChecker().checkSchedulerCall(call, member);
    }
    if (!n->name.empty()) {
      return nullptr; // a method of what the name is: reported there
    }
    std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
    if (!problem.empty()) {
      sema_.diags_.error(member->loc(), problem);
      for (Expr *&arg : call->args) {
        arg = sema_.exprChecker().checkExpr(arg);
      }
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
    return sema_.pathChecker().checkPathCall(call, target.module, target.name, loc);
  case NodeModule::Os:
    return sema_.osChecker().checkOsCall(call, target.name, loc);
  case NodeModule::Fs:
    return sema_.fsChecker().checkFsCall(call, target.name, loc);
  case NodeModule::FsPromises:
    return sema_.fsPromisesChecker().checkFsPromisesCall(call, target.name, loc);
  case NodeModule::TimersPromises:
    return sema_.timerChecker().checkTimersPromisesCall(call, target.name, loc, expected);
  case NodeModule::Readline:
  case NodeModule::ReadlinePromises:
    return sema_.readlineChecker().checkReadlineCall(call, target.module, target.name, loc);
  case NodeModule::ChildProcess:
    return sema_.childChecker().checkChildCall(call, target.name, loc);
  case NodeModule::Crypto:
    return sema_.cryptoFunctionChecker().checkCryptoCall(call, target.name, loc);
  case NodeModule::Process:
  case NodeModule::Buffer:
    break;
  case NodeModule::TzIo: {
    // readLine(question?): the line, or undefined at the end of the input.
    if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, loc, 0, 1)) {
      return call;
    }
    if (!call->args.empty()) {
      call->args[0] = sema_.argumentChecker().checkArg(call->args[0], Type::string());
    }
    call->builtin = Builtin::ReadLine;
    call->type = Type::optional(Type::string());
    return call;
  }
  }

  return nullptr;
}

} // namespace tz
