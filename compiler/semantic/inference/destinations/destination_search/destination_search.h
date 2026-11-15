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

class DestinationSearch {
public:
  using Place = std::pair<const Type *, llvm::SMLoc>;

  DestinationSearch(llvm::StringRef target, const Type *returned);

  void enterScope();
  void leaveScope();
  void setReturns(const Type *type);
  void addFound(const Type *type, llvm::SMLoc loc);
  void declare(llvm::StringRef local, const Type *type);


  bool isHidden(llvm::StringRef other) const;
  bool isDeclaredInnermost(llvm::StringRef local) const;


  const Type *getReturns() const;
  llvm::StringRef getName() const;
  llvm::ArrayRef<Place> getFound() const;
  std::optional<const Type *> findLocal(llvm::StringRef local) const;

private:
  llvm::StringRef name;
  const Type *returns;
  std::vector<Place> found;
  std::vector<llvm::StringMap<const Type *>> locals;
};

}
