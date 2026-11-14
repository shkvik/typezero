#include "compiler/semantic/inference_v2/destinations/destination_search/destination_search.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DestinationSearch::DestinationSearch(llvm::StringRef target, const Type *returned)
    : name(target),
      returns(returned) {}

void DestinationSearch::enterScope() {
  this->locals.emplace_back();
}

void DestinationSearch::leaveScope() {
  this->locals.pop_back();
}

llvm::StringRef DestinationSearch::getName() const {
  return this->name;
}

const Type *DestinationSearch::getReturns() const {
  return this->returns;
}

void DestinationSearch::setReturns(const Type *type) {
  this->returns = type;
}

llvm::ArrayRef<DestinationSearch::Place> DestinationSearch::getFound() const {
  return this->found;
}

bool DestinationSearch::isHidden(llvm::StringRef other) const {
  for (const llvm::StringMap<const Type *> &scope : this->locals) {
    if (scope.contains(other)) {
      return true;
    }
  }

  return false;
}

void DestinationSearch::addFound(const Type *type, llvm::SMLoc loc) {
  this->found.push_back({type, loc});
}

bool DestinationSearch::isDeclaredInnermost(llvm::StringRef local) const {
  return !this->locals.empty() && this->locals.back().contains(local);
}

void DestinationSearch::declare(llvm::StringRef local, const Type *type) {
  if (!this->locals.empty()) {
    this->locals.back()[local] = type;
  }
}

std::optional<const Type *> DestinationSearch::findLocal(llvm::StringRef local) const {
  for (const llvm::StringMap<const Type *> &scope : llvm::reverse(this->locals)) {
    auto entry = scope.find(local);
    if (entry != scope.end()) {
      return entry->second;
    }
  }

  return std::nullopt;
}

}
