#include "compiler/pipeline/check_program/topics/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleTypes::BuiltinModuleTypes(CheckState &owner)
    : state(owner) {}

void BuiltinModuleTypes::declare(llvm::ArrayRef<llvm::StringLiteral> pieces,
                                 llvm::StringRef bufferName) {
  std::string source;
  for (llvm::StringLiteral piece : pieces) {
    source += piece.str();
  }
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBufferCopy(source, bufferName);
  llvm::StringRef text = buffer->getBuffer();
  this->state.getDiagnostics().sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, this->state.getDiagnostics());
  Parser parser(lexer, this->state.getAst(), this->state.getDiagnostics());
  SourceFile file = parser.parseSourceFile();
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
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
