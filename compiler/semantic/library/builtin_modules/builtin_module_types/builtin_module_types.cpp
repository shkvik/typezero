#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleTypes::BuiltinModuleTypes(SemanticState &owner)
    : state(owner) {}

void BuiltinModuleTypes::declare(BuiltinText builtin) {
  llvm::ArrayRef<Stmt *> items = this->state.getLibraryDeclarations().getBuiltinItems(builtin);
  for (Stmt *item : items) {
    TypeAliasDecl *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.getLibraryDeclarations().rememberModuleAlias(alias);
    }
  }
}

void BuiltinModuleTypes::exportFrom(NodeModule module, llvm::StringRef name) {
  this->state.getLibraryDeclarations().addModuleExport(module, name);
}

bool BuiltinModuleTypes::declared(llvm::StringRef name) const {
  return this->state.getLibraryDeclarations().hasModuleAlias(name);
}

TypeAliasDecl *BuiltinModuleTypes::alias(llvm::StringRef name) const {
  return this->state.getLibraryDeclarations().findModuleAlias(name);
}

TypeAliasDecl *BuiltinModuleTypes::exported(NodeModule module, llvm::StringRef name) const {
  if (!this->state.getLibraryDeclarations().isModuleExport(module, name)) {
    return nullptr;
  }

  return this->alias(name);
}

const Type *BuiltinModuleTypes::named(llvm::StringRef name) {
  TypeAliasDecl *found = this->alias(name);
  assert(found && "a built-in module declares the type");

  return TypeDeclarer(this->state).resolveAlias(found);
}

}
