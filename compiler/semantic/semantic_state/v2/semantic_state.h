#pragma once

#include "compiler/program/ast/ast_context/ast_context.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
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


  bool isIterableRef(const TypeRef &ref) const;
  bool isUnshadowedGlobal(const NameExpr *name) const;
  bool isGlobalName(const Expr *expr, GlobalName global) const;
  bool isGlobalMember(const MemberExpr *member, GlobalMember which) const;


  Checkpoint saveBody();
  Stmt *findTypeDeclaration(llvm::StringRef name) const;
  const NodeName *findNodeName(llvm::StringRef name) const;
  std::optional<StartLoadEarly> findEarlyLoad(llvm::StringRef name) const;
  Module *findLaterSource(llvm::StringRef name, llvm::StringRef &declared) const;

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

private:
  bool isNamingGlobal(const Expr *expr, GlobalName global) const;
  bool isLoadable(Module *source, llvm::StringRef name) const;
  bool isGlobalThisMember(const Expr *expr, GlobalName global) const;


  TypeAliasDecl *findModuleExport(const NodeName &nodeName) const;
};

}
