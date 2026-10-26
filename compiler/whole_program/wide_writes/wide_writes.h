#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class WideWrites {
public:
  explicit WideWrites(Checker &checker);

  static bool sameReference(const Type *from, const Type *to);
  bool widensWrites(const Type *from, const Type *to);
  void checkWidenings(SourceFile &file);
  void reportWidening(const Type *from, const Type *to, llvm::SMLoc at, llvm::StringRef role,
                      llvm::StringRef name, llvm::SMLoc write, const Type *value,
                      const Type *through, bool element, llvm::StringRef field, const Type *narrow,
                      const Type *owner, int depth);

private:
  Checker &checker_;
  std::map<std::pair<const Type *, const Type *>, bool> widens_;
};

}
