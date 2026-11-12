#include "compiler/semantic/names/scopes/scope_entry.h"
#include "compiler/program/ast/class_decl/class_decl.h"
#include "compiler/program/ast/enum_decl/enum_decl.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

bool ScopeEntry::isFound() const {
  return !this->form.is<NoEntry>();
}

VarSymbol *ScopeEntry::var() const {
  switch (this->form.kind()) {
    case ScopeEntryKind::NO_ENTRY:
    case ScopeEntryKind::ENUM_ENTRY:
    case ScopeEntryKind::CLASS_ENTRY:
      return nullptr;
    case ScopeEntryKind::VAR_ENTRY:
      return this->form.as<VarEntry>()->var;
    case ScopeEntryKind::FUNCTION_ENTRY:
      return this->form.as<FunctionEntry>()->var;
  }

  return nullptr;
}

FuncSymbol *ScopeEntry::func() const {
  const FunctionEntry *function = this->form.as<FunctionEntry>();
  if (function == nullptr) {
    return nullptr;
  }

  return function->func;
}

EnumDecl *ScopeEntry::enumDecl() const {
  const EnumEntry *enumEntry = this->form.as<EnumEntry>();
  if (enumEntry == nullptr) {
    return nullptr;
  }

  return enumEntry->enumDecl;
}

ClassDecl *ScopeEntry::cls() const {
  const ClassEntry *classEntry = this->form.as<ClassEntry>();
  if (classEntry == nullptr) {
    return nullptr;
  }

  return classEntry->cls;
}

llvm::SMLoc ScopeEntry::loc() const {
  switch (this->form.kind()) {
    case ScopeEntryKind::NO_ENTRY:
      return llvm::SMLoc();
    case ScopeEntryKind::VAR_ENTRY:
      return this->form.as<VarEntry>()->var->loc;
    case ScopeEntryKind::FUNCTION_ENTRY:
      return this->functionLoc(*this->form.as<FunctionEntry>());
    case ScopeEntryKind::ENUM_ENTRY:
      return this->form.as<EnumEntry>()->enumDecl->loc();
    case ScopeEntryKind::CLASS_ENTRY:
      return this->form.as<ClassEntry>()->cls->loc();
  }

  return llvm::SMLoc();
}

llvm::SMLoc ScopeEntry::functionLoc(const FunctionEntry &function) const {
  if (function.var != nullptr) {
    return function.var->loc;
  }

  return function.func->loc;
}

}
