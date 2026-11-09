#include "compiler/semantic/semantic_external.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

SemanticExternalFacade::SemanticExternalFacade(AstContext &ast, Diagnostics &diagnostics,
                                               SourceFile &loaded, EventInference &inference)
    : file(loaded),
      state(std::make_unique<SemanticState>(ast, diagnostics, loaded, inference)),
      parts(std::make_unique<SemanticParts>(*this->state)) {
  this->state->rememberCompleter(&this->parts->getReturnCompleter());
}

SemanticExternalFacade::~SemanticExternalFacade() = default;

Errors SemanticExternalFacade::declareProgram() {
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

  return this->takeErrors();
}

Errors SemanticExternalFacade::findCallCycles() {
  this->parts->getCallCycles().findProgramCycles();

  return this->takeErrors();
}

Errors SemanticExternalFacade::walkBodies() {
  this->parts->getTopLevelChecker().loadAllTopLevels(this->file.modules);
  this->parts->getImportBinder().bindAllImports(this->file.modules);
  this->walkModuleItems();
  this->walkLiftedFunctions();
  this->walkDeclarationFiles();

  return this->takeErrors();
}

Errors SemanticExternalFacade::walkInstances() {
  do {
    this->parts->getLateClasses().checkLateClasses();
    this->parts->getGenericInstantiator().checkInstances();
  } while (this->state->getLateClassStore().hasUncheckedLateClasses());
  this->parts->getConstructorCalls().checkConstructorCalls();
  this->parts->getVarChecker().reportUncheckedResults();

  return this->takeErrors();
}

void SemanticExternalFacade::finishProgram() {
  this->parts->getReflectChecker().finishReflection();
  this->parts->getEmitterChecker().checkErrorListeners();
  this->parts->getEmitterChecker().checkDroppedListeners();
  for (Module *module : this->file.modules) {
    this->parts->getModuleDeclarer().useModule(module);
    this->parts->getVarChecker().reportUntyped();
  }
  this->parts->getScopes().closeScope();
}

void SemanticExternalFacade::collectInventory() {
  this->parts->getFsChecker().rememberNativeError();
  this->parts->getClassLinks().numberClasses();
  this->parts->getLateClasses().rememberEmittedLateClasses();
}

ProgramInventory SemanticExternalFacade::takeInventory() {
  return this->state->getInventoryStore().takeInventory();
}

bool SemanticExternalFacade::isInsideLibrary(llvm::SMLoc loc) const {
  return this->state->getLibraryDeclarations().isInsideLibrary(loc);
}

const WideningStore &SemanticExternalFacade::getWideningStore() const {
  return this->state->getWideningStore();
}

Errors SemanticExternalFacade::takeErrors() {
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
