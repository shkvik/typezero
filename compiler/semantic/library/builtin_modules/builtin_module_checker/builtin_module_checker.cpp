#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/library/builtin_modules/timers_exports/timers_exports.h"
#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports.h"
#include "compiler/semantic/library/child_process/child_checker/child_checker.h"
#include "compiler/semantic/library/child_process/child_exports/child_exports.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/crypto/crypto_exports/crypto_exports.h"
#include "compiler/semantic/library/crypto/crypto_function_checker/crypto_function_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/fs/fs_exports/fs_exports.h"
#include "compiler/semantic/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/semantic/library/fs/readline_checker/readline_checker.h"
#include "compiler/semantic/library/os/os_checker/os_checker.h"
#include "compiler/semantic/library/os/os_exports/os_exports.h"
#include "compiler/semantic/library/path/path_checker/path_checker.h"
#include "compiler/semantic/library/path/path_exports/path_exports.h"
#include "compiler/semantic/library/process/process_exports/process_exports.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleChecker::BuiltinModuleChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->fsChecker = &all.fsChecker;
  this->osChecker = &all.osChecker;
  this->exprChecker = &all.exprChecker;
  this->pathChecker = &all.pathChecker;
  this->childChecker = &all.childChecker;
  this->timerChecker = &all.timerChecker;
  this->cryptoChecker = &all.cryptoChecker;
  this->readlineChecker = &all.readlineChecker;
  this->fsPromisesChecker = &all.fsPromisesChecker;
  this->builtinModuleTypes = &all.builtinModuleTypes;
  this->cryptoFunctionChecker = &all.cryptoFunctionChecker;
}

llvm::StringRef BuiltinModuleChecker::typicalName(NodeModule module) const {
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

bool BuiltinModuleChecker::isPathModule(NodeModule module) const {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

MemberExpr *BuiltinModuleChecker::processMember(AstContext &context, llvm::SMLoc loc,
                                                llvm::StringRef name) const {
  auto *process = context.make<NameExpr>(loc, "process");

  return context.make<MemberExpr>(loc, process, name);
}

BuiltinModuleChecker::ResultExportFound
BuiltinModuleChecker::checkExportFound(NodeModule module, llvm::StringRef name,
                                       llvm::StringRef path) const {
  if (this->builtinModuleTypes->exported(module, name) != nullptr) {
    return ok(module);
  }
  auto found = this->checkModuleExport(module, name, path);
  if (found.isErr()) {
    std::string message = found.error().message;

    return err(BuiltinModuleCheckerExportFoundError{
        .code = BuiltinModuleCheckerExportFoundErrorCode::NOT_EXPORTED,
        .message = message,
    });
  }

  return ok(module);
}

BuiltinModuleChecker::ResultModuleExport
BuiltinModuleChecker::checkModuleExport(NodeModule module, llvm::StringRef name,
                                        llvm::StringRef path) const {
  switch (module) {
    case NodeModule::Fs:
      return FsExports::checkFsExport(name, path);
    case NodeModule::FsPromises:
      return FsExports::checkPromisesExport(name, path);
    case NodeModule::Readline:
    case NodeModule::ReadlinePromises:
      return FsExports::checkReadlineExport(name, path, module == NodeModule::Readline);
    case NodeModule::TzIo:
      return FsExports::checkTzIoExport(name, path);
    case NodeModule::TzFs:
      return FsExports::checkTzFsExport(name, path);
    case NodeModule::Path:
    case NodeModule::PathPosix:
    case NodeModule::PathWin32:
      return PathExports::checkPathExport(name, path);
    case NodeModule::Os:
      return OsExports::checkOsExport(name, path);
    case NodeModule::Process:
      return ProcessExports::checkProcessExport(name, path);
    case NodeModule::Buffer:
      return BufferExports::checkBufferExport(name, path);
    case NodeModule::TimersPromises:
      return TimersExports::checkTimersExport(name, path);
    case NodeModule::ChildProcess:
      return ChildExports::checkChildExport(name, path);
    case NodeModule::Crypto:
      return CryptoExports::checkCryptoExport(name, path);
    case NodeModule::TzCrypto:
      return CryptoExports::checkTzCryptoExport(name, path);
  }
  llvm_unreachable("unknown node module");
}

Expr *BuiltinModuleChecker::moduleValue(NodeModule module, llvm::StringRef name, Expr *expr) {
  if (module == NodeModule::Crypto) {
    return this->cryptoChecker->checkCryptoValue(name, expr);
  }
  if (this->isPathModule(module)) {
    return this->pathChecker->checkPathValue(module, name, expr);
  }
  if (module == NodeModule::Os) {
    return this->osChecker->checkOsValue(name, expr);
  }
  if (module == NodeModule::Fs || module == NodeModule::FsPromises) {
    return this->fsChecker->checkFsValue(name, expr);
  }

  return nullptr;
}

void BuiltinModuleChecker::importBuiltinModule(Module *module, const ImportDecl &importDecl) {
  auto &here = this->state.getModuleStates().getModuleState(module);
  auto which = KnownModules::moduleOf(importDecl.path);
  if (!which.has_value()) {
    for (const ModuleName &name : importDecl.names) {
      here.badImports.insert(name.as);
    }
    if (!importDecl.whole.empty()) {
      here.badImports.insert(importDecl.whole);
    }
    return;
  }
  if (!importDecl.whole.empty()) {
    this->reportWholeImport(module, importDecl, *which, here);
  }
  for (const ModuleName &name : importDecl.names) {
    this->bindNamedImport(importDecl, name, *which, here);
  }
}

void BuiltinModuleChecker::bindNodeName(ModuleState &here, const ImportDecl &importDecl,
                                        const NodeBinding &binding) {
  auto own = here.declared.find(binding.alias);
  if (own != here.declared.end()) {
    auto message = ErrorTextNodeImport::conflict(binding.alias, binding.name, importDecl.path);
    this->state.getDiagnostics().error(binding.loc, message);
    auto declared = ErrorTextNodeImport::declared(binding.alias);
    this->state.getDiagnostics().note(own->second, declared);
    return;
  }
  bool isTaken = here.imports.count(binding.alias) != 0;
  if (!isTaken) {
    auto [_, isEmplaced] = here.nodeNames.try_emplace(binding.alias, binding.node);
    isTaken = !isEmplaced;
  }
  if (isTaken) {
    auto message = ErrorTextNodeImport::duplicate(binding.alias);
    this->state.getDiagnostics().error(binding.loc, message);
  }
}

void BuiltinModuleChecker::reportWholeImport(Module *module, const ImportDecl &importDecl,
                                             NodeModule which, ModuleState &here) {
  auto used = this->wholeImportUses(module, importDecl.whole);
  auto names = ErrorTextNodeImport::list(used);
  auto form = ErrorTextNodeImport::form(importDecl.wholeIsDefault);
  auto message = this->wholeImportMessage(importDecl, which, names, form);
  this->state.getDiagnostics().error(importDecl.wholeLoc, message);
  NodeBinding binding{
      .alias = importDecl.whole,
      .loc = importDecl.wholeLoc,
      .node = NodeName{which, {}},
      .name = this->typicalName(which),
  };
  this->bindNodeName(here, importDecl, binding);
}

std::vector<llvm::StringRef> BuiltinModuleChecker::wholeImportUses(Module *module,
                                                                   llvm::StringRef whole) const {
  auto used = std::vector<llvm::StringRef>();
  for (Stmt *item : module->file.items) {
    for (const Expr *expr : AstWalker::allExprsIn(item)) {
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
      if (object != nullptr && object->name == whole && !llvm::is_contained(used, member->name)) {
        used.push_back(member->name);
      }
    }
  }

  return used;
}

std::string BuiltinModuleChecker::wholeImportMessage(const ImportDecl &importDecl, NodeModule which,
                                                     const std::string &names,
                                                     const std::string &form) const {
  if (which == NodeModule::Process && importDecl.whole == "process") {
    return ErrorTextNodeImport::process(form);
  }
  if (which == NodeModule::Buffer) {
    return ErrorTextNodeImport::buffer(form, importDecl.path);
  }
  if (!names.empty()) {
    return ErrorTextNodeImport::uses(form, names, importDecl.path);
  }
  auto typical = this->typicalName(which);

  return ErrorTextNodeImport::typical(form, typical, importDecl.path);
}

void BuiltinModuleChecker::bindNamedImport(const ImportDecl &importDecl, const ModuleName &name,
                                           NodeModule which, ModuleState &here) {
  auto found = this->checkExportFound(which, name.name, importDecl.path);
  if (found.isErr()) {
    this->state.getDiagnostics().error(name.loc, found.error().message);
    here.badImports.insert(name.as);
    return;
  }
  NodeBinding binding{
      .alias = name.as,
      .loc = name.asLoc,
      .node = this->importedNode(which, name.name),
      .name = name.name,
  };
  this->bindNodeName(here, importDecl, binding);
}

NodeName BuiltinModuleChecker::importedNode(NodeModule which, llvm::StringRef name) const {
  if (this->isPathModule(which) && (name == "posix" || name == "win32")) {
    return NodeName{name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
  }
  if (which == NodeModule::Fs && name == "promises") {
    return NodeName{NodeModule::FsPromises, {}};
  }

  return NodeName{which, name};
}

const NodeName *BuiltinModuleChecker::nodeName(llvm::StringRef name) const {
  auto &modules = this->state.getModuleStates();
  auto *current = modules.getCurrentModule();
  if (current == nullptr || !modules.hasModuleState(current)) {
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
  auto entry = this->scopes->lookup(name);
  if (entry.isFound()) {
    return nullptr;
  }

  return &found->second;
}

Expr *BuiltinModuleChecker::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const NodeName *nodeName = name != nullptr ? this->nodeName(name->name) : nullptr;
  if (nodeName != nullptr && nodeName->module == NodeModule::Buffer && nodeName->name == "Buffer") {
    return this->state.getAst().make<NameExpr>(name->loc(), "Buffer");
  }
  if (nodeName == nullptr || nodeName->module != NodeModule::Process) {
    return expr;
  }
  if (nodeName->name.empty()) {
    return this->state.getAst().make<NameExpr>(name->loc(), "process");
  }

  return this->processMember(this->state.getAst(), name->loc(), nodeName->name);
}

Expr *BuiltinModuleChecker::nodeValue(const NodeName &nodeName, Expr *expr) {
  expr->type = Type::error();
  auto label = NodeCheckHelpers::moduleLabel(nodeName.module);
  if (nodeName.name.empty()) {
    auto message = ErrorTextNodeValue::notValue(label, this->isPathModule(nodeName.module),
                                                this->typicalName(nodeName.module));
    this->state.getDiagnostics().error(expr->loc(), message);
    return expr;
  }
  auto *value = this->moduleValue(nodeName.module, nodeName.name, expr);
  if (value != nullptr) {
    return value;
  }
  auto message = ErrorTextNodeValue::onlyCalled(nodeName.name, label);
  this->state.getDiagnostics().error(expr->loc(), message);

  return expr;
}

Expr *BuiltinModuleChecker::checkNodeName(NameExpr *name) {
  const auto *nodeName = this->nodeName(name->name);
  if (nodeName == nullptr) {
    return nullptr;
  }
  if (nodeName->module == NodeModule::Process ||
      (nodeName->module == NodeModule::Buffer && !nodeName->name.empty())) {
    return this->exprChecker->checkExpr(this->processAlias(name));
  }

  return this->nodeValue(*nodeName, name);
}

Expr *BuiltinModuleChecker::checkNodeMember(MemberExpr *member) {
  auto *cryptoMember = this->cryptoChecker->checkCryptoModuleMember(member);
  if (cryptoMember != nullptr) {
    return cryptoMember;
  }
  auto *fsConstant = this->fsChecker->checkFsConstant(member);
  if (fsConstant != nullptr) {
    return fsConstant;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName = object != nullptr ? this->nodeName(object->name) : nullptr;
  if (nodeName == nullptr) {
    return nullptr;
  }
  if (nodeName->module == NodeModule::Process ||
      (nodeName->module == NodeModule::Buffer && !nodeName->name.empty())) {
    member->object = this->processAlias(object);
    return nullptr;
  }
  if (!nodeName->name.empty()) {
    return nullptr;
  }
  auto found = this->checkExportFound(nodeName->module, member->name,
                                      NodeCheckHelpers::moduleLabel(nodeName->module));
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    member->type = Type::error();
    return member;
  }
  NodeName inner{nodeName->module, member->name};
  if (this->isPathModule(nodeName->module) &&
      (member->name == "posix" || member->name == "win32")) {
    inner = {member->name == "posix" ? NodeModule::PathPosix : NodeModule::PathWin32, {}};
  }
  if (nodeName->module == NodeModule::Fs && member->name == "promises") {
    inner = {NodeModule::FsPromises, {}};
  }

  return this->nodeValue(inner, member);
}

Expr *BuiltinModuleChecker::checkNodeCall(CallExpr *call, const Type *expected) {
  if (call->optional) {
    return nullptr;
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name != nullptr) {
    return this->checkNodeNameCall(call, name, expected);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || member->optional) {
    return nullptr;
  }

  return this->checkNodeMemberCall(call, member, expected);
}

bool BuiltinModuleChecker::isProcessAlias(const NodeName &nodeName) const {
  return nodeName.module == NodeModule::Process ||
         (nodeName.module == NodeModule::Buffer && !nodeName.name.empty());
}

Expr *BuiltinModuleChecker::failNodeCall(CallExpr *call) {
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  call->type = Type::error();

  return call;
}

Expr *BuiltinModuleChecker::checkNodeNameCall(CallExpr *call, NameExpr *name,
                                              const Type *expected) {
  const auto *nodeName = this->nodeName(name->name);
  if (nodeName == nullptr) {
    return nullptr;
  }
  if (this->isProcessAlias(*nodeName)) {
    call->callee = this->processAlias(name);
    return nullptr;
  }
  if (nodeName->name.empty()) {
    auto message =
        ErrorTextNodeValue::notFunc(name->name, NodeCheckHelpers::moduleLabel(nodeName->module));
    this->state.getDiagnostics().error(name->loc(), message);
    return this->failNodeCall(call);
  }

  return this->dispatchNodeCall(call, *nodeName, name->loc(), expected);
}

Expr *BuiltinModuleChecker::checkNodeMemberCall(CallExpr *call, MemberExpr *member,
                                                const Type *expected) {
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName = object != nullptr ? this->nodeName(object->name) : nullptr;
  if (nodeName == nullptr) {
    return nullptr;
  }
  if (this->isProcessAlias(*nodeName)) {
    member->object = this->processAlias(object);
    return nullptr;
  }
  if (nodeName->module == NodeModule::TimersPromises && nodeName->name == "scheduler") {
    return this->checkSchedulerCall(call, member);
  }
  if (!nodeName->name.empty()) {
    return nullptr;
  }
  auto found = this->checkExportFound(nodeName->module, member->name,
                                      NodeCheckHelpers::moduleLabel(nodeName->module));
  if (found.isErr()) {
    this->state.getDiagnostics().error(member->loc(), found.error().message);
    return this->failNodeCall(call);
  }
  NodeName target{nodeName->module, member->name};

  return this->dispatchNodeCall(call, target, member->loc(), expected);
}

Expr *BuiltinModuleChecker::checkSchedulerCall(CallExpr *call, MemberExpr *member) {
  auto awaited = this->timerChecker->checkSchedulerCall(call, member);
  bool isReported =
      awaited.isErr() &&
      awaited.error().code == TimerCheckerCheckSchedulerCallErrorCode::ALREADY_REPORTED;
  if (awaited.isErr() && !isReported) {
    this->state.getDiagnostics().error(member->loc(), awaited.error().message);
  }

  return awaited.isOk() ? awaited.value() : call;
}

Expr *BuiltinModuleChecker::dispatchNodeCall(CallExpr *call, const NodeName &target,
                                             llvm::SMLoc loc, const Type *expected) {
  NodeModule module = target.module;
  llvm::StringRef functionName = target.name;
  if (module == NodeModule::TimersPromises) {
    return this->checkTimersPromises(call, functionName, loc, expected);
  }
  if (module == NodeModule::Crypto || module == NodeModule::TzCrypto) {
    return this->cryptoFunctionChecker->checkCryptoCall(call, functionName, loc);
  }
  if (this->isPathModule(module)) {
    return this->pathChecker->checkPathCall(call, module, functionName, loc);
  }
  if (module == NodeModule::Os) {
    return this->osChecker->checkOsCall(call, functionName, loc);
  }
  if (module == NodeModule::Fs || (module == NodeModule::TzFs && functionName == "replaceSync")) {
    return this->fsChecker->checkFsModuleCall(call, functionName, loc);
  }
  if (module == NodeModule::FsPromises ||
      (module == NodeModule::TzFs && functionName == "replace")) {
    return this->fsPromisesChecker->checkFsPromisesCall(call, functionName, loc);
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return this->readlineChecker->checkReadlineCall(call, module, functionName, loc);
  }
  if (module == NodeModule::TzIo) {
    return this->fsChecker->checkTzIoCall(call, loc);
  }
  if (module == NodeModule::ChildProcess) {
    return this->childChecker->checkChildCall(call, functionName, loc);
  }

  return nullptr;
}

Expr *BuiltinModuleChecker::checkTimersPromises(CallExpr *call, llvm::StringRef functionName,
                                                llvm::SMLoc loc, const Type *expected) {
  auto promised = this->timerChecker->checkTimersPromisesCall(call, functionName, loc, expected);
  if (promised.isOk()) {
    return promised.value();
  }
  switch (promised.error().code) {
    case TimerCheckerCheckTimersPromisesCallErrorCode::ALREADY_REPORTED:
      break;
    case TimerCheckerCheckTimersPromisesCallErrorCode::VOID_VALUE:
      this->state.getDiagnostics().error(call->args[functionName == "setImmediate" ? 0 : 1]->loc(),
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
