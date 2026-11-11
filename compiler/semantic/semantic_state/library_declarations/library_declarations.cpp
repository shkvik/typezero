#include "compiler/semantic/semantic_state/library_declarations/library_declarations.h"

namespace tz {

LibraryDeclarations::LibraryDeclarations(const BuiltinDeclarations &loaded)
    : builtins(loaded) {}

llvm::ArrayRef<Stmt *> LibraryDeclarations::getBuiltinItems(BuiltinText text) const {
  return this->builtins.getItems(text);
}

void LibraryDeclarations::rememberLibraryFunction(FunctionDecl *fn) {
  this->libraryFunctions[fn->name] = fn;
}

FunctionDecl *LibraryDeclarations::findLibraryFunction(llvm::StringRef name) const {
  return this->libraryFunctions.lookup(name);
}

void LibraryDeclarations::rememberLibraryType(TypeAliasDecl *alias) {
  this->libraryTypes[alias->name] = alias;
}

TypeAliasDecl *LibraryDeclarations::findLibraryType(llvm::StringRef name) const {
  return this->libraryTypes.lookup(name);
}

void LibraryDeclarations::rememberLibraryClass(ClassDecl *cls) {
  this->libraryClasses[cls->name] = cls;
}

ClassDecl *LibraryDeclarations::findLibraryClass(llvm::StringRef name) const {
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
