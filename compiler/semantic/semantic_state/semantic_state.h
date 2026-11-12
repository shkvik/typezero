#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/symbols/completer/completer.h"
#include "compiler/program/types/types.h"
#include "compiler/program/widening_store/widening_store.h"
#include "compiler/semantic/semantic_state/alias_store/alias_store.h"
#include "compiler/semantic/semantic_state/block_position/block_position.h"
#include "compiler/semantic/semantic_state/bytes_store/bytes_store.h"
#include "compiler/semantic/semantic_state/callback_facts/callback_facts.h"
#include "compiler/semantic/semantic_state/class_context/class_context.h"
#include "compiler/semantic/semantic_state/class_store/class_store.h"
#include "compiler/semantic/semantic_state/constant_store/constant_store.h"
#include "compiler/semantic/semantic_state/event_store/event_store.h"
#include "compiler/semantic/semantic_state/expression_marks/expression_marks.h"
#include "compiler/semantic/semantic_state/file_system_store/file_system_store.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack.h"
#include "compiler/semantic/semantic_state/generic_store/generic_store.h"
#include "compiler/semantic/semantic_state/inventory_store/inventory_store.h"
#include "compiler/semantic/semantic_state/late_class_store/late_class_store.h"
#include "compiler/semantic/semantic_state/library_declarations/library_declarations.h"
#include "compiler/semantic/semantic_state/module_states/module_states.h"
#include "compiler/semantic/semantic_state/predicate_store/predicate_store.h"
#include "compiler/semantic/semantic_state/reflect_store/reflect_store.h"
#include "compiler/semantic/semantic_state/return_inference/return_inference.h"
#include "compiler/semantic/semantic_state/scope_stack/scope_stack.h"
#include "compiler/semantic/semantic_state/stream_store/stream_store.h"
#include "compiler/semantic/semantic_state/type_context/type_context.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class SemanticState {
public:
  SemanticState(AstContext &context, Diagnostics &diags, SourceFile &file);

  AstContext &getAst();
  Diagnostics &getDiagnostics();
  Completer *getCompleter() const;
  void rememberCompleter(Completer *engine);
  ModuleStates &getModuleStates();
  ScopeStack &getScopeStack();
  FunctionStack &getFunctionStack();
  BlockPosition &getBlockPosition();
  FlowState &getFlowState();
  PredicateStore &getPredicateStore();
  ReturnInference &getReturnInference();
  GenericStore &getGenericStore();
  AliasStore &getAliasStore();
  TypeContext &getTypeContext();
  ClassStore &getClassStore();
  ClassContext &getClassContext();
  LateClassStore &getLateClassStore();
  ExpressionMarks &getExpressionMarks();
  ConstantStore &getConstantStore();
  CallbackFacts &getCallbackFacts();
  InventoryStore &getInventoryStore();
  LibraryDeclarations &getLibraryDeclarations();
  EventStore &getEventStore();
  ReflectStore &getReflectStore();
  StreamStore &getStreamStore();
  FileSystemStore &getFileSystemStore();
  BytesStore &getBytesStore();
  WideningStore &getWideningStore();
  WideningStore takeWideningStore();

private:
  AstContext &ast;
  Diagnostics &diagnostics;
  Completer *completer = nullptr;
  ModuleStates moduleStates;
  ScopeStack scopeStack;
  FunctionStack functionStack;
  BlockPosition blockPosition;
  FlowState flowState;
  PredicateStore predicateStore;
  ReturnInference returnInference;
  GenericStore genericStore;
  AliasStore aliasStore;
  TypeContext typeContext;
  ClassStore classStore;
  ClassContext classContext;
  LateClassStore lateClassStore;
  ExpressionMarks expressionMarks;
  ConstantStore constantStore;
  CallbackFacts callbackFacts;
  InventoryStore inventoryStore;
  LibraryDeclarations libraryDeclarations;
  EventStore eventStore;
  ReflectStore reflectStore;
  StreamStore streamStore;
  FileSystemStore fileSystemStore;
  BytesStore bytesStore;
  WideningStore wideningStore;
};

}
