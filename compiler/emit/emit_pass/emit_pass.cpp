#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/file_writer/file_writer.h"
#include "compiler/optimize/target/target.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/FileSystem.h"

namespace tz {

EmitPass::EmitPass(const Target &target)
    : target(target) {}

Result<ObjectFile, std::vector<Diagnostic>> EmitPass::run(llvm::Module &module) const {
  llvm::SmallString<128> path;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile("tz", "obj", path)) {
    std::string message = "cannot create a temporary file: " + ec.message();
    std::vector<Diagnostic> errors = {{.form = Diagnostic::Form::Tool, .message = message}};
    return err(errors);
  }
  ObjectFile object{std::string(path), std::make_unique<llvm::FileRemover>(path)};
  bool supported = true;
  FileWriter writer;
  std::vector<Diagnostic> errors = writer.write(path, [&](llvm::raw_pwrite_stream &out) {
    llvm::legacy::PassManager pm;
    if (this->target.machine().addPassesToEmitFile(pm, out, nullptr,
                                                   llvm::CodeGenFileType::ObjectFile)) {
      supported = false;
      return;
    }
    pm.run(module);
  });
  if (!supported) {
    errors.push_back(
        {.form = Diagnostic::Form::Tool, .message = "the target cannot emit object files"});
  }
  if (!errors.empty()) {
    return err(errors);
  }

  return ok(std::move(object));
}

}
