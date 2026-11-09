#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildTypeNames[] = {
    "ExecTextError",          "ExecBytesError",          "ExecFileTextError",
    "ExecFileBytesError",     "SpawnTextError",          "SpawnBytesError",
    "ExecFileShellTextError", "ExecFileShellBytesError", "SpawnShellTextError",
    "SpawnShellBytesError",   "SpawnTextResult",         "SpawnBytesResult"};

}

ChildErrorTypes::ChildErrorTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.getBuiltinModuleTypes();
}

void ChildErrorTypes::declare() {
  BuiltinModuleTypes &types = *this->builtinModuleTypes;
  if (types.declared("ExecTextError")) {
    return;
  }
  types.declare(BuiltinText::CHILD_PROCESS_ERRORS);
  for (llvm::StringLiteral name : kChildTypeNames) {
    types.exportFrom(NodeModule::ChildProcess, name);
  }
}

const Type *ChildErrorTypes::errorOf(llvm::StringRef fn, bool text, bool shell) {
  std::string name = fn == "execSync"       ? "Exec"
                     : fn == "execFileSync" ? (shell ? "ExecFileShell" : "ExecFile")
                                            : (shell ? "SpawnShell" : "Spawn");
  name += text ? "TextError" : "BytesError";

  return this->builtinModuleTypes->named(name);
}

const Type *ChildErrorTypes::spawnResult(bool text) {
  return this->builtinModuleTypes->named(text ? "SpawnTextResult" : "SpawnBytesResult");
}

}
