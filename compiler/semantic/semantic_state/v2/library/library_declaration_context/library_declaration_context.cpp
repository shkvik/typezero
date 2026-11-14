#include "compiler/semantic/semantic_state/v2/library/library_declaration_context/library_declaration_context.h"
#include "compiler/program/library/stream/stream_hook_table/stream_hook_table.h"

namespace tz {

LibraryDeclarationContext::LibraryDeclarationContext(const BuiltinDeclarations &loaded)
    : builtins(loaded) {}

llvm::ArrayRef<Stmt *> LibraryDeclarationContext::getBuiltinItems(BuiltinText text) const {
  return this->builtins.getItems(text);
}

void LibraryDeclarationContext::rememberLibraryFunction(FunctionDecl *functionDecl) {
  this->libraryFunctions[functionDecl->name] = functionDecl;
}

FunctionDecl *LibraryDeclarationContext::findLibraryFunction(llvm::StringRef name) const {
  return this->libraryFunctions.lookup(name);
}

FunctionDecl *
LibraryDeclarationContext::findLibraryFunction(LibraryFunction libraryFunction) const {
  auto name = LibraryFunctionTable::getName(libraryFunction);

  return this->libraryFunctions.lookup(name);
}

void LibraryDeclarationContext::rememberLibraryType(TypeAliasDecl *alias) {
  this->libraryTypes[alias->name] = alias;
}

TypeAliasDecl *LibraryDeclarationContext::findLibraryType(llvm::StringRef name) const {
  return this->libraryTypes.lookup(name);
}

TypeAliasDecl *LibraryDeclarationContext::findLibraryType(LibraryType libraryType) const {
  auto name = LibraryTypeTable::getName(libraryType);

  return this->libraryTypes.lookup(name);
}

void LibraryDeclarationContext::rememberLibraryClass(ClassDecl *classDecl) {
  this->libraryClasses[classDecl->name] = classDecl;
}

ClassDecl *LibraryDeclarationContext::findLibraryClass(llvm::StringRef name) const {
  return this->libraryClasses.lookup(name);
}

ClassDecl *LibraryDeclarationContext::findLibraryClass(LibraryClass libraryClass) const {
  auto name = LibraryClassTable::getName(libraryClass);

  return this->libraryClasses.lookup(name);
}

bool LibraryDeclarationContext::isStreamClass(const ClassDecl *classDecl) const {
  auto *stream = this->findLibraryClass(LibraryClass::STREAM);

  return classDecl != nullptr && stream != nullptr && classDecl->derivesFrom(stream);
}

bool LibraryDeclarationContext::isStreamHook(const FunctionDecl *functionDecl) const {
  const ClassDecl *classDecl = functionDecl->cls();
  bool isUserMethod = classDecl != nullptr && !functionDecl->isStatic() && !classDecl->library() &&
                      classDecl->literal() == nullptr && classDecl->parent != nullptr;
  auto hook = StreamHookTable::find(functionDecl->name);

  return isUserMethod && hook.has_value();
}

bool LibraryDeclarationContext::isLinkedStreamHook(const FunctionDecl *functionDecl) const {
  return this->isStreamHook(functionDecl) && this->isStreamClass(functionDecl->cls()->parent);
}

void LibraryDeclarationContext::addLibraryRange(const char *begin, const char *end) {
  this->libraryRanges.push_back({begin, end});
}

bool LibraryDeclarationContext::isInsideLibrary(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();
  for (const LibraryRange &range : this->libraryRanges) {
    if (at >= range.begin && at < range.end) {
      return true;
    }
  }

  return false;
}

void LibraryDeclarationContext::rememberModuleAlias(TypeAliasDecl *alias) {
  this->moduleAliases[alias->name] = alias;
}

TypeAliasDecl *LibraryDeclarationContext::findModuleAlias(llvm::StringRef name) const {
  return this->moduleAliases.lookup(name);
}

void LibraryDeclarationContext::addModuleExport(NodeModule module, llvm::StringRef name) {
  this->moduleExports.insert({module, name.str()});
}

bool LibraryDeclarationContext::isModuleExport(NodeModule module, llvm::StringRef name) const {
  return this->moduleExports.count({module, name.str()}) != 0;
}

}
