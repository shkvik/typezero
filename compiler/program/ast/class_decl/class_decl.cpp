#include "compiler/program/ast/class_decl/class_decl.h"

namespace tz {

bool ClassDecl::isGeneric() const {
  return !this->typeParams.empty() && !this->generic();
}

bool ClassDecl::isEmitted() const {
  return !this->isGeneric() && !this->checkOnly;
}

const ClassDecl *ClassDecl::staticOwner() const {
  const ClassDecl *original = this->generic();

  return original ? original : this;
}

ClassDecl *ClassDecl::staticOwner() {
  ClassDecl *original = this->generic();

  return original ? original : this;
}

ClassDecl *ClassDecl::generic() const {
  const GenericInstance *instance = this->generics.as<GenericInstance>();

  return instance ? instance->generic : nullptr;
}

llvm::ArrayRef<ClassDecl *> ClassDecl::instances() const {
  const GenericTemplate *templated = this->generics.as<GenericTemplate>();

  return templated ? llvm::ArrayRef<ClassDecl *>(templated->instances)
                   : llvm::ArrayRef<ClassDecl *>();
}

bool ClassDecl::library() const {
  return this->origin == ClassOrigin::LIBRARY;
}

bool ClassDecl::builtin() const {
  return this->origin == ClassOrigin::BUILTIN;
}

bool ClassDecl::expression() const {
  return this->syntax.is<ExpressionClass>();
}

ObjectLitExpr *ClassDecl::literal() const {
  const LiteralClass *made = this->syntax.as<LiteralClass>();

  return made ? made->literal : nullptr;
}

bool ClassDecl::hasMethodFields() const {
  if (this->literal() != nullptr) {
    return true;
  }
  for (const ClassDecl *owner = this; owner != nullptr; owner = owner->parent) {
    if (owner->hasOwnMethodField()) {
      return true;
    }
  }

  return false;
}

bool ClassDecl::hasOwnMethodField() const {
  for (const ClassField &field : this->fields) {
    if (field.method() != nullptr) {
      return true;
    }
  }

  return false;
}

std::vector<FunctionDecl *> ClassDecl::findConstructors() const {
  for (const ClassDecl *current = this; current != nullptr; current = current->parent) {
    if (!current->constructors.empty()) {
      return current->constructors;
    }
  }

  return {};
}

std::vector<FunctionDecl *> ClassDecl::collectMemberFunctions() const {
  std::vector<FunctionDecl *> members = this->methods;
  members.insert(members.end(), this->constructors.begin(), this->constructors.end());
  if (this->init != nullptr) {
    members.push_back(this->init);
  }
  for (const ClassField &field : this->fields) {
    if (field.method() != nullptr) {
      members.push_back(field.method());
    }
  }

  return members;
}

const ClassDecl *ClassDecl::findFinalClass() const {
  const ClassDecl *current = this;
  while (current->replacement != nullptr) {
    current = current->replacement;
  }

  return current;
}

const ClassField *ClassDecl::findField(llvm::StringRef name) const {
  const auto *own = this->field(name);
  if (own != nullptr) {
    return own;
  }

  return this->findInheritedField(name);
}

const ClassField *ClassDecl::findInheritedField(llvm::StringRef name) const {
  for (const ClassDecl *current = this->parent; current != nullptr; current = current->parent) {
    const auto *field = current->field(name);
    if (field != nullptr) {
      return field;
    }
  }

  return nullptr;
}

const ClassDecl *ClassDecl::findFieldOwner(llvm::StringRef name) const {
  if (this->field(name) != nullptr) {
    return this;
  }

  return this->findInheritedFieldOwner(name);
}

std::vector<FunctionDecl *> ClassDecl::findOwnMethods() const {
  auto methods = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : this->methods) {
    if (!functionDecl->isStatic()) {
      methods.push_back(functionDecl);
    }
  }
  for (const ClassField &field : this->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

FunctionDecl *ClassDecl::findSoleImplementation(FunctionDecl *variant) const {
  if (variant->openToLate()) {
    return nullptr;
  }
  if (!variant->overridden() && !variant->isAbstract()) {
    return variant;
  }
  if (variant->generic != nullptr && variant->overridden()) {
    return nullptr;
  }
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only != nullptr ? 1 : 0;
  std::vector<const ClassDecl *> stack(this->children.rbegin(), this->children.rend());
  while (!stack.empty()) {
    const ClassDecl *child = stack.back();
    stack.pop_back();
    for (FunctionDecl *functionDecl : child->findOwnMethods()) {
      bool isImplementing = !functionDecl->isAbstract() && functionDecl != variant &&
                            this->isOverriding(functionDecl, variant);
      if (isImplementing) {
        only = functionDecl;
        count += child->isEmitted() ? 1 : 2;
      }
    }
    stack.insert(stack.end(), child->children.rbegin(), child->children.rend());
  }

  return count == 1 ? only : nullptr;
}

const ClassDecl *ClassDecl::findInheritedFieldOwner(llvm::StringRef name) const {
  for (const ClassDecl *current = this->parent; current != nullptr; current = current->parent) {
    const auto *field = current->field(name);
    if (field != nullptr) {
      return current;
    }
  }

  return nullptr;
}

std::vector<FunctionDecl *> ClassDecl::findMethodsNamed(llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = this; current != nullptr; current = current->parent) {
    auto own = current->ownMethodsNamed(name);
    this->addNotOverridden(found, own);
  }

  return found;
}

std::vector<FunctionDecl *> ClassDecl::ownMethodsNamed(llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : this->methods) {
    if (method->name == name && !method->isStatic()) {
      own.push_back(method);
    }
  }
  for (const ClassField &field : this->fields) {
    if (field.method() != nullptr && field.name == name) {
      own.push_back(field.method());
    }
  }

  return own;
}

void ClassDecl::addNotOverridden(std::vector<FunctionDecl *> &found,
                                 const std::vector<FunctionDecl *> &own) const {
  for (FunctionDecl *method : own) {
    if (!this->isOverriddenByAny(found, method)) {
      found.push_back(method);
    }
  }
}

bool ClassDecl::isOverriddenByAny(const std::vector<FunctionDecl *> &found,
                                  const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->isOverriding(lower, method)) {
      return true;
    }
  }

  return false;
}

bool ClassDecl::isOverriding(const FunctionDecl *method, const FunctionDecl *base) const {
  for (const FunctionDecl *current = method; current != nullptr; current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

std::vector<FunctionDecl *> ClassDecl::findStaticMethodsNamed(llvm::StringRef name) const {
  auto found = std::vector<FunctionDecl *>();
  for (const ClassDecl *current = this; current != nullptr; current = current->parent) {
    auto own = current->ownStaticMethodsNamed(name);
    this->addNotHidden(found, own);
  }

  return found;
}

std::vector<FunctionDecl *> ClassDecl::ownStaticMethodsNamed(llvm::StringRef name) const {
  auto own = std::vector<FunctionDecl *>();
  for (FunctionDecl *method : this->staticOwner()->methods) {
    if (method->name == name && method->isStatic()) {
      own.push_back(method);
    }
  }

  return own;
}

void ClassDecl::addNotHidden(std::vector<FunctionDecl *> &found,
                             const std::vector<FunctionDecl *> &own) const {
  for (FunctionDecl *method : own) {
    if (!this->isHiddenByAny(found, method)) {
      found.push_back(method);
    }
  }
}

bool ClassDecl::isHiddenByAny(const std::vector<FunctionDecl *> &found,
                              const FunctionDecl *method) const {
  for (const FunctionDecl *lower : found) {
    if (this->hasSameSignature(lower->sym, method->sym)) {
      return true;
    }
  }

  return false;
}

bool ClassDecl::hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const {
  if (first == nullptr || second == nullptr) {
    return false;
  }

  return first->params == second->params && first->minArgs == second->minArgs &&
         first->hasRest == second->hasRest;
}

}
