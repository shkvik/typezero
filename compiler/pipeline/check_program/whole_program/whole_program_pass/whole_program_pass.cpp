#include "compiler/pipeline/check_program/whole_program/whole_program_pass/whole_program_pass.h"
#include "compiler/library/async_hooks/context_checker/context_checker.h"
#include "compiler/library/sync/pool_checker/pool_checker.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/declare/prelude/prelude.h"
#include "compiler/pipeline/check_program/scopes/scopes.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/events/emitter_checker/emitter_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker.h"
#include "compiler/pipeline/check_program/topics/stmt/var_checker/var_checker.h"
#include "compiler/pipeline/check_program/whole_program/load_order/load_order.h"
#include "compiler/pipeline/check_program/whole_program/unused/unused.h"
#include "compiler/pipeline/check_program/whole_program/wide_writes/wide_writes.h"
#include "compiler/program/program/program.h"
namespace tz {

WholeProgramPass::WholeProgramPass(CheckState &owner, Program &checked)
    : state(owner),
      program(checked) {}

Errors WholeProgramPass::run() const {
  SourceFile &file = this->program.file;
  LoadOrder(this->state).checkLoadOrder(file);
  Unused(this->state).findUnused(file);
  ReflectChecker(this->state).finishReflection();
  EmitterChecker(this->state).checkErrorListeners();
  EmitterChecker(this->state).checkDroppedListeners();
  for (Module *module : file.modules) {
    ModuleDeclarer(this->state).useModule(module);
    if (this->state.getExpressionMarks().hasUntyped()) {
      VarChecker(this->state).reportUntyped();
    }
  }
  Scopes(this->state).closeScope();
  const Type *native = FsChecker(this->state).nativeError();
  file.nativeError = native->isError() ? nullptr : native;
  ClassLinks(this->state).numberClasses();
  for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
    bool emittedLate = (cls->generic() || (cls->expression() && cls->type)) && cls->isEmitted();
    if (emittedLate) {
      file.items.push_back(cls);
    }
  }
  ContextChecker(this->program).run();
  PoolChecker(this->program).run();
  if (!this->state.getDiagnostics().hasErrors()) {
    WideWrites(this->state).checkWidenings(file);
  }

  return this->program.diags->take();
}

}
