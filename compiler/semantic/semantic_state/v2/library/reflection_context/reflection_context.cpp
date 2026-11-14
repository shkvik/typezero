#include "compiler/semantic/semantic_state/v2/library/reflection_context/reflection_context.h"

namespace tz {

void ReflectionContext::markReflectionUsed() {
  this->reflectionUsed = true;
}

bool ReflectionContext::isReflectionUsed() const {
  return this->reflectionUsed;
}

void ReflectionContext::markAnyObjectRead() {
  this->anyObjectRead = true;
}

bool ReflectionContext::isAnyObjectRead() const {
  return this->anyObjectRead;
}

void ReflectionContext::markAnyObjectFilled() {
  this->anyObjectFilled = true;
}

bool ReflectionContext::isAnyObjectFilled() const {
  return this->anyObjectFilled;
}

void ReflectionContext::rememberMetadataType(const void *key, const Type *type, llvm::SMLoc loc) {
  this->metadataTypes[key] = {type, loc};
}

const MetadataType *ReflectionContext::findMetadataType(const void *key) const {
  auto found = this->metadataTypes.find(key);
  if (found == this->metadataTypes.end()) {
    return nullptr;
  }

  return &found->second;
}

void ReflectionContext::addMetadataWriter(llvm::StringRef key, FunctionDecl *functionDecl) {
  std::vector<FunctionDecl *> &writers = this->metadataWriters[key];
  if (!llvm::is_contained(writers, functionDecl)) {
    writers.push_back(functionDecl);
  }
}

std::vector<FunctionDecl *> ReflectionContext::getMetadataWriters(llvm::StringRef key) const {
  auto found = this->metadataWriters.find(key);
  if (found == this->metadataWriters.end()) {
    return {};
  }

  return found->second;
}

void ReflectionContext::rememberBuiltinConstructor(llvm::StringRef name, ClassDecl *classDecl) {
  this->builtinConstructors[name] = classDecl;
}

ClassDecl *ReflectionContext::findBuiltinConstructor(llvm::StringRef name) const {
  return this->builtinConstructors.lookup(name);
}

void ReflectionContext::addReflectedNew(const NewExpr *expr, std::vector<const Type *> args) {
  this->reflectedNews.push_back({expr, std::move(args)});
}

bool ReflectionContext::hasReflectedNews() const {
  return !this->reflectedNews.empty();
}

std::vector<ReflectedNew> ReflectionContext::getReflectedNews() const {
  return this->reflectedNews;
}

void ReflectionContext::rememberConstructorCast(const CastExpr *castExpr, ClassDecl *classDecl) {
  this->constructorCasts[castExpr] = classDecl;
}

void ReflectionContext::forgetConstructorCast(const CastExpr *castExpr) {
  this->constructorCasts.erase(castExpr);
}

std::vector<ConstructorCast> ReflectionContext::getConstructorCasts() const {
  auto casts = std::vector<ConstructorCast>();
  for (const auto &[cast, cls] : this->constructorCasts) {
    casts.push_back({cast, cls});
  }

  return casts;
}

void ReflectionContext::addConstructorWidening(const Type *from, const Type *to) {
  this->constructorWidenings[from].insert(to);
}

std::vector<const Type *> ReflectionContext::getConstructorWidenings(const Type *from) const {
  auto found = this->constructorWidenings.find(from);
  if (found == this->constructorWidenings.end()) {
    return {};
  }

  return {found->second.begin(), found->second.end()};
}

void ReflectionContext::addEscapedClass(ClassDecl *classDecl) {
  this->escapedClasses.insert(classDecl);
}

std::vector<ClassDecl *> ReflectionContext::getEscapedClasses() const {
  return {this->escapedClasses.begin(), this->escapedClasses.end()};
}

void ReflectionContext::addReflectedObject(ClassDecl *classDecl) {
  this->reflectedObjects.insert(classDecl);
}

std::vector<ClassDecl *> ReflectionContext::getReflectedObjects() const {
  return {this->reflectedObjects.begin(), this->reflectedObjects.end()};
}

void ReflectionContext::markSeveralVariants(const CallExpr *call) {
  this->severalVariants.insert(call);
}

bool ReflectionContext::hasSeveralVariants(const CallExpr *call) const {
  return this->severalVariants.contains(call);
}

}
