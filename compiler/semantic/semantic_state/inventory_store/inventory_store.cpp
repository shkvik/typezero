#include "compiler/semantic/semantic_state/inventory_store/inventory_store.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

InventoryStore::InventoryStore(const SourceFile &loaded)
    : file(loaded) {}

void InventoryStore::markUsesTls() {
  this->inventory.usesTls = true;
}

void InventoryStore::markUsesAsync() {
  this->inventory.usesAsync = true;
}

bool InventoryStore::isUsingTls() const {
  return this->inventory.usesTls;
}

void InventoryStore::markTopLevelAwait() {
  this->inventory.topLevelAwait = true;
}

void InventoryStore::markComparesFunctions() {
  this->inventory.comparesFunctions = true;
}

void InventoryStore::addLateClass(ClassDecl *cls) {
  this->inventory.lateClasses.push_back(cls);
}

void InventoryStore::addInstance(FunctionDecl *fn) {
  this->inventory.instances.push_back(fn);
}

void InventoryStore::addSubtype(const Type *type) {
  if (!llvm::is_contained(this->inventory.subtypes, type)) {
    this->inventory.subtypes.push_back(type);
  }
}

ProgramInventory InventoryStore::takeInventory() {
  ProgramInventory taken = std::move(this->inventory);
  this->inventory = ProgramInventory();

  return taken;
}

void InventoryStore::addFieldName(llvm::StringRef name) {
  this->fieldNames.insert(name);
}

std::vector<Module *> InventoryStore::getModules() const {
  return this->file.modules;
}

void InventoryStore::rememberNativeError(const Type *type) {
  this->inventory.nativeError = type;
}

void InventoryStore::addListenerReceiver(const Type *type) {
  this->inventory.listenerReceivers.push_back(type);
}

bool InventoryStore::hasFieldName(llvm::StringRef name) const {
  return this->fieldNames.contains(name) || this->file.fieldNames.contains(name);
}

void InventoryStore::addView(const Type *from, const Type *to) {
  std::pair<const Type *, const Type *> view(from, to);
  if (!llvm::is_contained(this->inventory.views, view)) {
    this->inventory.views.push_back(view);
  }
}

std::vector<const Type *> InventoryStore::getSubtypes() const {
  return this->inventory.subtypes;
}

void InventoryStore::rememberNamedUse(const Expr *use, const NamedUse &named) {
  this->inventory.namedUses[use] = named;
}

std::vector<std::string> InventoryStore::getStringConstants(llvm::StringRef name) const {
  return this->file.stringConstants.lookup(name);
}

}
