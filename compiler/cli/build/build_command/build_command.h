#pragma once

#include "compiler/native/native_pass/native_link.h"
#include "compiler/pipeline/runtime_files/runtime_files.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz {

class Args;
struct BuildOptions;
struct Program;

class BuildCommand {
public:
  explicit BuildCommand(const RuntimeFiles &runtime);

  int run(const Args &args, llvm::StringRef output,
          std::vector<std::string> *dllDirs = nullptr) const;
  int explain(const Args &args) const;

private:
  struct BuiltProgram {
    std::vector<Diagnostic> warnings;
    std::vector<std::string> dlls;
  };

  using ResultBuiltProgram = Result<BuiltProgram, std::vector<Diagnostic>>;

  ResultBuiltProgram buildProgram(Program &program, const BuildOptions &options) const;
  std::vector<ForeignFunction> foreignFunctions(const Program &program) const;
  bool copyDlls(llvm::ArrayRef<std::string> dlls, llvm::StringRef output) const;

  const RuntimeFiles &runtime;
};

}
