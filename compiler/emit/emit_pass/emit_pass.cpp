#include "compiler/emit/emit_pass/emit_pass.h"
#include "compiler/emit/file_writer/file_writer.h"
#include "compiler/optimize/target/target.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/FileSystem.h"

namespace tz {

EmitPass::EmitPass(const Target &target) : target_(target) {}

Result<ObjectFile, Errors> EmitPass::run(llvm::Module &module) const {
  llvm::SmallString<128> path;
  if (std::error_code ec = llvm::sys::fs::createTemporaryFile("tz", "obj", path)) {
    Errors errors;
    errors.tool("cannot create a temporary file: " + ec.message());
    return err(errors);
  }
  ObjectFile object{std::string(path), std::make_unique<llvm::FileRemover>(path)};
  bool supported = true;
  FileWriter writer;
  Errors errors = writer.write(path, [&](llvm::raw_pwrite_stream &out) {
    llvm::legacy::PassManager pm;
    if (target_.machine().addPassesToEmitFile(pm, out, nullptr,
                                              llvm::CodeGenFileType::ObjectFile)) {
      supported = false;
      return;
    }
    pm.run(module);
  });
  if (!supported) {
    errors.tool("the target cannot emit object files");
  }
  if (errors) {
    return err(errors);
  }

  return ok(std::move(object));
}

} // namespace tz
