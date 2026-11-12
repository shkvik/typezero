#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

ChildErrorTypes::ChildErrorTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.builtinModuleTypes;
}

const Type *ChildErrorTypes::errorOf(llvm::StringRef functionName, bool text, bool shell) {
  std::string name = functionName == "execSync"       ? "Exec"
                     : functionName == "execFileSync" ? (shell ? "ExecFileShell" : "ExecFile")
                                                      : (shell ? "SpawnShell" : "Spawn");
  name += text ? "TextError" : "BytesError";

  return this->builtinModuleTypes->named(name);
}

const Type *ChildErrorTypes::spawnResult(bool text) {
  return this->builtinModuleTypes->named(text ? "SpawnTextResult" : "SpawnBytesResult");
}

}
