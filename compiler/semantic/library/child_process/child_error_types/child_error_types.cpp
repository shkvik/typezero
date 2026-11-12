#include "compiler/semantic/library/child_process/child_error_types/child_error_types.h"
#include "compiler/program/library/child_process/child_error_names/child_error_names.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

ChildErrorTypes::ChildErrorTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->builtinModuleTypes = &all.builtinModuleTypes;
}

const Type *ChildErrorTypes::errorOf(ChildFunction function, bool isText, bool isShell) {
  auto name = ChildErrorNames::getErrorName(function, isText, isShell);

  return this->builtinModuleTypes->named(name);
}

const Type *ChildErrorTypes::spawnResult(bool isText) {
  auto name = ChildErrorNames::getResultName(isText);

  return this->builtinModuleTypes->named(name);
}

}
