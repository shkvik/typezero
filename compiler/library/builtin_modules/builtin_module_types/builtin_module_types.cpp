#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::dyn_cast;

namespace tz {

BuiltinModuleTypes::BuiltinModuleTypes(Checker &checker) : checker_(checker) {}

void BuiltinModuleTypes::declare(llvm::ArrayRef<llvm::StringLiteral> pieces,
                                 llvm::StringRef bufferName) {
  std::string source;
  for (llvm::StringLiteral piece : pieces) {
    source += piece.str();
  }
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBufferCopy(source, bufferName);
  llvm::StringRef text = buffer->getBuffer();
  checker_.diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, checker_.diags_);
  Parser parser(lexer, checker_.ctx_, checker_.diags_);
  SourceFile file = parser.parseSourceFile();
  for (Stmt *item : file.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      aliases_[alias->name] = alias;
    }
  }
}

void BuiltinModuleTypes::exportFrom(NodeModule module, llvm::StringRef name) {
  exports_.insert({module, name.str()});
}

bool BuiltinModuleTypes::declared(llvm::StringRef name) const { return aliases_.count(name) != 0; }

TypeAliasDecl *BuiltinModuleTypes::alias(llvm::StringRef name) const {
  return aliases_.lookup(name);
}

TypeAliasDecl *BuiltinModuleTypes::exported(NodeModule module, llvm::StringRef name) const {
  if (exports_.count({module, name.str()}) == 0) {
    return nullptr;
  }

  return alias(name);
}

const Type *BuiltinModuleTypes::named(llvm::StringRef name) {
  TypeAliasDecl *found = alias(name);
  assert(found && "a built-in module declares the type");

  return checker_.typeDeclarer().resolveAlias(found);
}

}
