#include "compiler/pipeline/check_program/check/check_pass/check_pass.h"
#include "compiler/load/library_modules/library_modules.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/topics/declarations/declaration_checker/declaration_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/metadata_checker/metadata_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CheckPass::CheckPass(CheckState &owner, SourceFile &program)
    : state(owner),
      file(program) {}

Errors CheckPass::run() const {
  MetadataChecker(this->state).findMetadataWriters(this->file);

  llvm::StringSet<> names;
  for (Stmt *item : this->file.items) {
    AstWalker::collectNames(item, names, isa<FunctionDecl>(item));
  }
  for (const auto &name : names) {
    this->state.getScopeStack().markUsedInFunction(name.getKey());
  }
  this->state.getScopeStack().markUsedInFunction(kSubtleName);
  ImportBinder(this->state).noteImportsInFunctions(this->file);

  this->state.getFlowState().replaceFlow(Flow());
  for (Module *module : this->file.modules) {
    TopLevelChecker(this->state).checkTopLevel(module);
  }
  this->state.getModuleStates().markTopLevelDone();
  for (Module *module : this->file.modules) {
    ModuleDeclarer(this->state).useModule(module);
    ImportBinder(this->state).bindImports();
  }

  for (Module *module : this->file.modules) {
    ModuleDeclarer(this->state).useModule(module);
    for (Stmt *item : module->file.items) {
      if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->type) {
        if (!cls->baseExpr) {
          ClassChecker(this->state).checkClassBodies(cls);
        }
        continue;
      }
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        if (LateClasses(this->state).isTemplate(fn->sym)) {
          continue;
        }
        if (!fn->sym->ret) {
          ReturnInferrer(this->state).inferReturnType(fn->sym);
        }
        if (fn->checked) {
          continue;
        }
        this->state.getFlowState().replaceFlow(Flow());
        FunctionChecker(this->state).checkFunctionBody(fn);
      }
    }
  }
  for (FunctionDecl *fn : this->state.getReturnInference().getLiftedFunctions()) {
    if (LateClasses(this->state).isTemplate(fn->sym)) {
      continue;
    }
    if (Module *home = this->state.getModuleStates().findDeclModule(fn)) {
      ModuleDeclarer(this->state).useModule(home);
      ImportBinder(this->state).bindImports();
    }
    if (!fn->sym->ret) {
      ReturnInferrer(this->state).inferReturnType(fn->sym);
    }
    if (!fn->checked) {
      this->state.getFlowState().replaceFlow(Flow());
      FunctionChecker(this->state).checkFunctionBody(fn);
    }
  }
  for (Module *module : this->file.modules) {
    if (module->declaration) {
      DeclarationChecker(this->state).check(module);
    }
  }

  return this->state.getDiagnostics().take();
}

}
