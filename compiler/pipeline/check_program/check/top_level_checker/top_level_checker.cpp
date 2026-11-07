#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/stmt_checker/stmt_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TopLevelChecker::TopLevelChecker(CheckState &owner)
    : state(owner) {}

void TopLevelChecker::checkTopLevel(Module *module, llvm::StringRef until) {
  ModuleState &here = this->state.getModuleStates().getModuleState(module);
  Module *outer = this->state.getModuleStates().getCurrentModule();
  ModuleDeclarer(this->state).useModule(module);
  this->state.getModuleStates().enterLoading(module);
  here.busy = true;
  ImportBinder(this->state).bindImports();
  if (here.flow) {
    this->state.getFlowState().replaceFlow(std::move(*here.flow));
    here.flow.reset();
  }
  std::vector<Stmt *> &items = module->file.items;
  while (here.next < items.size()) {
    Stmt *item = items[here.next++];
    if (isa<FunctionDecl>(item)) {
      continue;
    }
    this->state.getBlockPosition().rememberBlockPosition(items, here.next - 1);
    StmtChecker(this->state).checkStmt(item);
    auto *decl = dyn_cast<VarDeclStmt>(item);
    if (!until.empty() && decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &d) { return d.name == until; })) {
      break;
    }
  }
  if (here.next == items.size()) {
    here.loaded = true;
    EmitterChecker(this->state).reportUnlistenedErrors();
  } else {
    here.flow = this->state.getFlowState().getFlow();
  }
  here.busy = false;
  this->state.getModuleStates().leaveLoading();
  here.values = this->state.getScopeStack().getTopValues();
  here.types = this->state.getScopeStack().getTopTypes();
  if (outer) {
    ModuleDeclarer(this->state).useModule(outer);
  }
}

Module *TopLevelChecker::laterSource(llvm::StringRef name, llvm::StringRef &declared) {
  ModuleState &here = this->state.getModuleStates().getModuleState(
      this->state.getModuleStates().getCurrentModule());
  declared = name;
  if (auto found = here.imports.find(name); found != here.imports.end()) {
    declared = found->second.target.name;
    Module *source = found->second.target.module;
    return this->state.getModuleStates().getModuleState(source).loaded ? nullptr : source;
  }
  if (this->state.getModuleStates().getLoadingModule() &&
      this->state.getModuleStates().getCurrentModule() !=
          this->state.getModuleStates().getLoadingModule() &&
      !here.loaded && here.topLevelNames.contains(name)) {
    return this->state.getModuleStates().getCurrentModule();
  }

  return nullptr;
}

ScopeEntry TopLevelChecker::lookupValue(llvm::StringRef name) {
  ScopeEntry entry = Scopes(this->state).lookup(name);
  if (entry.found() || !this->state.getModuleStates().getLoadingModule()) {
    return entry;
  }
  llvm::StringRef declared;
  Module *source = this->laterSource(name, declared);
  if (!source ||
      (!this->state.getFunctionStack().isInsideFunction() &&
       this->state.getModuleStates().getCurrentModule() ==
           this->state.getModuleStates().getLoadingModule()) ||
      !this->loadEarly(source, declared)) {
    return entry;
  }
  if (source == this->state.getModuleStates().getCurrentModule()) {
    for (const auto &value : this->state.getModuleStates()
                                 .getModuleState(this->state.getModuleStates().getCurrentModule())
                                 .values) {
      this->state.getScopeStack().addTopValue(value.getKey(), value.getValue());
    }
  } else {
    ImportBinder(this->state).bindImports();
  }

  return Scopes(this->state).lookup(name);
}

bool TopLevelChecker::loadEarly(Module *source, llvm::StringRef name) {
  ModuleState &there = this->state.getModuleStates().getModuleState(source);
  if (there.loaded || there.busy || !there.topLevelNames.contains(name)) {
    return false;
  }
  BodyState saved = ReturnInferrer(this->state).enterBody(nullptr);
  this->state.getFunctionStack().forgetEarlyFunction();
  this->state.getExpressionMarks().enterModuleLoad();
  this->state.getReturnInference().enterModuleLoad();
  bool muted = this->state.getDiagnostics().setMuted(false);

  this->checkTopLevel(source, name);

  this->state.getDiagnostics().setMuted(muted);
  this->state.getReturnInference().leaveModuleLoad();
  this->state.getExpressionMarks().leaveModuleLoad();
  ReturnInferrer(this->state).leaveBody(saved);

  return true;
}

std::string TopLevelChecker::loadChain(const Module *reader, const Module *source) {
  std::string chain;
  for (const Module *module : reader->loadPath) {
    chain += (chain.empty() ? "" : " -> ") + module->name;
  }
  if (source != reader) {
    bool started = llvm::is_contained(reader->loadPath, source);
    chain += ErrorTextNotLoaded::pending(source->name, started);
  }

  return chain;
}

std::string TopLevelChecker::loadLabel(llvm::StringRef name, llvm::StringRef declared,
                                       const Module *source) const {
  if (declared != name) {
    return ErrorTextNotLoaded::label(name, declared, source->name);
  }
  if (source != this->state.getModuleStates().getLoadingModule()) {
    return ErrorTextNotLoaded::labelOf(name, source->name);
  }

  return ErrorTextNotLoaded::quoted(name);
}

TopLevelChecker::ResultSourceIsLoaded TopLevelChecker::checkLoaded(const NameExpr *name) {
  if (this->state.getModuleStates()
          .getModuleState(this->state.getModuleStates().getCurrentModule())
          .badImports.contains(name->name)) {
    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  llvm::StringRef declared;
  Module *source = this->laterSource(name->name, declared);
  Module *reader = this->state.getModuleStates().getLoadingModule()
                       ? this->state.getModuleStates().getLoadingModule()
                       : this->state.getModuleStates().getCurrentModule();
  std::string what = source ? this->loadLabel(name->name, declared, source) : "";
  bool topLevel = !this->state.getFunctionStack().isInsideFunction() &&
                  this->state.getModuleStates().getCurrentModule() ==
                      this->state.getModuleStates().getLoadingModule();
  if (source && topLevel) {
    std::string chain = TopLevelChecker::loadChain(reader, source);
    std::string message =
        ErrorTextNotLoaded::whileLoads(reader->name, what, source->name, chain, declared);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::READ_WHILE_LOADING,
        .message = message,
    });
  }
  if (source && this->state.getFunctionStack().getEarlyFunction()) {
    std::string message = ErrorTextNotLoaded::beforeCall(
        what, reader->name, this->state.getFunctionStack().getEarlyFunction()->name);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::NO_TYPE_BEFORE_CALL,
        .message = message,
    });
  }
  if (source) {
    std::string message = ErrorTextNotLoaded::inFunction(what, reader->name);

    return err(TopLevelCheckerCheckLoadedError{
        .code = TopLevelCheckerCheckLoadedErrorCode::NO_TYPE_IN_FUNCTION,
        .message = message,
    });
  }

  return ok(name);
}

TopLevelChecker::ResultVarNotImported
TopLevelChecker::checkImportAssign(const NameExpr *name, const VarSymbol *var) const {
  if (this->state.getModuleStates()
          .getModuleState(this->state.getModuleStates().getCurrentModule())
          .importedVars.contains(var)) {
    std::string message = ErrorTextImportAssign::imported(name->name);

    return err(TopLevelCheckerCheckImportAssignError{
        .code = TopLevelCheckerCheckImportAssignErrorCode::IMPORTED_VARIABLE,
        .message = message,
    });
  }

  return ok(var);
}

}
