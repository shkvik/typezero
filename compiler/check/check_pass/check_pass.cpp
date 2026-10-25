#include "compiler/check/check_pass/check_pass.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/declare/function_names/function_names.h"
#include "compiler/declare/import_binder/import_binder.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/program/program/program.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

Errors CheckPass::run(Program &program) const {
  Checker &checker = *program.checker;
  SourceFile &file = program.file;
  checker.metadataChecker().findMetadataWriters(file);

  // A global no function mentions cannot change during a call, and can live in
  // main's stack frame. That includes the arrows of top-level code.
  for (Stmt *item : file.items) {
    collectNames(item, checker.namesInFunctions_, /*inFunction=*/isa<FunctionDecl>(item));
  }
  // `crypto.subtle` reads the SubtleCrypto of tz:webcrypto by a name no code writes, in
  // functions too (sema_webcrypto.cpp).
  checker.namesInFunctions_.insert(kSubtleName);
  checker.importBinder().noteImportsInFunctions(file);

  // Top-level code runs in order, file by file in the order they load; its
  // `let` and `const` become globals. A file sees the variables of the files
  // that load before it.
  checker.flow_ = Checker::Flow();
  for (Module *module : file.modules) {
    checker.topLevelChecker().checkTopLevel(module);
  }
  checker.topLevelDone_ = true;
  for (Module *module : file.modules) {
    checker.moduleDeclarer().useModule(module);
    checker.importBinder().bindImports();
  }

  // Bodies come last so that functions can use globals declared after them,
  // except those whose return type top-level code needed first. Nothing is
  // known about the globals when a function starts.
  for (Module *module : file.modules) {
    checker.moduleDeclarer().useModule(module);
    for (Stmt *item : module->file.items) {
      // A class whose base is a value is checked with those made late.
      if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->type) {
        if (!cls->baseExpr) {
          checker.classChecker().checkClassBodies(cls);
        }
        continue;
      }
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        // The body of a template is checked in its copies only.
        if (checker.lateClasses().isTemplate(fn->sym)) {
          continue;
        }
        if (!fn->sym->ret) {
          checker.returnInferrer().inferReturnType(fn->sym);
        }
        if (fn->checked) {
          continue;
        }
        checker.flow_ = Checker::Flow();
        checker.functionChecker().checkFunctionBody(fn);
      }
    }
  }
  for (FunctionDecl *fn : checker.lifted_) {
    if (checker.lateClasses().isTemplate(fn->sym)) {
      continue;
    }
    if (!fn->sym->ret) {
      checker.returnInferrer().inferReturnType(fn->sym);
    }
    if (!fn->checked) {
      checker.flow_ = Checker::Flow();
      checker.functionChecker().checkFunctionBody(fn);
    }
  }

  return program.diags->take();
}

} // namespace tz
