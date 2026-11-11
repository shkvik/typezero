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
  this->state->rememberCompleter(&this->parts->returnCompleter);
}

SemanticExternalFacade::~SemanticExternalFacade() = default;

SemanticExternalFacade::ResultProgramChecked SemanticExternalFacade::run() {
  ItemList<Diagnostic> errors;

  auto declarations = this->collectDeclarations();
  if (declarations.isErr()) {
    errors.append(declarations.error());
  }

  auto recursion = this->findRecursion();
  if (recursion.isErr()) {
    errors.append(recursion.error());
  }

  auto moduleCode = this->checkModuleCode();
  if (moduleCode.isErr()) {
    errors.append(moduleCode.error());
  }

  auto genericCopies = this->checkGenericCopies();
  if (genericCopies.isErr()) {
    errors.append(genericCopies.error());
  }

  auto finished = this->finishProgram();
  if (finished.isErr()) {
    errors.append(finished.error());
  }
  if (!errors.empty()) {
    return err(errors);
  }

  return ok();
}

Diagnostics::ResultTaken SemanticExternalFacade::collectDeclarations() {
  this->parts->scopes.openScope();
  this->parts->libraryCalls.prepareLibraries(this->file);
  this->parts->builtinModuleTypes.declareErrorTypes();
  this->parts->fsDeclarer.declareFsClasses();
  this->parts->prelude.declarePrelude();
  this->parts->assignedFields.rememberAssigned(this->file);
  this->parts->moduleDeclarer.declareModules(this->file);
  this->parts->metadataWriters.rememberWriters(this->file);
  this->parts->namesInFunctions.rememberNames(this->file);

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::findRecursion() {
  this->parts->callCycles.findProgramCycles();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::checkModuleCode() {
  this->parts->topLevelChecker.loadAllTopLevels(this->file.modules);
  this->parts->importBinder.bindAllImports(this->file.modules);
  this->walkModuleItems();
  this->walkLiftedFunctions();
  this->walkDeclarationFiles();
  this->parts->emitterChecker.finishSentEmits();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::checkGenericCopies() {
  do {
    this->parts->lateClasses.checkLateClasses();
    this->parts->genericInstantiator.checkInstances();
  } while (this->state->getLateClassStore().hasUncheckedLateClasses());

  this->parts->constructorCalls.checkConstructorCalls();
  this->parts->varChecker.reportUncheckedResults();

  return this->state->getDiagnostics().take();
}

Diagnostics::ResultTaken SemanticExternalFacade::finishProgram() {
  this->parts->reflectChecker.finishReflection();
  this->parts->emitterChecker.checkErrorListeners();
  this->parts->emitterChecker.checkDroppedListeners();

  for (Module *module : this->file.modules) {
    this->parts->moduleDeclarer.useModule(module);
    this->parts->varChecker.reportUntyped();
  }
  this->parts->scopes.closeScope();

  this->parts->fsChecker.rememberNativeError();
  this->parts->classLinks.numberClasses();
  this->parts->lateClasses.rememberEmittedLateClasses();
  this->file.inventory = this->state->getInventoryStore().takeInventory();
  this->file.inventory.widenings = this->state->takeWideningStore();

  std::vector<ClassDecl *> &late = this->file.inventory.lateClasses;
  this->file.items.insert(this->file.items.end(), late.begin(), late.end());

  return this->state->getDiagnostics().take();
}

void SemanticExternalFacade::walkModuleItems() {
  for (Module *module : this->file.modules) {
    this->parts->moduleDeclarer.useModule(module);
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
      this->parts->declarationChecker.walkDeclarationFile(module);
    }
  }
}

void SemanticExternalFacade::walkClass(ClassDecl *cls) {
  if (cls->type == nullptr || cls->baseExpr != nullptr) {
    return;
  }
  this->parts->classChecker.checkClassBodies(cls);
}

void SemanticExternalFacade::walkFunction(FunctionDecl *fn) {
  if (this->parts->lateClasses.isTemplate(fn->sym)) {
    return;
  }
  if (!fn->sym->hasReturnType()) {
    this->parts->returnInferrer.inferReturnType(fn->sym);
  }
  if (fn->checked) {
    return;
  }
  this->parts->functionChecker.walkFunctionFromScratch(fn);
}

void SemanticExternalFacade::walkLiftedFunction(FunctionDecl *fn) {
  if (this->parts->lateClasses.isTemplate(fn->sym)) {
    return;
  }
  this->parts->importBinder.enterHomeModule(fn);
  this->walkFunction(fn);
}

}
