#include "compiler/semantic/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/program/library/builtin_modules/error_export_table/error_export_table.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleTypes::BuiltinModuleTypes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeDeclarer = &all.typeDeclarer;
}

void BuiltinModuleTypes::declareErrorTypes() {
  for (BuiltinText text : ErrorExportTable::getTexts()) {
    this->declare(text);
  }
  for (const ErrorExportInfo &exports : ErrorExportTable::getExports()) {
    std::vector<llvm::StringRef> names = exports.names();
    for (llvm::StringRef name : names) {
      this->exportFrom(exports.module, name);
    }
  }
}

void BuiltinModuleTypes::declare(BuiltinText builtin) {
  auto items = this->state.getLibraryDeclarations().getBuiltinItems(builtin);
  for (Stmt *item : items) {
    auto *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.getLibraryDeclarations().rememberModuleAlias(alias);
    }
  }
}

void BuiltinModuleTypes::exportFrom(NodeModule module, llvm::StringRef name) {
  this->state.getLibraryDeclarations().addModuleExport(module, name);
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
  auto *found = this->alias(name);
  assert(found != nullptr && "a built-in module declares the type");

  return this->typeDeclarer->resolveAlias(found);
}

}
