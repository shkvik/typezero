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
  auto semanticChecked = semantic.run();
  if (semanticChecked.isErr()) {
    diagnostics.append(semanticChecked.error());
  }

  InterproceduralExternalFacade interprocedural(program, options.overflow);
  auto interproceduralChecked = interprocedural.run();
  if (interproceduralChecked.isErr()) {
    diagnostics.append(interproceduralChecked.error());
  }
  if (!diagnostics.empty()) {
    return err(diagnostics);
  }

  return ok();
}

}
