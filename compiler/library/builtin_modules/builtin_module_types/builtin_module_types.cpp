#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleTypes::BuiltinModuleTypes(Checker &owner)
    : checker(owner) {}

void BuiltinModuleTypes::declare(llvm::ArrayRef<llvm::StringLiteral> pieces,
                                 llvm::StringRef bufferName) {
  std::string source;
  for (llvm::StringLiteral piece : pieces) {
    source += piece.str();
  }
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBufferCopy(source, bufferName);
  llvm::StringRef text = buffer->getBuffer();
  this->checker.diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, this->checker.diags_);
  Parser parser(lexer, this->checker.ctx_, this->checker.diags_);
  SourceFile file = parser.parseSourceFile();
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      this->aliases[alias->name] = alias;
    }
  }
}

void BuiltinModuleTypes::exportFrom(NodeModule module, llvm::StringRef name) {
  this->exports.insert({module, name.str()});
}

bool BuiltinModuleTypes::declared(llvm::StringRef name) const {
  return this->aliases.count(name) != 0;
}

TypeAliasDecl *BuiltinModuleTypes::alias(llvm::StringRef name) const {
  return this->aliases.lookup(name);
}

TypeAliasDecl *BuiltinModuleTypes::exported(NodeModule module, llvm::StringRef name) const {
  if (this->exports.count({module, name.str()}) == 0) {
    return nullptr;
  }

  return this->alias(name);
}

const Type *BuiltinModuleTypes::named(llvm::StringRef name) {
  TypeAliasDecl *found = this->alias(name);
  assert(found && "a built-in module declares the type");

  return this->checker.typeDeclarer().resolveAlias(found);
}

}
