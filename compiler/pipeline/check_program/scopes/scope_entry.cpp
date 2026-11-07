#include "compiler/pipeline/check_program/scopes/scope_entry.h"
#include "compiler/program/ast/class_decl/class_decl.h"
#include "compiler/program/ast/enum_decl/enum_decl.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

bool ScopeEntry::found() const {
  return !this->form.is<NoEntry>();
}

VarSymbol *ScopeEntry::var() const {
  VarSymbol *symbol = nullptr;
  switch (this->form.kind()) {
    case ScopeEntryKind::NO_ENTRY:
    case ScopeEntryKind::ENUM_ENTRY:
    case ScopeEntryKind::CLASS_ENTRY:
      break;
    case ScopeEntryKind::VAR_ENTRY:
      symbol = this->form.as<VarEntry>()->var;
      break;
    case ScopeEntryKind::FUNCTION_ENTRY:
      symbol = this->form.as<FunctionEntry>()->var;
      break;
  }

  return symbol;
}

FuncSymbol *ScopeEntry::func() const {
  const FunctionEntry *function = this->form.as<FunctionEntry>();

  return function ? function->func : nullptr;
}

EnumDecl *ScopeEntry::enumDecl() const {
  const EnumEntry *enumEntry = this->form.as<EnumEntry>();

  return enumEntry ? enumEntry->enumDecl : nullptr;
}

ClassDecl *ScopeEntry::cls() const {
  const ClassEntry *classEntry = this->form.as<ClassEntry>();

  return classEntry ? classEntry->cls : nullptr;
}

llvm::SMLoc ScopeEntry::loc() const {
  llvm::SMLoc at;
  switch (this->form.kind()) {
    case ScopeEntryKind::NO_ENTRY:
      break;
    case ScopeEntryKind::VAR_ENTRY:
      at = this->form.as<VarEntry>()->var->loc;
      break;
    case ScopeEntryKind::FUNCTION_ENTRY: {
      const FunctionEntry *function = this->form.as<FunctionEntry>();
      at = function->var ? function->var->loc : function->func->loc;
      break;
    }
    case ScopeEntryKind::ENUM_ENTRY:
      at = this->form.as<EnumEntry>()->enumDecl->loc();
      break;
    case ScopeEntryKind::CLASS_ENTRY:
      at = this->form.as<ClassEntry>()->cls->loc();
      break;
  }

  return at;
}

}
