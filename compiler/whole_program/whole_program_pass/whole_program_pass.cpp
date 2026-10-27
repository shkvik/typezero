#include "compiler/declare/prelude/prelude.h"
#include "compiler/whole_program/whole_program_pass/whole_program_pass.h"
#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/events/emitter_checker/emitter_checker.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/program/program/program.h"
#include "compiler/whole_program/load_order/load_order.h"
#include "compiler/whole_program/unused/unused.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

Errors WholeProgramPass::run(Program &program) const {
  Checker &checker = *program.checker;
  SourceFile &file = program.file;
  checker.loadOrder().checkLoadOrder(file);
  checker.unused().findUnused(file);
  checker.reflectChecker().finishReflection();
  checker.emitterChecker().checkErrorListeners();
  for (Module *module : file.modules) {
    checker.moduleDeclarer().useModule(module);
    if (!checker.untyped_.empty()) {
      checker.varChecker().reportUntyped();
    }
  }
  checker.popScope();
  for (ClassDecl *kind : checker.errorKinds_) {
    file.items.push_back(kind);
  }
  file.systemError = checker.prelude().builtinType("SystemError");
  checker.classLinks().numberClasses();
  for (ClassDecl *cls : checker.classes_) {
    if ((cls->generic || (cls->expression && cls->type)) && cls->isEmitted()) {
      file.items.push_back(cls);
    }
  }
  if (!checker.diags_.hasErrors()) {
    checker.wideWrites().checkWidenings(file);
  }

  return program.diags->take();
}

}
