#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

std::optional<NodeModule> BuiltinModuleChecker::moduleOf(llvm::StringRef path) {
  if (path == "tz:io") {
    return NodeModule::TzIo;
  }
  if (path == "tz:fs") {
    return NodeModule::TzFs;
  }
  if (path == "tz:crypto") {
    return NodeModule::TzCrypto;
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

std::string BuiltinModuleChecker::missingModule(llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (path.starts_with("tz:")) {
    if (path == "tz:string") {
      return "built-in module " + quoted + " is not supported yet";
    }
    return "there is no built-in module " + quoted +
           "; tz has 'tz:io', 'tz:fs', 'tz:crypto', 'tz:http', 'tz:http2' and 'tz:sync'";
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
    return message + " (docs/net.md, section 7); serve requests with 'Server' from \"tz:http\", "
                     "and make them with the global 'fetch'";
  }
  if (bare == "http2") {
    return message + " (docs/net.md, section 7); HTTP/2 streams are in \"tz:http2\": "
                     "'Http2Server' and 'Http2Client'";
  }
  static const llvm::StringLiteral network[] = {"tls", "dns", "dns/promises"};
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

llvm::StringRef BuiltinModuleChecker::typicalName(NodeModule module) {
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
  case NodeModule::TzFs:
    return "replaceSync";
  case NodeModule::TzCrypto:
    return "randomBytesAsync";
  case NodeModule::ChildProcess:
    return "execFileSync";
  case NodeModule::Crypto:
    return "createHash";
  }

  return "x";
}

bool BuiltinModuleChecker::isPathModule(NodeModule module) {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

std::string BuiltinModuleChecker::moduleProblem(llvm::StringRef path) {
  if (BuiltinModuleChecker::moduleOf(path)) {
    return "";
  }

  return BuiltinModuleChecker::missingModule(path);
}

MemberExpr *BuiltinModuleChecker::processMember(AstContext &ctx, llvm::SMLoc loc,
                                                llvm::StringRef name) {
  auto *process = ctx.make<NameExpr>(loc, "process");

  return ctx.make<MemberExpr>(loc, process, name);
}

BuiltinModuleChecker::BuiltinModuleChecker(Checker &owner)
    : checker(owner) {}

std::string BuiltinModuleChecker::exportProblem(NodeModule module, llvm::StringRef name,
                                                llvm::StringRef path) const {
  if (this->checker.builtinModuleTypes().exported(module, name)) {
    return "";
  }
  for (const Feature *feature : this->checker.features_->all()) {
    if (std::optional<std::string> problem = feature->exportProblem(module, name, path)) {
      return *problem;
    }
  }

  return "";
}

void BuiltinModuleChecker::importBuiltinModule(Module *module, const ImportDecl &decl) {
  Checker::ModuleState &here = this->checker.state(module);
  std::optional<NodeModule> which = BuiltinModuleChecker::moduleOf(decl.path);
  if (!which) {
    for (const ModuleName &name : decl.names) {
      here.badImports.insert(name.as);
    }
    if (!decl.whole.empty()) {
      here.badImports.insert(decl.whole);
    }
    return;
  }
  auto bind = [&](llvm::StringRef alias, llvm::SMLoc loc, Checker::NodeName n,
                  llvm::StringRef name) {
    if (auto own = here.declared.find(alias); own != here.declared.end()) {
      this->checker.diags_.error(
          loc, "import '" + alias + "' conflicts with the declaration of '" + alias +
                   "' in this file; import it under another name, e.g. 'import { " + name + " as " +
                   name + "2 } from \"" + decl.path + "\"'");
      this->checker.diags_.note(own->second, "'" + alias + "' is declared here");
      return;
    }
    if (here.imports.count(alias) || !here.nodeNames.try_emplace(alias, n).second) {
      this->checker.diags_.error(loc, "duplicate import of '" + alias + "'");
      return;
    }
  };

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
      this->checker.diags_.error(decl.wholeLoc,
                                 form + "; 'process' is global, as in Node, so the import is "
                                        "not needed: drop it");
    } else if (*which == NodeModule::Buffer) {
      this->checker.diags_.error(decl.wholeLoc,
                                 form +
                                     "; 'Buffer' is global, as in Node: import it as "
                                     "'import { Buffer } from \"" +
                                     decl.path + "\"', or drop the import");
    } else if (!names.empty()) {
      this->checker.diags_.error(decl.wholeLoc,
                                 form + "; import the names this file uses: 'import { " + names +
                                     " } from \"" + decl.path + "\"'");
    } else {
      this->checker.diags_.error(decl.wholeLoc,
                                 form + "; import the names you need, e.g. 'import { " +
                                     BuiltinModuleChecker::typicalName(*which).str() +
                                     " } from \"" + decl.path + "\"'");
    }
    bind(decl.whole, decl.wholeLoc, Checker::NodeName{*which, {}},
         BuiltinModuleChecker::typicalName(*which));
  }

  for (const ModuleName &name : decl.names) {
    std::string problem = this->exportProblem(*which, name.name, decl.path);
    if (!problem.empty()) {
      this->checker.diags_.error(name.loc, problem);
      here.badImports.insert(name.as);
      continue;
    }
    Checker::NodeName n{*which, name.name};
    if (BuiltinModuleChecker::isPathModule(*which) &&
        (name.name == "posix" || name.name == "win32")) {
      n = Checker::NodeName{name.name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32,
                            {}};
    }
    if (*which == NodeModule::Fs && name.name == "promises") {
      n = Checker::NodeName{NodeModule::FsPromises, {}};
    }
    bind(name.as, name.asLoc, n, name.name);
  }
}

const Checker::NodeName *BuiltinModuleChecker::nodeName(llvm::StringRef name) const {
  if (!this->checker.module_ ||
      this->checker.module_->order >= this->checker.moduleStates_.size()) {
    return nullptr;
  }
  const llvm::StringMap<Checker::NodeName> &names =
      this->checker.moduleStates_[this->checker.module_->order].nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  if (found->second.module == NodeModule::Buffer && name == "Buffer") {
    return nullptr;
  }
  ScopeEntry entry = this->checker.scopes().lookup(name);
  if (entry.found()) {
    return nullptr;
  }

  return &found->second;
}

Expr *BuiltinModuleChecker::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const Checker::NodeName *n = name ? this->nodeName(name->name) : nullptr;
  if (n && n->module == NodeModule::Buffer && n->name == "Buffer") {
    return this->checker.ctx_.make<NameExpr>(name->loc(), "Buffer");
  }
  if (!n || n->module != NodeModule::Process) {
    return expr;
  }
  if (n->name.empty()) {
    return this->checker.ctx_.make<NameExpr>(name->loc(), "process");
  }

  return BuiltinModuleChecker::processMember(this->checker.ctx_, name->loc(), n->name);
}

Expr *BuiltinModuleChecker::nodeValue(const Checker::NodeName &n, Expr *expr) {
  expr->type = Type::error();
  std::string label = moduleLabel(n.module);
  if (n.name.empty()) {
    this->checker.diags_.error(
        expr->loc(), "this is not a value: the functions of '" + label +
                         "' are used by their names, e.g. '" +
                         (BuiltinModuleChecker::isPathModule(n.module)
                              ? "posix.join(a, b)'"
                              : BuiltinModuleChecker::typicalName(n.module).str() + "(...)'"));
    return expr;
  }
  for (const Feature *feature : this->checker.features_->all()) {
    if (Expr *value = feature->checkModuleValue(this->checker, n.module, n.name, expr)) {
      return value;
    }
  }
  this->checker.diags_.error(expr->loc(),
                             "'" + n.name + "' of '" + label +
                                 "' can only be called; where a function is expected, pass an "
                                 "arrow that calls it, e.g. 'x => " +
                                 n.name + "(x)'");

  return expr;
}

Expr *BuiltinModuleChecker::checkNodeName(NameExpr *name) {
  const Checker::NodeName *n = this->nodeName(name->name);
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    return this->checker.exprChecker().checkExpr(this->processAlias(name));
  }

  return this->nodeValue(*n, name);
}

Expr *BuiltinModuleChecker::checkNodeMember(MemberExpr *member) {
  for (const Feature *feature : this->checker.features_->all()) {
    if (Expr *checked = feature->checkModuleMember(this->checker, member)) {
      return checked;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const Checker::NodeName *n = object ? this->nodeName(object->name) : nullptr;
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    member->object = this->processAlias(object);
    return nullptr;
  }
  if (!n->name.empty()) {
    return nullptr;
  }
  std::string problem = this->exportProblem(n->module, member->name, moduleLabel(n->module));
  if (!problem.empty()) {
    this->checker.diags_.error(member->loc(), problem);
    member->type = Type::error();
    return member;
  }
  Checker::NodeName inner{n->module, member->name};
  if (BuiltinModuleChecker::isPathModule(n->module) &&
      (member->name == "posix" || member->name == "win32")) {
    inner = {member->name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
  }
  if (n->module == NodeModule::Fs && member->name == "promises") {
    inner = {NodeModule::FsPromises, {}};
  }

  return this->nodeValue(inner, member);
}

Expr *BuiltinModuleChecker::checkNodeCall(CallExpr *call, const Type *expected) {
  if (call->optional) {
    return nullptr;
  }
  Checker::NodeName target{NodeModule::Fs, {}};
  llvm::SMLoc loc;
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    const Checker::NodeName *n = this->nodeName(name->name);
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      call->callee = this->processAlias(name);
      return nullptr;
    }
    if (n->name.empty()) {
      this->checker.diags_.error(name->loc(), "'" + name->name +
                                                  "' is not a function: call the functions of '" +
                                                  moduleLabel(n->module) + "' by their names");
      for (Expr *&arg : call->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = *n;
    loc = name->loc();
  } else if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !member->optional) {
    auto *object = dyn_cast<NameExpr>(member->object);
    const Checker::NodeName *n = object ? this->nodeName(object->name) : nullptr;
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      member->object = this->processAlias(object);
      return nullptr;
    }
    if (n->module == NodeModule::TimersPromises && n->name == "scheduler") {
      TimerChecker::ResultSchedulerAwait awaited =
          this->checker.timerChecker().checkSchedulerCall(call, member);
      bool reported =
          awaited.isErr() &&
          awaited.error().code == TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED;
      if (awaited.isErr() && !reported) {
        this->checker.diags_.error(member->loc(), awaited.error().message);
      }
      return awaited.isOk() ? awaited.value() : call;
    }
    if (!n->name.empty()) {
      return nullptr;
    }
    std::string problem = this->exportProblem(n->module, member->name, moduleLabel(n->module));
    if (!problem.empty()) {
      this->checker.diags_.error(member->loc(), problem);
      for (Expr *&arg : call->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = {n->module, member->name};
    loc = member->loc();
  } else {
    return nullptr;
  }

  for (const Feature *feature : this->checker.features_->all()) {
    if (Expr *checked = feature->checkModuleCall(this->checker, call, target.module, target.name,
                                                 loc, expected)) {
      return checked;
    }
  }

  return nullptr;
}

Expr *BuiltinModuleChecker::checkTimersPromises(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                                const Type *expected) {
  TimerChecker::ResultPromiseOfTimer promised =
      this->checker.timerChecker().checkTimersPromisesCall(call, fn, loc, expected);
  if (promised.isOk()) {
    return promised.value();
  }
  switch (promised.error().code) {
    case TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED:
      break;
    case TimerCheckerCheckTimersPromisesCallErrorCode::VOID_VALUE:
      this->checker.diags_.error(call->args[fn == "setImmediate" ? 0 : 1]->loc(),
                                 promised.error().message);
      break;
    case TimerCheckerCheckTimersPromisesCallErrorCode::SCHEDULER_ITSELF:
    case TimerCheckerCheckTimersPromisesCallErrorCode::WRONG_ARITY:
      this->checker.diags_.error(loc, promised.error().message);
      break;
  }

  return call;
}

}
