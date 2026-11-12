#include "compiler/semantic/types/destination_scan/destination_scan.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DestinationScan::DestinationScan(llvm::StringRef target, const Type *returned)
    : name(target),
      returns(returned) {}

void DestinationScan::enterScope() {
  this->locals.emplace_back();
}

void DestinationScan::leaveScope() {
  this->locals.pop_back();
}

llvm::StringRef DestinationScan::getName() const {
  return this->name;
}

const Type *DestinationScan::getReturns() const {
  return this->returns;
}

void DestinationScan::setReturns(const Type *type) {
  this->returns = type;
}

llvm::ArrayRef<DestinationScan::Place> DestinationScan::getFound() const {
  return this->found;
}

bool DestinationScan::isHidden(llvm::StringRef other) const {
  for (const llvm::StringMap<const Type *> &scope : this->locals) {
    if (scope.contains(other)) {
      return true;
    }
  }

  return false;
}

void DestinationScan::addFound(const Type *type, llvm::SMLoc loc) {
  this->found.push_back({type, loc});
}

bool DestinationScan::isDeclaredInnermost(llvm::StringRef local) const {
  return !this->locals.empty() && this->locals.back().contains(local);
}

void DestinationScan::declare(llvm::StringRef local, const Type *type) {
  if (!this->locals.empty()) {
    this->locals.back()[local] = type;
  }
}

std::optional<const Type *> DestinationScan::findLocal(llvm::StringRef local) const {
  for (const llvm::StringMap<const Type *> &scope : llvm::reverse(this->locals)) {
    auto entry = scope.find(local);
    if (entry != scope.end()) {
      return entry->second;
    }
  }

  return std::nullopt;
}

}
