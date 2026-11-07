#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SMLoc.h"
#include <vector>

namespace tz {

struct MetadataType {
  const Type *type = nullptr;
  llvm::SMLoc loc;
};

struct ReflectedNew {
  const NewExpr *expr = nullptr;
  std::vector<const Type *> args;
};

struct ConstructorCast {
  const CastExpr *cast = nullptr;
  ClassDecl *cls = nullptr;
};

class ReflectStore {
public:
  void markReflectionUsed();
  bool isReflectionUsed() const;
  void markAnyObjectRead();
  bool isAnyObjectRead() const;
  void markAnyObjectFilled();
  bool isAnyObjectFilled() const;
  void rememberMetadataType(const void *key, const Type *type, llvm::SMLoc loc);
  const MetadataType *findMetadataType(const void *key) const;
  void addMetadataWriter(llvm::StringRef key, FunctionDecl *fn);
  std::vector<FunctionDecl *> getMetadataWriters(llvm::StringRef key) const;
  void rememberBuiltinConstructor(llvm::StringRef name, ClassDecl *cls);
  ClassDecl *findBuiltinConstructor(llvm::StringRef name) const;
  void addReflectedNew(const NewExpr *expr, std::vector<const Type *> args);
  bool hasReflectedNews() const;
  std::vector<ReflectedNew> getReflectedNews() const;
  void rememberConstructorCast(const CastExpr *cast, ClassDecl *cls);
  void forgetConstructorCast(const CastExpr *cast);
  std::vector<ConstructorCast> getConstructorCasts() const;
  void addConstructorWidening(const Type *from, const Type *to);
  std::vector<const Type *> getConstructorWidenings(const Type *from) const;
  void addEscapedClass(ClassDecl *cls);
  std::vector<ClassDecl *> getEscapedClasses() const;
  void addReflectedObject(ClassDecl *cls);
  std::vector<ClassDecl *> getReflectedObjects() const;

private:
  bool reflectionUsed = false;
  bool anyObjectRead = false;
  bool anyObjectFilled = false;
  llvm::DenseMap<const void *, MetadataType> metadataTypes;
  llvm::StringMap<std::vector<FunctionDecl *>> metadataWriters;
  llvm::StringMap<ClassDecl *> builtinConstructors;
  std::vector<ReflectedNew> reflectedNews;
  llvm::DenseMap<const CastExpr *, ClassDecl *> constructorCasts;
  llvm::DenseMap<const Type *, llvm::SmallPtrSet<const Type *, 4>> constructorWidenings;
  llvm::SmallPtrSet<ClassDecl *, 8> escapedClasses;
  llvm::SmallPtrSet<ClassDecl *, 8> reflectedObjects;
};

}
