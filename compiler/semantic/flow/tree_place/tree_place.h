#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class TreePlace {
public:
  bool isNested() const;
  bool isInsideLoop() const;
  bool isInsideSwitch() const;
  bool isInsideFunction() const;
  bool hasLabel(llvm::StringRef label) const;

  TreePlace enterLoop() const;
  TreePlace enterSwitch() const;
  TreePlace enterFunction() const;
  TreePlace enterLabel(llvm::StringRef label) const;

private:
  int loops = 0;
  int switches = 0;
  int functions = 0;
  llvm::SmallVector<llvm::StringRef, 2> labels;
};

}
