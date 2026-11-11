#include "compiler/semantic/semantic_external.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

SemanticExternalFacade::SemanticExternalFacade(Program &program)
    : file(program.file),
      state(std::make_unique<SemanticState>(program.ast, program.diags, program.file)),
      parts(std::make_unique<SemanticParts>(*this->state)) {
  this->state->rememberCompleter(&this->parts->getReturnCompleter());
}

SemanticExternalFacade::~SemanticExternalFacade() = default;

Diagnostics::ResultTaken SemanticExternalFacade::collectDeclarations() {
  this->parts->getScopes().openScope();
  this->parts->getLibraryCalls().prepareLibraries(this->file);
  this->parts->getCryptoErrors().declare();
  this->parts->getFsDeclarer().declareFsClasses();
  this->parts->getFsErrors().declare();
  this->parts->getChildErrorTypes().declare();
  this->parts->getPrelude().declarePrelude();
  this->parts->getAssignedFields().rememberAssigned(this->file);
  this->parts->getModuleDeclarer().declareModules(this->file);
  this->parts->getMetadataWriters().rememberWriters(this->file);
  this->parts->getNamesInFunctions().rememberNames(this->file);

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::findRecursion() {
  this->parts->getCallCycles().findProgramCycles();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::checkModuleCode() {
  this->parts->getTopLevelChecker().loadAllTopLevels(this->file.modules);
  this->parts->getImportBinder().bindAllImports(this->file.modules);
  this->walkModuleItems();
  this->walkLiftedFunctions();
  this->walkDeclarationFiles();
  this->parts->getEmitterChecker().finishSentEmits();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::checkGenericCopies() {
  do {
    this->parts->getLateClasses().checkLateClasses();
    this->parts->getGenericInstantiator().checkInstances();
  } while (this->state->getLateClassStore().hasUncheckedLateClasses());

  this->parts->getConstructorCalls().checkConstructorCalls();
  this->parts->getVarChecker().reportUncheckedResults();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::finishProgram() {
  this->parts->getReflectChecker().finishReflection();
  this->parts->getEmitterChecker().checkErrorListeners();
  this->parts->getEmitterChecker().checkDroppedListeners();

  for (Module *module : this->file.modules) {
    this->parts->getModuleDeclarer().useModule(module);
    this->parts->getVarChecker().reportUntyped();
  }
  this->parts->getScopes().closeScope();

  this->parts->getFsChecker().rememberNativeError();
  this->parts->getClassLinks().numberClasses();
  this->parts->getLateClasses().rememberEmittedLateClasses();
  this->file.inventory = this->state->getInventoryStore().takeInventory();
  this->file.inventory.widenings = this->state->takeWideningStore();

  std::vector<ClassDecl *> &late = this->file.inventory.lateClasses;
  this->file.items.insert(this->file.items.end(), late.begin(), late.end());

  return this->state->getDiagnostics().take();
}

void SemanticExternalFacade::walkModuleItems() {
  for (Module *module : this->file.modules) {
    this->parts->getModuleDeclarer().useModule(module);
    for (Stmt *item : module->file.items) {
      ClassDecl *cls = dyn_cast<ClassDecl>(item);
      if (cls != nullptr) {
        this->walkClass(cls);
      }
      FunctionDecl *fn = dyn_cast<FunctionDecl>(item);
      if (fn != nullptr) {
        this->walkFunction(fn);
      }
    }
  }
}

void SemanticExternalFacade::walkLiftedFunctions() {
  for (FunctionDecl *fn : this->state->getReturnInference().getLiftedFunctions()) {
    this->walkLiftedFunction(fn);
  }
}

void SemanticExternalFacade::walkDeclarationFiles() {
  for (Module *module : this->file.modules) {
    if (module->declaration != nullptr) {
      this->parts->getDeclarationChecker().walkDeclarationFile(module);
    }
  }
}

void SemanticExternalFacade::walkClass(ClassDecl *cls) {
  if (cls->type == nullptr || cls->baseExpr != nullptr) {
    return;
  }
  this->parts->getClassChecker().checkClassBodies(cls);
}

void SemanticExternalFacade::walkFunction(FunctionDecl *fn) {
  if (this->parts->getLateClasses().isTemplate(fn->sym)) {
    return;
  }
  if (!fn->sym->hasReturnType()) {
    this->parts->getReturnInferrer().inferReturnType(fn->sym);
  }
  if (fn->checked) {
    return;
  }
  this->parts->getFunctionChecker().walkFunctionFromScratch(fn);
}

void SemanticExternalFacade::walkLiftedFunction(FunctionDecl *fn) {
  if (this->parts->getLateClasses().isTemplate(fn->sym)) {
    return;
  }
  this->parts->getImportBinder().enterHomeModule(fn);
  this->walkFunction(fn);
}

}
