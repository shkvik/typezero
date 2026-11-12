#include "compiler/semantic/semantic_state/library_declarations/library_declarations.h"

namespace tz {

LibraryDeclarations::LibraryDeclarations(const BuiltinDeclarations &loaded)
    : builtins(loaded) {}

llvm::ArrayRef<Stmt *> LibraryDeclarations::getBuiltinItems(BuiltinText text) const {
  return this->builtins.getItems(text);
}

void LibraryDeclarations::rememberLibraryFunction(FunctionDecl *functionDecl) {
  this->libraryFunctions[functionDecl->name] = functionDecl;
}

FunctionDecl *LibraryDeclarations::findLibraryFunction(llvm::StringRef name) const {
  return this->libraryFunctions.lookup(name);
}

FunctionDecl *LibraryDeclarations::findLibraryFunction(LibraryFunction libraryFunction) const {
  auto name = LibraryFunctionTable::getName(libraryFunction);

  return this->libraryFunctions.lookup(name);
}

void LibraryDeclarations::rememberLibraryType(TypeAliasDecl *alias) {
  this->libraryTypes[alias->name] = alias;
}

TypeAliasDecl *LibraryDeclarations::findLibraryType(llvm::StringRef name) const {
  return this->libraryTypes.lookup(name);
}

TypeAliasDecl *LibraryDeclarations::findLibraryType(LibraryType libraryType) const {
  auto name = LibraryTypeTable::getName(libraryType);

  return this->libraryTypes.lookup(name);
}

void LibraryDeclarations::rememberLibraryClass(ClassDecl *classDecl) {
  this->libraryClasses[classDecl->name] = classDecl;
}

ClassDecl *LibraryDeclarations::findLibraryClass(llvm::StringRef name) const {
  return this->libraryClasses.lookup(name);
}

ClassDecl *LibraryDeclarations::findLibraryClass(LibraryClass libraryClass) const {
  auto name = LibraryClassTable::getName(libraryClass);

  return this->libraryClasses.lookup(name);
}

void LibraryDeclarations::addLibraryRange(const char *begin, const char *end) {
  this->libraryRanges.push_back({begin, end});
}

bool LibraryDeclarations::isInsideLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();
  for (const SourceRange &range : this->libraryRanges) {
    if (at >= range.begin && at < range.end) {
      return true;
    }
  }

  return false;
}

void LibraryDeclarations::rememberModuleAlias(TypeAliasDecl *alias) {
  this->moduleAliases[alias->name] = alias;
}

TypeAliasDecl *LibraryDeclarations::findModuleAlias(llvm::StringRef name) const {
  return this->moduleAliases.lookup(name);
}

void LibraryDeclarations::addModuleExport(NodeModule module, llvm::StringRef name) {
  this->moduleExports.insert({module, name.str()});
}

bool LibraryDeclarations::isModuleExport(NodeModule module, llvm::StringRef name) const {
  return this->moduleExports.count({module, name.str()}) != 0;
}

}
