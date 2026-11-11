#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/type_context/type_context.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

struct DeclaratorPlace {
  VarDeclStmt *decl = nullptr;
  VarDeclarator *declarator = nullptr;
  size_t number = 0;
  const Type *declared = nullptr;
  llvm::ArrayRef<Stmt *> block;
  size_t blockIndex = 0;
};

struct VariableValue {
  const Type *type = nullptr;
  const Type *hint = nullptr;
  Expr *value = nullptr;
  VarSymbol *early = nullptr;
  bool untyped = false;
  DestinationPlaces places;
};

enum class DeclaratorKind {
  GENERIC_ALIAS,
  GENERIC_FUNCTION,
  CONST_FUNCTION,
  VALUE,
};

struct GenericAliasDeclarator {
  static constexpr DeclaratorKind kind = DeclaratorKind::GENERIC_ALIAS;
  FuncSymbol *generic = nullptr;
};

struct GenericFunctionDeclarator {
  static constexpr DeclaratorKind kind = DeclaratorKind::GENERIC_FUNCTION;
  FunctionDecl *function = nullptr;
};

struct ConstFunctionDeclarator {
  static constexpr DeclaratorKind kind = DeclaratorKind::CONST_FUNCTION;
  FunctionExpr *function = nullptr;
};

struct ValueDeclarator {
  static constexpr DeclaratorKind kind = DeclaratorKind::VALUE;
};

using DeclaratorForm = Union<GenericAliasDeclarator, GenericFunctionDeclarator,
                             ConstFunctionDeclarator, ValueDeclarator>;

}
