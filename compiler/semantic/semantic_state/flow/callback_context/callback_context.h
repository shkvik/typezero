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

class CallbackContext {
public:
  void rememberIndexCallback(const FunctionDecl *functionDecl, unsigned param);
  void rememberByteCallback(const FunctionDecl *functionDecl, const ByteCallback &callback);
  void rememberElementCallback(const FunctionDecl *functionDecl, const ElementCallback &callback);


  const ByteCallback *findByteCallback(const FunctionDecl *functionDecl) const;
  std::optional<unsigned> findIndexParam(const FunctionDecl *functionDecl) const;
  const ElementCallback *findElementCallback(const FunctionDecl *functionDecl) const;

private:
  llvm::DenseMap<const FunctionDecl *, unsigned> indexCallbacks;
  llvm::DenseMap<const FunctionDecl *, ByteCallback> byteCallbacks;
  llvm::DenseMap<const FunctionDecl *, ElementCallback> elementCallbacks;
};

}
