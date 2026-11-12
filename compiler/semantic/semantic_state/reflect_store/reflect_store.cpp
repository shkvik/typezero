#include "compiler/semantic/semantic_state/reflect_store/reflect_store.h"

namespace tz {

void ReflectStore::markReflectionUsed() {
  this->reflectionUsed = true;
}

bool ReflectStore::isReflectionUsed() const {
  return this->reflectionUsed;
}

void ReflectStore::markAnyObjectRead() {
  this->anyObjectRead = true;
}

bool ReflectStore::isAnyObjectRead() const {
  return this->anyObjectRead;
}

void ReflectStore::markAnyObjectFilled() {
  this->anyObjectFilled = true;
}

bool ReflectStore::isAnyObjectFilled() const {
  return this->anyObjectFilled;
}

void ReflectStore::rememberMetadataType(const void *key, const Type *type, llvm::SMLoc loc) {
  this->metadataTypes[key] = {type, loc};
}

const MetadataType *ReflectStore::findMetadataType(const void *key) const {
  auto found = this->metadataTypes.find(key);
  if (found == this->metadataTypes.end()) {
    return nullptr;
  }

  return &found->second;
}

void ReflectStore::addMetadataWriter(llvm::StringRef key, FunctionDecl *functionDecl) {
  std::vector<FunctionDecl *> &writers = this->metadataWriters[key];
  if (!llvm::is_contained(writers, functionDecl)) {
    writers.push_back(functionDecl);
  }
}

std::vector<FunctionDecl *> ReflectStore::getMetadataWriters(llvm::StringRef key) const {
  auto found = this->metadataWriters.find(key);
  if (found == this->metadataWriters.end()) {
    return {};
  }

  return found->second;
}

void ReflectStore::rememberBuiltinConstructor(llvm::StringRef name, ClassDecl *classDecl) {
  this->builtinConstructors[name] = classDecl;
}

ClassDecl *ReflectStore::findBuiltinConstructor(llvm::StringRef name) const {
  return this->builtinConstructors.lookup(name);
}

void ReflectStore::addReflectedNew(const NewExpr *expr, std::vector<const Type *> args) {
  this->reflectedNews.push_back({expr, std::move(args)});
}

bool ReflectStore::hasReflectedNews() const {
  return !this->reflectedNews.empty();
}

std::vector<ReflectedNew> ReflectStore::getReflectedNews() const {
  return this->reflectedNews;
}

void ReflectStore::rememberConstructorCast(const CastExpr *castExpr, ClassDecl *classDecl) {
  this->constructorCasts[castExpr] = classDecl;
}

void ReflectStore::forgetConstructorCast(const CastExpr *castExpr) {
  this->constructorCasts.erase(castExpr);
}

std::vector<ConstructorCast> ReflectStore::getConstructorCasts() const {
  auto casts = std::vector<ConstructorCast>();
  for (const auto &[cast, cls] : this->constructorCasts) {
    casts.push_back({cast, cls});
  }

  return casts;
}

void ReflectStore::addConstructorWidening(const Type *from, const Type *to) {
  this->constructorWidenings[from].insert(to);
}

std::vector<const Type *> ReflectStore::getConstructorWidenings(const Type *from) const {
  auto found = this->constructorWidenings.find(from);
  if (found == this->constructorWidenings.end()) {
    return {};
  }

  return {found->second.begin(), found->second.end()};
}

void ReflectStore::addEscapedClass(ClassDecl *classDecl) {
  this->escapedClasses.insert(classDecl);
}

std::vector<ClassDecl *> ReflectStore::getEscapedClasses() const {
  return {this->escapedClasses.begin(), this->escapedClasses.end()};
}

void ReflectStore::addReflectedObject(ClassDecl *classDecl) {
  this->reflectedObjects.insert(classDecl);
}

std::vector<ClassDecl *> ReflectStore::getReflectedObjects() const {
  return {this->reflectedObjects.begin(), this->reflectedObjects.end()};
}

}
