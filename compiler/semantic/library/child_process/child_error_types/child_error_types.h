#pragma once

#include "compiler/program/library/child_process/child_function_table/child_function_table.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class BuiltinModuleTypes;

class ChildErrorTypes {
public:
  ChildErrorTypes(SemanticState &owner, SemanticParts &all);

  const Type *errorOf(ChildFunction function, bool isText, bool isShell);
  const Type *spawnResult(bool isText);

private:
  std::string errorBaseName(ChildFunction function, bool isShell) const;

  SemanticState &state;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
};

}
