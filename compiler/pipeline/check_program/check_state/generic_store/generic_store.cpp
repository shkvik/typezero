#include "compiler/pipeline/check_program/check_state/generic_store/generic_store.h"

namespace tz {

void GenericStore::rememberCheckingInstance(InstanceRecord *instance) {
  this->checkingInstance = instance;
}

void GenericStore::forgetCheckingInstance() {
  this->checkingInstance = nullptr;
}

InstanceRecord *GenericStore::getCheckingInstance() const {
  return this->checkingInstance;
}

bool GenericStore::isCheckingInstance() const {
  return this->checkingInstance != nullptr;
}

InstanceRecord *GenericStore::addInstanceRecord() {
  return &this->instanceRecords.emplace_back();
}

void GenericStore::rememberFunctionInstance(const FuncSymbol *generic,
                                            std::vector<const Type *> key,
                                            InstanceRecord *instance) {
  this->functionInstances[{generic, std::move(key)}] = instance;
}

InstanceRecord *GenericStore::findFunctionInstance(const FuncSymbol *generic,
                                                   const std::vector<const Type *> &key) const {
  auto found = this->functionInstances.find({generic, key});
  if (found == this->functionInstances.end()) {
    return nullptr;
  }

  return found->second;
}

void GenericStore::addPendingInstance(InstanceRecord *instance) {
  this->pendingInstances.push_back(instance);
}

int GenericStore::countPendingInstances() const {
  return int(this->pendingInstances.size());
}

InstanceRecord *GenericStore::getPendingInstance(int index) const {
  return this->pendingInstances[index];
}

void GenericStore::forgetPendingInstances() {
  this->pendingInstances.clear();
}

void GenericStore::markBrokenGeneric(const FuncSymbol *generic) {
  this->brokenGenerics.insert(generic);
}

bool GenericStore::isBrokenGeneric(const FuncSymbol *generic) const {
  return this->brokenGenerics.contains(generic);
}

void GenericStore::markOwnClassReturnReported(const FuncSymbol *generic) {
  this->ownClassReturnsReported.insert(generic);
}

bool GenericStore::isOwnClassReturnReported(const FuncSymbol *generic) const {
  return this->ownClassReturnsReported.contains(generic);
}

void GenericStore::rememberFailedCall(llvm::SMLoc loc, const InstanceRecord *instance) {
  this->failedCalls[loc.getPointer()] = instance;
}

const InstanceRecord *GenericStore::findFailedCall(llvm::SMLoc loc) const {
  return this->failedCalls.lookup(loc.getPointer());
}

void GenericStore::rememberForcedType(const Expr *expr, const Type *type) {
  this->forcedTypes[{expr->loc().getPointer(), unsigned(expr->kind())}] = type;
}

bool GenericStore::hasForcedTypes() const {
  return !this->forcedTypes.empty();
}

const Type *GenericStore::findForcedType(const Expr *expr) const {
  return this->forcedTypes.lookup({expr->loc().getPointer(), unsigned(expr->kind())});
}

void GenericStore::rememberClassInstance(const ClassDecl *generic, std::vector<const Type *> args,
                                         ClassDecl *instance) {
  this->classInstances[{generic, std::move(args)}] = instance;
}

ClassDecl *GenericStore::findClassInstance(const ClassDecl *generic,
                                           const std::vector<const Type *> &args) const {
  auto found = this->classInstances.find({generic, args});
  if (found == this->classInstances.end()) {
    return nullptr;
  }

  return found->second;
}

void GenericStore::rememberClassDefaults(const ClassDecl *cls, std::vector<const Type *> defaults) {
  this->classDefaults[cls] = std::move(defaults);
}

std::vector<const Type *> GenericStore::getClassDefaults(const ClassDecl *cls) const {
  return this->classDefaults.lookup(cls);
}

void GenericStore::rememberClassCheck(const ClassDecl *instance, InstanceRecord *check) {
  this->classChecks[instance] = check;
}

InstanceRecord *GenericStore::findClassCheck(const ClassDecl *instance) const {
  return this->classChecks.lookup(instance);
}

void GenericStore::addUnfilledClass(ClassDecl *instance) {
  this->unfilledClasses.push_back(instance);
}

std::vector<ClassDecl *> GenericStore::takeUnfilledClasses() {
  std::vector<ClassDecl *> classes = std::move(this->unfilledClasses);
  this->unfilledClasses.clear();

  return classes;
}

void GenericStore::enterFilling(const ClassDecl *instance) {
  this->filling.push_back(instance);
}

void GenericStore::leaveFilling() {
  this->filling.pop_back();
}

int GenericStore::countFillingOf(const ClassDecl *generic) const {
  int count = 0;
  for (const ClassDecl *instance : this->filling) {
    if (instance->generic() == generic) {
      count += 1;
    }
  }

  return count;
}

void GenericStore::markEndlessReported(const ClassDecl *generic) {
  this->endlessReported.insert(generic);
}

bool GenericStore::isEndlessReported(const ClassDecl *generic) const {
  return this->endlessReported.contains(generic);
}

void GenericStore::rememberConstructorFunction(const ClassDecl *generic, const FunctionDecl *ctor,
                                               FuncSymbol *fn) {
  this->constructorFunctions[{generic, ctor}] = fn;
}

FuncSymbol *GenericStore::findConstructorFunction(const ClassDecl *generic,
                                                  const FunctionDecl *ctor) const {
  auto found = this->constructorFunctions.find({generic, ctor});
  if (found == this->constructorFunctions.end()) {
    return nullptr;
  }

  return found->second;
}

void GenericStore::enterInstanceLoc(llvm::SMLoc loc) {
  this->instanceLocs.push_back(loc);
}

void GenericStore::leaveInstanceLoc() {
  this->instanceLocs.pop_back();
}

llvm::SMLoc GenericStore::getInstanceLoc() const {
  return this->instanceLocs.empty() ? llvm::SMLoc() : this->instanceLocs.back();
}

void GenericStore::rememberExactParams(const FuncSymbol *generic, std::vector<bool> exact) {
  this->exactParams[generic] = std::move(exact);
}

std::optional<std::vector<bool>> GenericStore::findExactParams(const FuncSymbol *generic) const {
  auto found = this->exactParams.find(generic);
  if (found == this->exactParams.end()) {
    return std::nullopt;
  }

  return found->second;
}

void GenericStore::rememberNumberParam(const Type *param, const Type *only, const Type *number) {
  this->numberParams[{param, only}] = number;
}

const Type *GenericStore::findNumberParam(const Type *param, const Type *only) const {
  return this->numberParams.lookup({param, only});
}

void GenericStore::enterSubstitution(const Type *type) {
  this->substituting.insert(type);
}

void GenericStore::leaveSubstitution(const Type *type) {
  this->substituting.erase(type);
}

bool GenericStore::isSubstituting(const Type *type) const {
  return this->substituting.contains(type);
}

}
