#pragma once

#include "compiler/program/ast/ast_context/ast_context.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/program_inventory/program_inventory.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/symbols/completer/completer.h"
#include "compiler/semantic/semantic_state/v2/checkpoint/checkpoint.h"
#include "compiler/semantic/semantic_state/v2/code/code_storage.h"
#include "compiler/semantic/semantic_state/v2/declaration/declaration_storage.h"
#include "compiler/semantic/semantic_state/v2/flow/flow_storage.h"
#include "compiler/semantic/semantic_state/v2/generics/generic_storage.h"
#include "compiler/semantic/semantic_state/v2/inference/inference_storage.h"
#include "compiler/semantic/semantic_state/v2/library/library_storage.h"
#include "compiler/semantic/semantic_state/v2/types/type_storage.h"
#include "compiler/semantic/semantic_state/v2/walk/walk_storage.h"

namespace tz {

class SemanticState {
public:
  SemanticState(AstContext &context, Diagnostics &collected, SourceFile &loaded);

  void restoreBody(Checkpoint saved);


  Checkpoint saveBody();

  AstContext &ast;
  CodeStorage code;
  FlowStorage flow;
  SourceFile &file;
  WalkStorage walk;
  TypeStorage types;
  LibraryStorage library;
  GenericStorage generics;
  Diagnostics &diagnostics;
  InferenceStorage inference;
  ProgramInventory &inventory;
  Completer *completer = nullptr;
  DeclarationStorage declaration;
};

}
