#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <utility>
#include <vector>

namespace tz {

class DestinationScan {
public:
  using Place = std::pair<const Type *, llvm::SMLoc>;

  DestinationScan(llvm::StringRef target, const Type *returned);

  void enterScope();
  void leaveScope();
  llvm::StringRef getName() const;
  const Type *getReturns() const;
  void setReturns(const Type *type);
  llvm::ArrayRef<Place> getFound() const;
  bool isHidden(llvm::StringRef other) const;
  void addFound(const Type *type, llvm::SMLoc loc);
  bool isDeclaredInnermost(llvm::StringRef local) const;
  void declare(llvm::StringRef local, const Type *type);
  std::optional<const Type *> findLocal(llvm::StringRef local) const;

private:
  llvm::StringRef name;
  const Type *returns;
  std::vector<Place> found;
  std::vector<llvm::StringMap<const Type *>> locals;
};

}
