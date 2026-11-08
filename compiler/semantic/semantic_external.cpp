#include "compiler/semantic/semantic_external.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/declarations/declaration_checker/declaration_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker_errors.h"
#include "compiler/semantic/declare/assigned_fields/assigned_fields.h"
#include "compiler/semantic/declare/import_binder/import_binder.h"
#include "compiler/semantic/declare/metadata_writers/metadata_writers.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/declare/names_in_functions/names_in_functions.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/feature/feature.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/infer/constructor_calls/constructor_calls.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/infer/return_completer/return_completer.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/events/emitter_checker/emitter_checker.h"
#include "compiler/semantic/library/feature_registry/feature_registry.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

using llvm::dyn_cast;

namespace tz {

SemanticExternalFacade::SemanticExternalFacade(AstContext &ast, Diagnostics &diagnostics,
                                               SourceFile &loaded, EventInference &inference)
    : file(loaded),
      features(std::make_unique<FeatureRegistry>()),
      state(std::make_unique<SemanticState>(ast, diagnostics, loaded, this->features->all(),
                                            inference)),
      completer(std::make_unique<ReturnCompleter>(*this->state)) {
  this->state->rememberCompleter(this->completer.get());
}

SemanticExternalFacade::~SemanticExternalFacade() = default;

void SemanticExternalFacade::finishProgram() {
  SemanticState &engine = *this->state;
  ReflectChecker(engine).finishReflection();
  EmitterChecker(engine).checkErrorListeners();
  EmitterChecker(engine).checkDroppedListeners();
  for (Module *module : this->file.modules) {
    ModuleDeclarer(engine).useModule(module);
    if (engine.getExpressionMarks().hasUntyped()) {
      VarChecker(engine).reportUntyped();
    }
  }
  Scopes(engine).closeScope();
  const Type *native = FsChecker(engine).nativeError();
  const Type *nativeError = native->isError() ? nullptr : native;
  engine.getInventoryStore().rememberNativeError(nativeError);
  ClassLinks(engine).numberClasses();
  for (ClassDecl *cls : engine.getClassStore().getClasses()) {
    bool emittedLate = (cls->generic() || (cls->expression() && cls->type)) && cls->isEmitted();
    if (emittedLate) {
      engine.getInventoryStore().addLateClass(cls);
    }
  }
}

Errors SemanticExternalFacade::inferProgram() {
  SemanticState &engine = *this->state;
  auto cycleCallee = [&engine](FunctionDecl *fn, llvm::StringRef name) {
    return ImportBinder(engine).cycleCallee(fn, name);
  };
  std::vector<FunctionDecl *> declared = engine.getReturnInference().getDeclaredFunctions();
  CallCycles(engine).findCycles(declared, cycleCallee);

  return engine.getDiagnostics().take();
}

Errors SemanticExternalFacade::declareProgram() {
  SemanticState &engine = *this->state;
  engine.getScopeStack().openScope();
  for (const Feature *feature : engine.getFeatures()) {
    feature->prepare(engine, this->file);
  }
  for (const Feature *feature : engine.getFeatures()) {
    feature->declare(engine);
  }
  Prelude(engine).declarePrelude();
  AssignedFields(engine).rememberAssigned(this->file);
  std::vector<FunctionDecl *> declared = ModuleDeclarer(engine).declareModules(this->file);
  engine.getReturnInference().rememberDeclaredFunctions(std::move(declared));
  MetadataWriters(engine).rememberWriters(this->file);
  NamesInFunctions(engine).rememberNames(this->file);

  return engine.getDiagnostics().take();
}

Errors SemanticExternalFacade::checkInstances() {
  SemanticState &engine = *this->state;
  do {
    LateClasses(engine).checkLateClasses();
    GenericInstantiator(engine).checkInstances();
  } while (engine.getLateClassStore().hasUncheckedLateClasses());
  ConstructorCalls(engine).checkConstructorCalls();
  VarChecker(engine).reportUncheckedResults();

  return engine.getDiagnostics().take();
}

Diagnostics &SemanticExternalFacade::getDiagnostics() {
  return this->state->getDiagnostics();
}

SemanticExternalFacade::ResultBodiesChecked SemanticExternalFacade::walkBodies() {
  SemanticState &engine = *this->state;
  engine.getFlowState().replaceFlow(Flow());
  for (Module *module : this->file.modules) {
    TopLevelChecker(engine).loadTopLevel(module);
  }
  engine.getModuleStates().markTopLevelDone();
  for (Module *module : this->file.modules) {
    ModuleDeclarer(engine).useModule(module);
    ImportBinder(engine).bindImports();
  }

  for (Module *module : this->file.modules) {
    ModuleDeclarer(engine).useModule(module);
    for (Stmt *item : module->file.items) {
      ClassDecl *cls = dyn_cast<ClassDecl>(item);
      if (cls != nullptr) {
        this->checkClass(cls);
      }
      FunctionDecl *fn = dyn_cast<FunctionDecl>(item);
      if (fn != nullptr) {
        this->checkFunction(fn);
      }
    }
  }

  for (FunctionDecl *fn : engine.getReturnInference().getLiftedFunctions()) {
    this->checkLiftedFunction(fn);
  }
  DeclarationChecker declarations(engine);
  for (Module *module : this->file.modules) {
    if (module->declaration != nullptr) {
      declarations.walkDeclarationFile(module);
    }
  }

  Errors taken = engine.getDiagnostics().take();
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

ProgramInventory SemanticExternalFacade::takeInventory() {
  return this->state->getInventoryStore().takeInventory();
}

bool SemanticExternalFacade::isInsideLibrary(llvm::SMLoc loc) const {
  return this->state->getLibraryDeclarations().isInsideLibrary(loc);
}

const WideningStore &SemanticExternalFacade::getWideningStore() const {
  return this->state->getWideningStore();
}

std::string SemanticExternalFacade::quotedName(llvm::StringRef name) {
  return ErrorTextFunctionName::quoted(name);
}

const Type *SemanticExternalFacade::awaitedType(const Type *type) {
  return AsyncChecker::awaitedType(type);
}

bool SemanticExternalFacade::copiesIntoRecord(const Type *from, const Type *to) {
  return TypeRelations::copiesIntoRecord(from, to);
}

std::string SemanticExternalFacade::loadChain(const Module *reader, const Module *source) {
  return TopLevelChecker::loadChain(reader, source);
}

std::string SemanticExternalFacade::notLoadedText(llvm::StringRef reader, llvm::StringRef what,
                                                  llvm::StringRef source, llvm::StringRef chain,
                                                  llvm::StringRef declared) {
  return ErrorTextNotLoaded::whileLoads(reader, what, source, chain, declared);
}

bool SemanticExternalFacade::isImplicitlyConvertible(const Type *from, const Type *to) {
  return TypeRelations::isImplicitlyConvertible(from, to);
}

std::optional<DecimalLiteral> SemanticExternalFacade::decimalLiteral(const Expr *expr) {
  return DecimalChecker::decimalLiteral(expr);
}

std::vector<NarrowedPart> SemanticExternalFacade::narrowingsOf(const Type *from, const Type *to) {
  return WideningRelations::narrowingsOf(from, to);
}

void SemanticExternalFacade::checkClass(ClassDecl *cls) {
  if (cls->type == nullptr || cls->baseExpr != nullptr) {
    return;
  }
  ClassChecker(*this->state).checkClassBodies(cls);
}

void SemanticExternalFacade::checkFunction(FunctionDecl *fn) {
  SemanticState &engine = *this->state;
  if (LateClasses(engine).isTemplate(fn->sym)) {
    return;
  }
  if (!fn->sym->hasReturnType()) {
    ReturnInferrer(engine).inferReturnType(fn->sym);
  }
  if (fn->checked) {
    return;
  }
  engine.getFlowState().replaceFlow(Flow());
  FunctionChecker(engine).checkFunctionBody(fn);
}

void SemanticExternalFacade::checkLiftedFunction(FunctionDecl *fn) {
  SemanticState &engine = *this->state;
  if (LateClasses(engine).isTemplate(fn->sym)) {
    return;
  }
  Module *home = engine.getModuleStates().findDeclModule(fn);
  if (home != nullptr) {
    ModuleDeclarer(engine).useModule(home);
    ImportBinder(engine).bindImports();
  }
  this->checkFunction(fn);
}

}
