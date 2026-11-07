#pragma once

#include "compiler/pipeline/check_program/check_state/alias_store/alias_store.h"
#include "compiler/pipeline/check_program/check_state/block_position/block_position.h"
#include "compiler/pipeline/check_program/check_state/bytes_store/bytes_store.h"
#include "compiler/pipeline/check_program/check_state/callback_facts/callback_facts.h"
#include "compiler/pipeline/check_program/check_state/class_context/class_context.h"
#include "compiler/pipeline/check_program/check_state/class_store/class_store.h"
#include "compiler/pipeline/check_program/check_state/constant_store/constant_store.h"
#include "compiler/pipeline/check_program/check_state/event_store/event_store.h"
#include "compiler/pipeline/check_program/check_state/expression_marks/expression_marks.h"
#include "compiler/pipeline/check_program/check_state/file_system_store/file_system_store.h"
#include "compiler/pipeline/check_program/check_state/flow_state/flow_state.h"
#include "compiler/pipeline/check_program/check_state/function_stack/function_stack.h"
#include "compiler/pipeline/check_program/check_state/generic_store/generic_store.h"
#include "compiler/pipeline/check_program/check_state/late_class_store/late_class_store.h"
#include "compiler/pipeline/check_program/check_state/library_declarations/library_declarations.h"
#include "compiler/pipeline/check_program/check_state/module_states/module_states.h"
#include "compiler/pipeline/check_program/check_state/predicate_store/predicate_store.h"
#include "compiler/pipeline/check_program/check_state/program_use/program_use.h"
#include "compiler/pipeline/check_program/check_state/reflect_store/reflect_store.h"
#include "compiler/pipeline/check_program/check_state/return_inference/return_inference.h"
#include "compiler/pipeline/check_program/check_state/scope_stack/scope_stack.h"
#include "compiler/pipeline/check_program/check_state/stream_store/stream_store.h"
#include "compiler/pipeline/check_program/check_state/type_context/type_context.h"
#include "compiler/pipeline/check_program/check_state/widening_store/widening_store.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/program/types/types.h"

namespace tz {

class FeatureRegistry;

class CheckState {
public:
  CheckState(AstContext &ctx, Diagnostics &diags, SourceFile &file, FeatureRegistry &registry,
             EventInference &inference);

  AstContext &getAst();
  Diagnostics &getDiagnostics();
  FeatureRegistry &getFeatures();
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
  ProgramUse &getProgramUse();
  LibraryDeclarations &getLibraryDeclarations();
  EventStore &getEventStore();
  ReflectStore &getReflectStore();
  StreamStore &getStreamStore();
  FileSystemStore &getFileSystemStore();
  BytesStore &getBytesStore();
  WideningStore &getWideningStore();

private:
  AstContext &ast;
  Diagnostics &diagnostics;
  FeatureRegistry &features;
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
  ProgramUse programUse;
  LibraryDeclarations libraryDeclarations;
  EventStore eventStore;
  ReflectStore reflectStore;
  StreamStore streamStore;
  FileSystemStore fileSystemStore;
  BytesStore bytesStore;
  WideningStore wideningStore;
};

}
