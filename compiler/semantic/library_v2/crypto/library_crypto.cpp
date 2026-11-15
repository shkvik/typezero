#include "compiler/semantic/library_v2/crypto/library_crypto.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

LibraryCrypto::LibraryCrypto(SemanticState &owner)
    : state(owner) {}

bool LibraryCrypto::isX509Class(const Expr *object) const {
  const auto *name = dyn_cast<NameExpr>(object);
  const NodeName *nodeName = name != nullptr ? this->state.findNodeName(name->name) : nullptr;

  return nodeName != nullptr && nodeName->module == NodeModule::Crypto &&
         CryptoClassTable::find(nodeName->name) == Type::Crypto::X509;
}

bool LibraryCrypto::isSubtleClass(const Type *type) const {
  auto name = LibraryClassTable::getName(LibraryClass::SUBTLE_CRYPTO);
  const ClassDecl *classDecl = type != nullptr ? type->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() && classDecl->name == name &&
         classDecl == this->state.library.declarations.findLibraryClass(name);
}

const Type *LibraryCrypto::cryptoTypeNamed(llvm::StringRef name) const {
  const NodeName *nodeName = this->state.findNodeName(name);
  if (nodeName == nullptr || nodeName->module != NodeModule::Crypto) {
    return nullptr;
  }
  auto kind = CryptoClassTable::find(nodeName->name);
  if (!kind.has_value()) {
    return nullptr;
  }

  return Type::cryptoType(*kind);
}

}
