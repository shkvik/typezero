#include "compiler/cli/build/build_command/build_command.h"
#include "compiler/cli/command_line/args/args.h"
#include "compiler/cli/command_line/error_printer/error_printer.h"
#include "compiler/cli/command_line/exit_codes/exit_codes.h"
#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/ir_writer/ir_writer.h"
#include "compiler/link/licenses_pass/licenses_pass.h"
#include "compiler/link/link_pass/link_pass.h"
#include "compiler/lower/library/lowerer_registry/lowerer_registry.h"
#include "compiler/lower/lower_pass/lower_pass.h"
#include "compiler/memory/memory_pass/memory_pass.h"
#include "compiler/optimize/optimize_pass/optimize_pass.h"
#include "compiler/optimize/target/target.h"
#include "compiler/packages/project_packages/project_packages.h"
#include "compiler/parallel/parallel_analysis/parallel_analysis.h"
#include "compiler/parallel/parallel_pass/parallel_pass.h"
#include "compiler/pipeline/build_options/build_options.h"
#include "compiler/pipeline/check_program/check_program.h"
#include "compiler/program/program/program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz {

BuildCommand::BuildCommand(const RuntimeFiles &runtime)
    : runtime(runtime) {}

int BuildCommand::run(const Args &args, llvm::StringRef output) const {
  ProjectPackages packages;
  if (!packages.prepare(args.input)) {
    return kExitError;
  }
  BuildOptions options{args.input,    output.str(),     args.optimize,
                       args.emitLLVM, packages.graph(), this->runtime};
  options.debug = args.debug ? DebugLevel::Full : DebugLevel::Lines;
  Program program;
  ErrorPrinter printer(program.sourceMgr, args.hints);

  ResultCheckedProgram checked = checkProgram(program, options);
  if (checked.isErr()) {
    printer.print(checked.error());
  }
  if (checked.isErr() && Diagnostics::hasErrors(checked.error())) {
    return kExitError;
  }

  llvm::StringRef dir = llvm::sys::path::parent_path(output);
  if (!dir.empty()) {
    if (std::error_code ec = llvm::sys::fs::create_directories(dir)) {
      llvm::errs() << "tz: cannot create the directory " << dir << ": " << ec.message() << "\n";
      return kExitError;
    }
  }
  ResultBuiltProgram built = buildProgram(program, options);
  if (built.isErr()) {
    printer.print(built.error());
    return kExitError;
  }
  printer.print(built.value().warnings);

  return 0;
}

int BuildCommand::explain(const Args &args) const {
  ProjectPackages packages;
  if (!packages.prepare(args.input)) {
    return kExitError;
  }
  BuildOptions options{args.input, "", args.optimize, false, packages.graph(), this->runtime};
  Program program;
  ErrorPrinter printer(program.sourceMgr);

  ResultCheckedProgram checked = checkProgram(program, options);
  if (checked.isErr()) {
    printer.print(checked.error());
  }
  if (checked.isErr() && Diagnostics::hasErrors(checked.error())) {
    return kExitError;
  }
  explainParallel(program.file, program.sourceMgr, program.ast, llvm::outs());

  return 0;
}

BuildCommand::ResultBuiltProgram BuildCommand::buildProgram(Program &program,
                                                            const BuildOptions &options) const {
  auto target = Target::host(options.optimize);
  if (target.isErr()) {
    return err(target.error());
  }

  ParallelPass parallel;
  ParallelPlan plan = parallel.run(program);

  MemoryPass memory;
  MemoryPlan memoryPlan = memory.run(program);

  llvm::LLVMContext context;
  LowererRegistry lowerers;
  LowerPass lower(context, target.value(), llvm::sys::path::filename(options.input), lowerers,
                  options.debug, options.optimize);
  auto lowered = lower.run(program, plan, memoryPlan);
  if (lowered.isErr()) {
    return err(lowered.error());
  }
  llvm::Module &module = *lowered.value().module;

  OptimizePass optimize(target.value(), options);
  std::vector<Diagnostic> optimizeErrors = optimize.run(module, lowered.value().parallel);
  if (!optimizeErrors.empty()) {
    return err(optimizeErrors);
  }

  if (options.emitLLVM) {
    IrWriter writer;
    std::vector<Diagnostic> writeErrors = writer.run(module, options.output);
    if (!writeErrors.empty()) {
      return err(writeErrors);
    }
    return ok(BuiltProgram{});
  }

  LicensesPass licenses;
  CryptoUse crypto = licenses.run(module);

  EmitPass emit(target.value());
  auto object = emit.run(module);
  if (object.isErr()) {
    return err(object.error());
  }

  LinkPass link(target.value(), options);
  auto linked = link.run(object.value(), crypto, lowered.value().parallel);
  if (linked.isErr()) {
    return err(linked.error());
  }

  return ok(BuiltProgram{
      .warnings = linked.value(),
  });
}

}
