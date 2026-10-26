#include "compiler/load/load_pass/load_pass.h"
#include "compiler/load/loader/loader.h"
#include "compiler/program/program/program.h"

namespace tz {

LoadPass::LoadPass(llvm::StringRef path, const pkg::Graph *packages)
    : path_(path.str()), packages_(packages) {}

Errors LoadPass::run(Program &program) const {
  program.diags = std::make_unique<Diagnostics>(program.sourceMgr);
  AstContext &ast = *program.asts.emplace_back(std::make_unique<AstContext>());
  program.file = SourceFile();
  loadProgram(path_, program.sourceMgr, ast, *program.diags, program.file, packages_);

  return program.diags->take();
}

}
