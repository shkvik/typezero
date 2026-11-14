#include "compiler/semantic/semantic_state/v2/generics/generic_copy_context/generic_copy_context.h"

namespace tz {

void GenericCopyContext::rememberCheckingInstance(InstanceRecord *instance) {
  this->checkingInstance = instance;
}

void GenericCopyContext::forgetCheckingInstance() {
  this->checkingInstance = nullptr;
}

InstanceRecord *GenericCopyContext::getCheckingInstance() const {
  return this->checkingInstance;
}

bool GenericCopyContext::isCheckingInstance() const {
  return this->checkingInstance != nullptr;
}

InstanceRecord *GenericCopyContext::addInstanceRecord() {
  return &this->instanceRecords.emplace_back();
}

void GenericCopyContext::rememberFunctionInstance(const FuncSymbol *generic,
                                                  std::vector<const Type *> key,
                                                  InstanceRecord *instance) {
  this->functionInstances[{generic, std::move(key)}] = instance;
}

InstanceRecord *
GenericCopyContext::findFunctionInstance(const FuncSymbol *generic,
                                         const std::vector<const Type *> &key) const {
  auto found = this->functionInstances.find({generic, key});
  if (found == this->functionInstances.end()) {
    return nullptr;
  }

  return found->second;
}

void GenericCopyContext::addPendingInstance(InstanceRecord *instance) {
  this->pendingInstances.push_back(instance);
}

int GenericCopyContext::countPendingInstances() const {
  return int(this->pendingInstances.size());
}

InstanceRecord *GenericCopyContext::getPendingInstance(int index) const {
  return this->pendingInstances[index];
}

void GenericCopyContext::forgetPendingInstances() {
  this->pendingInstances.clear();
}

void GenericCopyContext::markBrokenGeneric(const FuncSymbol *generic) {
  this->brokenGenerics.insert(generic);
}

bool GenericCopyContext::isBrokenGeneric(const FuncSymbol *generic) const {
  return this->brokenGenerics.contains(generic);
}

void GenericCopyContext::markOwnClassReturnReported(const FuncSymbol *generic) {
  this->ownClassReturnsReported.insert(generic);
}

bool GenericCopyContext::isOwnClassReturnReported(const FuncSymbol *generic) const {
  return this->ownClassReturnsReported.contains(generic);
}

void GenericCopyContext::rememberFailedCall(llvm::SMLoc loc, const InstanceRecord *instance) {
  this->failedCalls[loc.getPointer()] = instance;
}

const InstanceRecord *GenericCopyContext::findFailedCall(llvm::SMLoc loc) const {
  return this->failedCalls.lookup(loc.getPointer());
}

void GenericCopyContext::rememberForcedType(const Expr *expr, const Type *type) {
  this->forcedTypes[{expr->loc().getPointer(), unsigned(expr->kind())}] = type;
}

bool GenericCopyContext::hasForcedTypes() const {
  return !this->forcedTypes.empty();
}

const Type *GenericCopyContext::findForcedType(const Expr *expr) const {
  return this->forcedTypes.lookup({expr->loc().getPointer(), unsigned(expr->kind())});
}

void GenericCopyContext::rememberClassDefaults(const ClassDecl *classDecl,
                                               std::vector<const Type *> defaults) {
  this->classDefaults[classDecl] = std::move(defaults);
}

std::vector<const Type *> GenericCopyContext::getClassDefaults(const ClassDecl *classDecl) const {
  return this->classDefaults.lookup(classDecl);
}

void GenericCopyContext::rememberClassCheck(const ClassDecl *instance, InstanceRecord *check) {
  this->classChecks[instance] = check;
}

InstanceRecord *GenericCopyContext::findClassCheck(const ClassDecl *instance) const {
  return this->classChecks.lookup(instance);
}

void GenericCopyContext::addUnfilledClass(ClassDecl *instance) {
  this->unfilledClasses.push_back(instance);
}

std::vector<ClassDecl *> GenericCopyContext::takeUnfilledClasses() {
  std::vector<ClassDecl *> classes = std::move(this->unfilledClasses);
  this->unfilledClasses.clear();

  return classes;
}

void GenericCopyContext::enterFilling(const ClassDecl *instance) {
  this->filling.push_back(instance);
}

void GenericCopyContext::leaveFilling() {
  this->filling.pop_back();
}

int GenericCopyContext::countFillingOf(const ClassDecl *generic) const {
  int count = 0;
  for (const ClassDecl *instance : this->filling) {
    if (instance->generic() == generic) {
      count += 1;
    }
  }

  return count;
}

void GenericCopyContext::markEndlessReported(const ClassDecl *generic) {
  this->endlessReported.insert(generic);
}

bool GenericCopyContext::isEndlessReported(const ClassDecl *generic) const {
  return this->endlessReported.contains(generic);
}

void GenericCopyContext::rememberConstructorFunction(const ClassDecl *generic,
                                                     const FunctionDecl *constructor,
                                                     FuncSymbol *function) {
  this->constructorFunctions[{generic, constructor}] = function;
}

FuncSymbol *GenericCopyContext::findConstructorFunction(const ClassDecl *generic,
                                                        const FunctionDecl *constructor) const {
  auto found = this->constructorFunctions.find({generic, constructor});
  if (found == this->constructorFunctions.end()) {
    return nullptr;
  }

  return found->second;
}

void GenericCopyContext::enterInstanceLoc(llvm::SMLoc loc) {
  this->instanceLocs.push_back(loc);
}

void GenericCopyContext::leaveInstanceLoc() {
  this->instanceLocs.pop_back();
}

llvm::SMLoc GenericCopyContext::getInstanceLoc() const {
  return this->instanceLocs.empty() ? llvm::SMLoc() : this->instanceLocs.back();
}

void GenericCopyContext::markBrokenClass(const ClassDecl *classDecl) {
  this->brokenClasses.insert(classDecl);
}

bool GenericCopyContext::isBrokenClass(const ClassDecl *classDecl) const {
  return this->brokenClasses.contains(classDecl);
}

}
