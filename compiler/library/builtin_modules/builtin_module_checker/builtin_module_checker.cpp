#include "compiler/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/check/async/timer_checker/timer_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::optional<NodeModule> moduleOf(llvm::StringRef path) {
  if (path == "tz:io") {
    return NodeModule::TzIo;
  }
  if (path == "tz:fs") {
    return NodeModule::TzFs;
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

std::string missingModule(llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (path.starts_with("tz:")) {
    if (path == "tz:string") {
      return "built-in module " + quoted + " is not supported yet";
    }
    return "there is no built-in module " + quoted +
           "; tz has 'tz:io', 'tz:fs', 'tz:http' and 'tz:http2'";
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
  case NodeModule::TzFs:
    return "replaceSync";
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

}

std::string builtinModuleProblem(llvm::StringRef path) {
  return moduleOf(path) ? std::string() : missingModule(path);
}

namespace {

MemberExpr *processMember(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef name) {
  auto *process = ctx.make<NameExpr>(loc, "process");

  return ctx.make<MemberExpr>(loc, process, name);
}

}

BuiltinModuleChecker::BuiltinModuleChecker(Checker &checker) : checker_(checker) {}

std::string BuiltinModuleChecker::exportProblem(NodeModule module, llvm::StringRef name,
                                                llvm::StringRef path) const {
  if (checker_.builtinModuleTypes().exported(module, name)) {
    return "";
  }
  for (const Feature *feature : checker_.features_->all()) {
    if (std::optional<std::string> problem = feature->exportProblem(module, name, path)) {
      return *problem;
    }
  }

  return "";
}

void BuiltinModuleChecker::importBuiltinModule(Module *module, const ImportDecl &decl) {
  Checker::ModuleState &here = checker_.state(module);
  std::optional<NodeModule> which = moduleOf(decl.path);
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
      checker_.diags_.error(loc,
                            "import '" + alias + "' conflicts with the declaration of '" + alias +
                                "' in this file; import it under another name, e.g. 'import { " +
                                name + " as " + name + "2 } from \"" + decl.path + "\"'");
      checker_.diags_.note(own->second, "'" + alias + "' is declared here");
      return;
    }
    if (here.imports.count(alias) || !here.nodeNames.try_emplace(alias, n).second) {
      checker_.diags_.error(loc, "duplicate import of '" + alias + "'");
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
      checker_.diags_.error(decl.wholeLoc,
                            form + "; 'process' is global, as in Node, so the import is "
                                   "not needed: drop it");
    } else if (*which == NodeModule::Buffer) {
      checker_.diags_.error(decl.wholeLoc, form +
                                               "; 'Buffer' is global, as in Node: import it as "
                                               "'import { Buffer } from \"" +
                                               decl.path + "\"', or drop the import");
    } else if (!names.empty()) {
      checker_.diags_.error(decl.wholeLoc, form + "; import the names this file uses: 'import { " +
                                               names + " } from \"" + decl.path + "\"'");
    } else {
      checker_.diags_.error(decl.wholeLoc, form + "; import the names you need, e.g. 'import { " +
                                               typicalName(*which).str() + " } from \"" +
                                               decl.path + "\"'");
    }
    bind(decl.whole, decl.wholeLoc, Checker::NodeName{*which, {}}, typicalName(*which));
  }

  for (const ModuleName &name : decl.names) {
    std::string problem = exportProblem(*which, name.name, decl.path);
    if (!problem.empty()) {
      checker_.diags_.error(name.loc, problem);
      here.badImports.insert(name.as);
      continue;
    }
    Checker::NodeName n{*which, name.name};
    if (isPathModule(*which) && (name.name == "posix" || name.name == "win32")) {
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
  if (!checker_.module_ || checker_.module_->order >= checker_.moduleStates_.size()) {
    return nullptr;
  }
  const llvm::StringMap<Checker::NodeName> &names =
      checker_.moduleStates_[checker_.module_->order].nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  if (found->second.module == NodeModule::Buffer && name == "Buffer") {
    return nullptr;
  }
  Checker::ScopeEntry entry = checker_.scopes().lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls) {
    return nullptr;
  }

  return &found->second;
}

Expr *BuiltinModuleChecker::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const Checker::NodeName *n = name ? nodeName(name->name) : nullptr;
  if (n && n->module == NodeModule::Buffer && n->name == "Buffer") {
    return checker_.ctx_.make<NameExpr>(name->loc(), "Buffer");
  }
  if (!n || n->module != NodeModule::Process) {
    return expr;
  }
  if (n->name.empty()) {
    return checker_.ctx_.make<NameExpr>(name->loc(), "process");
  }

  return processMember(checker_.ctx_, name->loc(), n->name);
}

Expr *BuiltinModuleChecker::nodeValue(const Checker::NodeName &n, Expr *expr) {
  expr->type = Type::error();
  std::string label = moduleLabel(n.module);
  if (n.name.empty()) {
    checker_.diags_.error(expr->loc(),
                          "this is not a value: the functions of '" + label +
                              "' are used by their names, e.g. '" +
                              (isPathModule(n.module) ? "posix.join(a, b)'"
                                                      : typicalName(n.module).str() + "(...)'"));
    return expr;
  }
  for (const Feature *feature : checker_.features_->all()) {
    if (Expr *value = feature->checkModuleValue(checker_, n.module, n.name, expr)) {
      return value;
    }
  }
  checker_.diags_.error(expr->loc(),
                        "'" + n.name + "' of '" + label +
                            "' can only be called; where a function is expected, pass an "
                            "arrow that calls it, e.g. 'x => " +
                            n.name + "(x)'");

  return expr;
}

Expr *BuiltinModuleChecker::checkNodeName(NameExpr *name) {
  const Checker::NodeName *n = nodeName(name->name);
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    return checker_.exprChecker().checkExpr(processAlias(name));
  }

  return nodeValue(*n, name);
}

Expr *BuiltinModuleChecker::checkNodeMember(MemberExpr *member) {
  for (const Feature *feature : checker_.features_->all()) {
    if (Expr *checked = feature->checkModuleMember(checker_, member)) {
      return checked;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const Checker::NodeName *n = object ? nodeName(object->name) : nullptr;
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    member->object = processAlias(object);
    return nullptr;
  }
  if (!n->name.empty()) {
    return nullptr;
  }
  std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
  if (!problem.empty()) {
    checker_.diags_.error(member->loc(), problem);
    member->type = Type::error();
    return member;
  }
  Checker::NodeName inner{n->module, member->name};
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
  Checker::NodeName target{NodeModule::Fs, {}};
  llvm::SMLoc loc;
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    const Checker::NodeName *n = nodeName(name->name);
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      call->callee = processAlias(name);
      return nullptr;
    }
    if (n->name.empty()) {
      checker_.diags_.error(name->loc(), "'" + name->name +
                                             "' is not a function: call the functions of '" +
                                             moduleLabel(n->module) + "' by their names");
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = *n;
    loc = name->loc();
  } else if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !member->optional) {
    auto *object = dyn_cast<NameExpr>(member->object);
    const Checker::NodeName *n = object ? nodeName(object->name) : nullptr;
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      member->object = processAlias(object);
      return nullptr;
    }
    if (n->module == NodeModule::TimersPromises && n->name == "scheduler") {
      return checker_.timerChecker().checkSchedulerCall(call, member);
    }
    if (!n->name.empty()) {
      return nullptr;
    }
    std::string problem = exportProblem(n->module, member->name, moduleLabel(n->module));
    if (!problem.empty()) {
      checker_.diags_.error(member->loc(), problem);
      for (Expr *&arg : call->args) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = {n->module, member->name};
    loc = member->loc();
  } else {
    return nullptr;
  }

  for (const Feature *feature : checker_.features_->all()) {
    if (Expr *checked =
            feature->checkModuleCall(checker_, call, target.module, target.name, loc, expected)) {
      return checked;
    }
  }

  return nullptr;
}

}
