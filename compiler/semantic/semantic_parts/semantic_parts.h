#pragma once

#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/semantic/code/code_registries.h"
#include "compiler/semantic/declaration/declaration_registries.h"
#include "compiler/semantic/flow/flow_registries.h"
#include "compiler/semantic/generics/generic_registries.h"
#include "compiler/semantic/inference/inference_registries.h"
#include "compiler/semantic/library/library_registries.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_registries.h"

namespace tz {

class SemanticParts {
public:
  explicit SemanticParts(SemanticState &owner);

  Diagnostics::ResultTaken walkProgram();

private:
  void runTasks();
  void runTask(const CheckTask &task);

  SemanticState &state;
  CodeRegistries code;
  FlowRegistries flow;
  TypeRegistries types;
  LibraryRegistries library;
  GenericRegistries generics;
  InferenceRegistries inference;
  DeclarationRegistries declaration;
};

}
