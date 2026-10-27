#include "compiler/declare/declare_pass/declare_pass.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/declare/prelude/prelude.h"
#include "compiler/library/child_process/child_checker/child_checker.h"
#include "compiler/library/feature_registry/feature_registry.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
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

DeclarePass::DeclarePass(EventInference &inference, const FeatureRegistry &features)
    : inference_(inference), features_(features) {}

Errors DeclarePass::run(Program &program) const {
  program.checker = std::make_unique<Checker>(*program.asts.back(), *program.diags);
  Checker &checker = *program.checker;
  checker.setEventInference(&inference_);
  checker.features_ = &features_;
  SourceFile &file = program.file;
  checker.file_ = &file;
  if (file.modules.empty()) {
    Module *module = checker.ctx_.newModule();
    module->isEntry = true;
    module->file.items = file.items;
    module->file.imports = std::move(file.imports);
    module->file.exports = std::move(file.exports);
    module->loadPath = {module};
    file.modules.push_back(module);
  }
  checker.pushScope();
  for (const Feature *feature : features_.all()) {
    feature->prepare(checker, file);
  }
  for (const Feature *feature : features_.all()) {
    feature->declare(checker);
  }
  checker.prelude().declarePrelude();

  for (Stmt *item : file.items) {
    visitExprs(item, [&](const Expr *e) {
      const Expr *target = nullptr;
      if (auto *assign = dyn_cast<AssignExpr>(e)) {
        target = assign->target;
      } else if (auto *update = dyn_cast<UpdateExpr>(e)) {
        target = update->target;
      }
      if (auto *member = llvm::dyn_cast_or_null<MemberExpr>(target)) {
        checker.assignedFields_.insert(member->name);
      } else if (auto *index = llvm::dyn_cast_or_null<IndexExpr>(target)) {
        checker.keyedFieldChecker().keyNames(
            index->index, [&](llvm::StringRef name) { checker.assignedFields_.insert(name); });
      }
    });
  }

  checker.declaredFunctions_ = checker.moduleDeclarer().declareModules(file);

  return program.diags->take();
}

}
