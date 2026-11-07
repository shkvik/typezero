#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include <cstdint>
#include <optional>

namespace tz {

struct ByteCallback {
  unsigned param = 0;
  int64_t lo = 0, hi = 0;
  const Type *target = nullptr;
};

struct ElementCallback {
  unsigned param = 0;
  int64_t lo = 0;
  int64_t hi = 0;
};

class CallbackFacts {
public:
  void rememberByteCallback(const FunctionDecl *fn, const ByteCallback &callback);
  const ByteCallback *findByteCallback(const FunctionDecl *fn) const;
  void rememberIndexCallback(const FunctionDecl *fn, unsigned param);
  std::optional<unsigned> findIndexParam(const FunctionDecl *fn) const;
  void rememberElementCallback(const FunctionDecl *fn, const ElementCallback &callback);
  const ElementCallback *findElementCallback(const FunctionDecl *fn) const;

private:
  llvm::DenseMap<const FunctionDecl *, ByteCallback> byteCallbacks;
  llvm::DenseMap<const FunctionDecl *, unsigned> indexCallbacks;
  llvm::DenseMap<const FunctionDecl *, ElementCallback> elementCallbacks;
};

}
