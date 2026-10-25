#include "compiler/whole_program/whole_program_pass/whole_program_pass.h"
#include "compiler/check/sema/sema.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/stmt/var_checker/var_checker.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/whole_program/load_order/load_order.h"
#include "compiler/whole_program/unused/unused.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
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

Errors WholeProgramPass::run(Program &program) const {
  Sema &sema = *program.checker;
  SourceFile &file = program.file;
  sema.loadOrder().checkLoadOrder(file);
  sema.unused().findUnused(file);
  sema.reflectChecker().finishReflection();
  // The globals without a type, file by file.
  for (Module *module : file.modules) {
    sema.moduleDeclarer().useModule(module);
    if (!sema.untyped_.empty()) {
      sema.varChecker().reportUntyped();
    }
  }
  sema.popScope();
  // Error and its kinds are classes of the program from here on, for CodeGen,
  // and so are the instances of generic classes, numbered with the others.
  for (ClassDecl *kind : sema.errorKinds_) {
    file.items.push_back(kind);
  }
  file.items.push_back(sema.systemErrorClass_);
  file.items.push_back(sema.execSyncErrorClass_);
  sema.classLinks().numberClasses();
  for (ClassDecl *cls : sema.classes_) {
    if ((cls->generic || (cls->expression && cls->type)) && cls->isEmitted()) {
      file.items.push_back(cls);
    }
  }
  // Writes through a wider type, now that every write is known.
  if (!sema.diags_.hasErrors()) {
    sema.wideWrites().checkWidenings(file);
  }

  return program.diags->take();
}

} // namespace tz
