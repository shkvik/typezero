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

class ReflectionContext {
public:
  void markAnyObjectRead();
  void markReflectionUsed();
  void markAnyObjectFilled();
  void addEscapedClass(ClassDecl *classDecl);
  void addReflectedObject(ClassDecl *classDecl);
  void markSeveralVariants(const CallExpr *call);
  void forgetConstructorCast(const CastExpr *castExpr);
  void addConstructorWidening(const Type *from, const Type *to);
  void addMetadataWriter(llvm::StringRef key, FunctionDecl *functionDecl);
  void addReflectedNew(const NewExpr *expr, std::vector<const Type *> args);
  void rememberBuiltinConstructor(llvm::StringRef name, ClassDecl *classDecl);
  void rememberConstructorCast(const CastExpr *castExpr, ClassDecl *classDecl);
  void rememberMetadataType(const void *key, const Type *type, llvm::SMLoc loc);


  bool isAnyObjectRead() const;
  bool hasReflectedNews() const;
  bool isReflectionUsed() const;
  bool isAnyObjectFilled() const;
  bool hasSeveralVariants(const CallExpr *call) const;


  std::vector<ClassDecl *> getEscapedClasses() const;
  std::vector<ClassDecl *> getReflectedObjects() const;


  std::vector<ReflectedNew> getReflectedNews() const;
  std::vector<ConstructorCast> getConstructorCasts() const;
  const MetadataType *findMetadataType(const void *key) const;
  ClassDecl *findBuiltinConstructor(llvm::StringRef name) const;
  std::vector<FunctionDecl *> getMetadataWriters(llvm::StringRef key) const;
  std::vector<const Type *> getConstructorWidenings(const Type *from) const;

private:
  bool anyObjectRead = false;
  bool reflectionUsed = false;
  bool anyObjectFilled = false;
  std::vector<ReflectedNew> reflectedNews;
  llvm::SmallPtrSet<ClassDecl *, 8> escapedClasses;
  llvm::StringMap<ClassDecl *> builtinConstructors;
  llvm::SmallPtrSet<ClassDecl *, 8> reflectedObjects;
  llvm::SmallPtrSet<const CallExpr *, 4> severalVariants;
  llvm::DenseMap<const void *, MetadataType> metadataTypes;
  llvm::StringMap<std::vector<FunctionDecl *>> metadataWriters;
  llvm::DenseMap<const CastExpr *, ClassDecl *> constructorCasts;
  llvm::DenseMap<const Type *, llvm::SmallPtrSet<const Type *, 4>> constructorWidenings;
};

}
