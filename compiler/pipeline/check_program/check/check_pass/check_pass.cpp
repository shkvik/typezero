#include "compiler/pipeline/check_program/check/check_pass/check_pass.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"

using llvm::dyn_cast;

namespace tz {

CheckPass::CheckPass(CheckState &owner, SourceFile &program)
    : state(owner),
      file(program),
      lateClasses(owner),
      classChecker(owner),
      importBinder(owner),
      moduleDeclarer(owner),
      returnInferrer(owner),
      functionChecker(owner),
      topLevelChecker(owner),
      declarationChecker(owner) {}

CheckPass::ResultChecked CheckPass::run() {
  this->state.getFlowState().replaceFlow(Flow());
  for (Module *module : this->file.modules) {
    this->topLevelChecker.loadTopLevel(module);
  }
  this->state.getModuleStates().markTopLevelDone();
  for (Module *module : this->file.modules) {
    this->moduleDeclarer.useModule(module);
    this->importBinder.bindImports();
  }

  for (Module *module : this->file.modules) {
    this->moduleDeclarer.useModule(module);
    for (Stmt *item : module->file.items) {
      auto *cls = dyn_cast<ClassDecl>(item);
      if (cls != nullptr) {
        this->checkClass(cls);
      }
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (fn != nullptr) {
        this->checkFunction(fn);
      }
    }
  }

  for (FunctionDecl *fn : this->state.getReturnInference().getLiftedFunctions()) {
    this->checkLiftedFunction(fn);
  }
  for (Module *module : this->file.modules) {
    if (module->declaration != nullptr) {
      this->declarationChecker.walkDeclarationFile(module);
    }
  }

  Errors taken = this->state.getDiagnostics().take();
  if (taken) {
    return err(taken);
  }
  if (taken.isEmpty()) {
    PassDone done = NoWarnings{};

    return ok(done);
  }
  
  Warnings warnings;
  for (const Diagnostic &diagnostic : taken) {
    warnings.add(diagnostic);
  }
  PassDone done = WithWarnings{.warnings = warnings};

  return ok(done);
}

void CheckPass::checkClass(ClassDecl *cls) {
  if (cls->type == nullptr || cls->baseExpr != nullptr) {
    return;
  }
  this->classChecker.checkClassBodies(cls);
}

void CheckPass::checkFunction(FunctionDecl *fn) {
  if (this->lateClasses.isTemplate(fn->sym)) {
    return;
  }
  if (fn->sym->ret == nullptr) {
    this->returnInferrer.inferReturnType(fn->sym);
  }
  if (fn->checked) {
    return;
  }
  this->state.getFlowState().replaceFlow(Flow());
  this->functionChecker.checkFunctionBody(fn);
}

void CheckPass::checkLiftedFunction(FunctionDecl *fn) {
  if (this->lateClasses.isTemplate(fn->sym)) {
    return;
  }
  Module *home = this->state.getModuleStates().findDeclModule(fn);
  if (home != nullptr) {
    this->moduleDeclarer.useModule(home);
    this->importBinder.bindImports();
  }
  this->checkFunction(fn);
}

}
