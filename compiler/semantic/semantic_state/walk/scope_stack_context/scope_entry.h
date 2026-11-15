#pragma once

#include "compiler/union/union/union.h"
#include "llvm/Support/SMLoc.h"

namespace tz {

struct ClassDecl;
struct EnumDecl;
class FuncSymbol;
struct VarSymbol;

enum class ScopeEntryKind {
  NO_ENTRY,
  VAR_ENTRY,
  FUNCTION_ENTRY,
  ENUM_ENTRY,
  CLASS_ENTRY,
};

struct NoEntry {
  static constexpr ScopeEntryKind kind = ScopeEntryKind::NO_ENTRY;
};

struct VarEntry {
  static constexpr ScopeEntryKind kind = ScopeEntryKind::VAR_ENTRY;
  VarSymbol *var = nullptr;
};

struct FunctionEntry {
  static constexpr ScopeEntryKind kind = ScopeEntryKind::FUNCTION_ENTRY;
  FuncSymbol *func = nullptr;
  VarSymbol *var = nullptr;
};

struct EnumEntry {
  static constexpr ScopeEntryKind kind = ScopeEntryKind::ENUM_ENTRY;
  EnumDecl *enumDecl = nullptr;
};

struct ClassEntry {
  static constexpr ScopeEntryKind kind = ScopeEntryKind::CLASS_ENTRY;
  ClassDecl *cls = nullptr;
};

using ScopeEntryForm = Union<NoEntry, VarEntry, FunctionEntry, EnumEntry, ClassEntry>;

struct ScopeEntry {
  ScopeEntryForm form = NoEntry{};

  bool isFound() const;
  VarSymbol *var() const;
  ClassDecl *cls() const;
  FuncSymbol *func() const;
  llvm::SMLoc loc() const;
  EnumDecl *enumDecl() const;
  llvm::SMLoc functionLoc(const FunctionEntry &function) const;
};

}
