#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

ChildErrorTypes::ChildErrorTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.builtinModuleTypes;
}

const Type *ChildErrorTypes::errorOf(ChildFunction function, bool isText, bool isShell) {
  std::string name = this->errorBaseName(function, isShell);
  name += isText ? "TextError" : "BytesError";

  return this->builtinModuleTypes->named(name);
}

std::string ChildErrorTypes::errorBaseName(ChildFunction function, bool isShell) const {
  switch (function) {
    case ChildFunction::EXEC_SYNC:
      return "Exec";
    case ChildFunction::EXEC_FILE_SYNC:
      return isShell ? "ExecFileShell" : "ExecFile";
    case ChildFunction::SPAWN_SYNC:
      return isShell ? "SpawnShell" : "Spawn";
  }

  return isShell ? "SpawnShell" : "Spawn";
}

const Type *ChildErrorTypes::spawnResult(bool isText) {
  return this->builtinModuleTypes->named(isText ? "SpawnTextResult" : "SpawnBytesResult");
}

}
