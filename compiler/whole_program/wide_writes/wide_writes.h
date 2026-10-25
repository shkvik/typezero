#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Writes through a wider type, which the whole program shows (docs/objects.md, section 2).
class WideWrites {
public:
  explicit WideWrites(Checker &checker);

  // Writes through a wider type (sema_widen.cpp, docs/objects.md section 2):
  // whether a conversion lets code write what the value's own type does
  // not (`Dog[]` as `Animal[]`, a narrowed or read-only field); the
  // conversions that do, which the check of the whole program follows to
  // the writes; and whether some class or interface narrows a field of one
  // up its chain, which calls of the methods there convert `this` for.
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

} // namespace tz
