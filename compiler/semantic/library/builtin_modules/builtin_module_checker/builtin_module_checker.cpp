#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/feature/feature.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

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

MemberExpr *BuiltinModuleChecker::processMember(AstContext &ctx, llvm::SMLoc loc,
                                                llvm::StringRef name) {
  auto *process = ctx.make<NameExpr>(loc, "process");

  return ctx.make<MemberExpr>(loc, process, name);
}

BuiltinModuleChecker::BuiltinModuleChecker(SemanticState &owner)
    : state(owner) {}

BuiltinModuleChecker::ResultExportFound
BuiltinModuleChecker::exportFound(NodeModule module, llvm::StringRef name,
                                  llvm::StringRef path) const {
  if (BuiltinModuleTypes(this->state).exported(module, name)) {
    return ok(module);
  }
  for (const Feature *feature : this->state.getFeatures()) {
    Feature::ResultExportFound found = feature->exportFound(module, name, path);
    if (found.isOk()) {
      return ok(module);
    }
    if (found.error().code != FeatureExportFoundErrorCode::NOT_MINE) {
      std::string message = found.error().message;

      return err(BuiltinModuleCheckerExportFoundError{
          .code = BuiltinModuleCheckerExportFoundErrorCode::NOT_EXPORTED,
          .message = message,
      });
    }
  }

  return ok(module);
}

void BuiltinModuleChecker::importBuiltinModule(Module *module, const ImportDecl &decl) {
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  std::optional<NodeModule> which = KnownModules::moduleOf(decl.path);
  if (!which) {
    for (const ModuleName &name : decl.names) {
      here.badImports.insert(name.as);
    }
    if (!decl.whole.empty()) {
      here.badImports.insert(decl.whole);
    }
    return;
  }
  auto bind = [&](llvm::StringRef alias, llvm::SMLoc loc, NodeName n, llvm::StringRef name) {
    if (auto own = here.declared.find(alias); own != here.declared.end()) {
      std::string message = ErrorTextNodeImport::conflict(alias, name, decl.path);
      this->state.getDiagnostics().error(loc, message);
      std::string declared = ErrorTextNodeImport::declared(alias);
      this->state.getDiagnostics().note(own->second, declared);
      return;
    }
    if (here.imports.count(alias) || !here.nodeNames.try_emplace(alias, n).second) {
      std::string message = ErrorTextNodeImport::duplicate(alias);
      this->state.getDiagnostics().error(loc, message);
      return;
    }
  };

  if (!decl.whole.empty()) {
    std::vector<llvm::StringRef> used;
    for (Stmt *item : module->file.items) {
      AstWalker::visitExprs(item, [&](const Expr *e) {
        auto *member = dyn_cast<MemberExpr>(e);
        auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
        if (object && object->name == decl.whole && !llvm::is_contained(used, member->name)) {
          used.push_back(member->name);
        }
      });
    }
    std::string names = ErrorTextNodeImport::list(used);
    std::string form = ErrorTextNodeImport::form(decl.wholeIsDefault);
    llvm::StringRef typical = BuiltinModuleChecker::typicalName(*which);
    std::string message = *which == NodeModule::Process && decl.whole == "process"
                              ? ErrorTextNodeImport::process(form)
                          : *which == NodeModule::Buffer
                              ? ErrorTextNodeImport::buffer(form, decl.path)
                          : !names.empty() ? ErrorTextNodeImport::uses(form, names, decl.path)
                                           : ErrorTextNodeImport::typical(form, typical, decl.path);
    this->state.getDiagnostics().error(decl.wholeLoc, message);
    bind(decl.whole, decl.wholeLoc, NodeName{*which, {}},
         BuiltinModuleChecker::typicalName(*which));
  }

  for (const ModuleName &name : decl.names) {
    ResultExportFound found = this->exportFound(*which, name.name, decl.path);
    if (found.isErr()) {
      this->state.getDiagnostics().error(name.loc, found.error().message);
      here.badImports.insert(name.as);
      continue;
    }
    NodeName n{*which, name.name};
    if (BuiltinModuleChecker::isPathModule(*which) &&
        (name.name == "posix" || name.name == "win32")) {
      n = NodeName{name.name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
    }
    if (*which == NodeModule::Fs && name.name == "promises") {
      n = NodeName{NodeModule::FsPromises, {}};
    }
    bind(name.as, name.asLoc, n, name.name);
  }
}

const NodeName *BuiltinModuleChecker::nodeName(llvm::StringRef name) const {
  ModuleStates &modules = this->state.getModuleStates();
  Module *current = modules.getCurrentModule();
  if (!current || !modules.hasModuleState(current)) {
    return nullptr;
  }
  const llvm::StringMap<NodeName> &names = modules.getModuleState(current).nodeNames;
  auto found = names.find(name);
  if (found == names.end()) {
    return nullptr;
  }
  if (found->second.module == NodeModule::Buffer && name == "Buffer") {
    return nullptr;
  }
  ScopeEntry entry = Scopes(this->state).lookup(name);
  if (entry.found()) {
    return nullptr;
  }

  return &found->second;
}

Expr *BuiltinModuleChecker::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const NodeName *n = name ? this->nodeName(name->name) : nullptr;
  if (n && n->module == NodeModule::Buffer && n->name == "Buffer") {
    return this->state.getAst().make<NameExpr>(name->loc(), "Buffer");
  }
  if (!n || n->module != NodeModule::Process) {
    return expr;
  }
  if (n->name.empty()) {
    return this->state.getAst().make<NameExpr>(name->loc(), "process");
  }

  return BuiltinModuleChecker::processMember(this->state.getAst(), name->loc(), n->name);
}

Expr *BuiltinModuleChecker::nodeValue(const NodeName &n, Expr *expr) {
  expr->type = Type::error();
  std::string label = NodeCheckHelpers::moduleLabel(n.module);
  if (n.name.empty()) {
    std::string message =
        ErrorTextNodeValue::notValue(label, BuiltinModuleChecker::isPathModule(n.module),
                                     BuiltinModuleChecker::typicalName(n.module));
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }
  for (const Feature *feature : this->state.getFeatures()) {
    if (Expr *value = feature->checkModuleValue(this->state, n.module, n.name, expr)) {
      return value;
    }
  }
  std::string message = ErrorTextNodeValue::onlyCalled(n.name, label);
  this->state.getDiagnostics().error(expr->loc(), message);

  return expr;
}

Expr *BuiltinModuleChecker::checkNodeName(NameExpr *name) {
  const NodeName *n = this->nodeName(name->name);
  if (!n) {
    return nullptr;
  }
  if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
    return ExprChecker(this->state).checkExpr(this->processAlias(name));
  }

  return this->nodeValue(*n, name);
}

Expr *BuiltinModuleChecker::checkNodeMember(MemberExpr *member) {
  for (const Feature *feature : this->state.getFeatures()) {
    if (Expr *checked = feature->checkModuleMember(this->state, member)) {
      return checked;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *n = object ? this->nodeName(object->name) : nullptr;
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
  ResultExportFound found =
      this->exportFound(n->module, member->name, NodeCheckHelpers::moduleLabel(n->module));
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    member->type = Type::error();
    return member;
  }
  NodeName inner{n->module, member->name};
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
  NodeName target{NodeModule::Fs, {}};
  llvm::SMLoc loc;
  if (auto *name = dyn_cast<NameExpr>(call->callee)) {
    const NodeName *n = this->nodeName(name->name);
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      call->callee = this->processAlias(name);
      return nullptr;
    }
    if (n->name.empty()) {
      std::string message =
          ErrorTextNodeValue::notFunc(name->name, NodeCheckHelpers::moduleLabel(n->module));
      this->state.getDiagnostics().error(name->loc(), message);
      for (Expr *&arg : call->args) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = *n;
    loc = name->loc();
  } else if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !member->optional) {
    auto *object = dyn_cast<NameExpr>(member->object);
    const NodeName *n = object ? this->nodeName(object->name) : nullptr;
    if (!n) {
      return nullptr;
    }
    if (n->module == NodeModule::Process || (n->module == NodeModule::Buffer && !n->name.empty())) {
      member->object = this->processAlias(object);
      return nullptr;
    }
    if (n->module == NodeModule::TimersPromises && n->name == "scheduler") {
      TimerChecker::ResultSchedulerAwait awaited =
          TimerChecker(this->state).checkSchedulerCall(call, member);
      bool reported =
          awaited.isErr() &&
          awaited.error().code == TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED;
      if (awaited.isErr() && !reported) {
        this->state.getDiagnostics().error(member->loc(), awaited.error().message);
      }
      return awaited.isOk() ? awaited.value() : call;
    }
    if (!n->name.empty()) {
      return nullptr;
    }
    ResultExportFound found =
        this->exportFound(n->module, member->name, NodeCheckHelpers::moduleLabel(n->module));
    if (found.isErr()) {
      this->state.getDiagnostics().error(member->loc(), found.error().message);
      for (Expr *&arg : call->args) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
      call->type = Type::error();
      return call;
    }
    target = {n->module, member->name};
    loc = member->loc();
  } else {
    return nullptr;
  }

  for (const Feature *feature : this->state.getFeatures()) {
    if (Expr *checked = feature->checkModuleCall(this->state, call, target.module, target.name, loc,
                                                 expected)) {
      return checked;
    }
  }

  return nullptr;
}

Expr *BuiltinModuleChecker::checkTimersPromises(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                                const Type *expected) {
  TimerChecker::ResultPromiseOfTimer promised =
      TimerChecker(this->state).checkTimersPromisesCall(call, fn, loc, expected);
  if (promised.isOk()) {
    return promised.value();
  }
  switch (promised.error().code) {
    case TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED:
      break;
    case TimerCheckerCheckTimersPromisesCallErrorCode::VOID_VALUE:
      this->state.getDiagnostics().error(call->args[fn == "setImmediate" ? 0 : 1]->loc(),
                                         promised.error().message);
      break;
    case TimerCheckerCheckTimersPromisesCallErrorCode::SCHEDULER_ITSELF:
    case TimerCheckerCheckTimersPromisesCallErrorCode::WRONG_ARITY:
      this->state.getDiagnostics().error(loc, promised.error().message);
      break;
  }

  return call;
}

}
