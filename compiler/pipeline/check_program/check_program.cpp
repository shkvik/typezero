#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/interprocedural/interprocedural_external.h"
#include "compiler/item_list/item_list/item_list.h"
#include "compiler/load/load_pass/load_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/program/program/program.h"
#include "compiler/semantic/semantic_external.h"

namespace tz {

ResultCheckedProgram checkProgram(Program &program, const BuildOptions &options) {
  LoadPass load(program, options);
  auto loaded = load.run();
  if (loaded.isErr()) {
    return err(loaded.error());
  }

  ItemList<Diagnostic> diagnostics;
  SemanticExternalFacade semantic(program);

  auto declarations = semantic.collectDeclarations();
  if (declarations.isErr()) {
    diagnostics.append(declarations.error());
  }

  auto recursion = semantic.findRecursion();
  if (recursion.isErr()) {
    diagnostics.append(recursion.error());
  }

  auto moduleCode = semantic.checkModuleCode();
  if (moduleCode.isErr()) {
    diagnostics.append(moduleCode.error());
  }

  auto genericCopies = semantic.checkGenericCopies();
  if (genericCopies.isErr()) {
    diagnostics.append(genericCopies.error());
  }

  auto finished = semantic.finishProgram();
  if (finished.isErr()) {
    diagnostics.append(finished.error());
  }

  InterproceduralExternalFacade interprocedural(program);

  auto earlyReads = interprocedural.checkReadsBeforeInit();
  if (earlyReads.isErr()) {
    diagnostics.append(earlyReads.error());
  }

  interprocedural.markUnreachableFunctions();

  auto storages = interprocedural.checkAsyncLocalStorages();
  if (storages.isErr()) {
    diagnostics.append(storages.error());
  }

  auto poolObjects = interprocedural.checkPoolObjects();
  if (poolObjects.isErr()) {
    diagnostics.append(poolObjects.error());
  }

  if (diagnostics.empty()) {
    auto wideWrites = interprocedural.checkWideWrites();
    if (wideWrites.isErr()) {
      diagnostics.append(wideWrites.error());
    }
  }

  ItemList<Diagnostic> warnings;
  if (options.overflow) {
    auto overflows = interprocedural.checkOverflows();
    if (overflows.isErr()) {
      diagnostics.append(overflows.error());
    } else {
      warnings.append(overflows.value());
    }
  }

  if (!diagnostics.empty()) {
    diagnostics.append(warnings);
    return err(diagnostics);
  }

  return ok(CheckedProgram{
      .warnings = std::move(warnings),
  });
}

}
