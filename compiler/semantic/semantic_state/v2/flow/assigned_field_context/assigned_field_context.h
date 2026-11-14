#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class AssignedFieldContext {
public:
  void markFieldAssigned(llvm::StringRef name);


  bool isFieldAssigned(llvm::StringRef name) const;

private:
  llvm::StringSet<> assignedFields;
};

}
