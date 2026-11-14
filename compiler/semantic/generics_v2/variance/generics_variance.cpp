#include "compiler/semantic/generics_v2/variance/generics_variance.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/generics_v2/variance/generics_variance_errors.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"

namespace tz {

bool GenericsVariance::isMentioning(const TypeRef &ref, llvm::StringRef name) {
  for (const TypeRef *part : TypeRefParts::collect(ref)) {
    if (part->kind() == TypeRefKind::NAME_REF && part->name == name) {
      return true;
    }
  }

  return false;
}

bool GenericsVariance::isParamMentioning(const Param &param, llvm::StringRef name) {
  return param.type.has_value() && this->isMentioning(*param.type, name);
}

bool GenericsVariance::isTaking(const FunctionDecl *method, llvm::StringRef name) {
  if (method->isStatic() || method->access() != Access::Public) {
    return false;
  }
  for (const Param &param : method->params) {
    if (this->isParamMentioning(param, name)) {
      return true;
    }
  }

  return false;
}

const FunctionDecl *GenericsVariance::publicTaker(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *method : generic->methods) {
    if (this->isTaking(method, name)) {
      return method;
    }
  }

  return nullptr;
}

const ClassField *GenericsVariance::settableField(const ClassDecl *generic, llvm::StringRef name) {
  for (const ClassField &field : generic->fields) {
    bool isOpen = !field.isStatic() && field.access == Access::Public && !field.readonly;
    if (isOpen && field.type.has_value() && this->isMentioning(*field.type, name)) {
      return &field;
    }
  }

  return nullptr;
}

bool GenericsVariance::hasOpenPropertyParam(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *constructor : generic->constructors) {
    for (const Param &param : constructor->params) {
      bool isOpen = param.property.has_value() && param.property->access == Access::Public &&
                    !param.property->readonly;
      if (isOpen && this->isParamMentioning(param, name)) {
        return true;
      }
    }
  }

  return false;
}

std::string GenericsVariance::writerOf(const ClassDecl *generic, llvm::StringRef name) {
  const FunctionDecl *taker = this->publicTaker(generic, name);
  if (taker != nullptr) {
    return ErrorTextVariance::takes(taker->name, name);
  }
  const ClassField *field = this->settableField(generic, name);
  if (field != nullptr) {
    return ErrorTextVariance::settable(field->name);
  }

  return "";
}

bool GenericsVariance::isWrittenFromOutside(const ClassDecl *generic, llvm::StringRef name) {
  if (this->publicTaker(generic, name) != nullptr) {
    return true;
  }
  if (this->settableField(generic, name) != nullptr) {
    return true;
  }

  return this->hasOpenPropertyParam(generic, name);
}

bool GenericsVariance::isReferenceFitting(const Type *own, const Type *wide) {
  return WideningRelations::isReferenceHolding(ReferenceFitsGoal{.own = own, .wide = wide});
}

bool GenericsVariance::isCovariant(const ClassDecl *generic, size_t index) {
  auto ready = this->readyVariance(generic, index);
  if (ready.has_value()) {
    return *ready;
  }
  auto frames = std::vector<VarianceFrame>();
  this->startVariance(frames, generic, index);
  while (!frames.empty()) {
    this->varianceStep(frames);
  }

  return this->knownVariances[std::make_pair(generic, index)];
}

std::optional<bool> GenericsVariance::readyVariance(const ClassDecl *generic, size_t index) {
  if (index >= generic->typeParams.size()) {
    return false;
  }
  auto &known = this->knownVariances;
  auto found = known.find(std::make_pair(generic, index));
  if (found == known.end()) {
    return std::nullopt;
  }

  return found->second;
}

void GenericsVariance::startVariance(std::vector<VarianceFrame> &frames, const ClassDecl *generic,
                                     size_t index) {
  auto &known = this->knownVariances;
  auto key = std::make_pair(generic, index);
  known[key] = false;
  llvm::StringRef name = generic->typeParams[index].name;
  if (this->isWrittenFromOutside(generic, name)) {
    return;
  }
  if (!generic->base.has_value() || !this->isMentioning(*generic->base, name)) {
    known[key] = true;
    return;
  }
  const ClassDecl *parent = generic->parent;
  const ClassDecl *baseGeneric =
      parent != nullptr && parent->generic() != nullptr ? parent->generic() : parent;
  if (baseGeneric == nullptr) {
    return;
  }
  auto frame = VarianceFrame{
      .generic = generic,
      .index = index,
      .baseGeneric = baseGeneric,
      .nextArg = 0,
  };
  frames.push_back(frame);
}

void GenericsVariance::varianceStep(std::vector<VarianceFrame> &frames) {
  VarianceFrame &frame = frames.back();
  llvm::StringRef name = frame.generic->typeParams[frame.index].name;
  const std::vector<TypeRef> &args = frame.generic->base->args;
  while (frame.nextArg < args.size()) {
    size_t index = frame.nextArg;
    auto passing = this->argumentVariance(frame.baseGeneric, args[index], name, index);
    if (passing == Variance::UNSAFE) {
      return this->finishVariance(frames, Variance::UNSAFE);
    }
    if (passing == Variance::WAITING) {
      return this->startVariance(frames, frame.baseGeneric, index);
    }
    frame.nextArg += 1;
  }

  return this->finishVariance(frames, Variance::SAFE);
}

GenericsVariance::Variance GenericsVariance::argumentVariance(const ClassDecl *baseGeneric,
                                                              const TypeRef &arg,
                                                              llvm::StringRef name, size_t index) {
  if (!this->isMentioning(arg, name)) {
    return Variance::SAFE;
  }
  bool isDirect = arg.kind() == TypeRefKind::NAME_REF && arg.name == name;
  if (!isDirect) {
    return Variance::UNSAFE;
  }
  auto passed = this->readyVariance(baseGeneric, index);
  if (!passed.has_value()) {
    return Variance::WAITING;
  }

  return *passed ? Variance::SAFE : Variance::UNSAFE;
}

void GenericsVariance::finishVariance(std::vector<VarianceFrame> &frames, Variance variance) {
  const VarianceFrame &frame = frames.back();
  auto key = std::make_pair(frame.generic, frame.index);
  this->knownVariances[key] = variance == Variance::SAFE;
  frames.pop_back();
}

const ClassDecl *GenericsVariance::instanceOf(const ClassDecl *classDecl,
                                              const ClassDecl *generic) {
  const ClassDecl *current = classDecl;
  while (current != nullptr) {
    if (current->generic() == generic) {
      return current;
    }
    current = current->parent;
  }

  return nullptr;
}

bool GenericsVariance::isWidening(const Type *from, const Type *to) {
  return WideningRelations::isReferenceHolding(WidensGoal{.from = from, .to = to});
}

const ClassDecl *GenericsVariance::genericTarget(const Type *from, const Type *to) {
  if (!from->isClass() || !to->isClass() || from->classDecl() == nullptr) {
    return nullptr;
  }
  const ClassDecl *target = to->classDecl();
  if (target == nullptr || target->generic() == nullptr) {
    return nullptr;
  }

  return target;
}

std::string GenericsVariance::whyNot(const Type *from, const Type *to) {
  const ClassDecl *target = this->genericTarget(from, to);
  if (target == nullptr) {
    return "";
  }
  const auto *instance = this->instanceOf(from->classDecl(), target->generic());
  if (instance == nullptr || instance->typeArgs.size() != target->typeArgs.size()) {
    return "";
  }
  for (size_t index = 0; index < target->typeArgs.size(); ++index) {
    const Type *own = instance->typeArgs[index];
    const Type *wide = target->typeArgs[index];
    auto why = this->argumentWhyNot(target->generic(), own, wide, index);
    if (!why.empty()) {
      return why;
    }
  }

  return "";
}

std::string GenericsVariance::argumentWhyNot(const ClassDecl *generic, const Type *own,
                                             const Type *wide, size_t index) {
  if (own == wide) {
    return "";
  }
  if (!this->isCovariant(generic, index)) {
    return this->invariantText(generic, index);
  }
  if (this->isReferenceFitting(own, wide)) {
    return "";
  }
  std::string ownName = own->name().str();
  std::string wideName = wide->name().str();
  bool isNamed = !ownName.empty() && ownName.front() != '{' && !own->isUnion();
  std::string example = isNamed ? ownName : ErrorTextVariance::sampleName();

  return ErrorTextVariance::noExtends(ownName, wideName, example);
}

std::string GenericsVariance::invariantText(const ClassDecl *generic, size_t index) {
  llvm::StringRef name = generic->typeParams[index].name;
  auto writer = this->writerOf(generic, name);
  if (writer.empty()) {
    writer = ErrorTextVariance::baseTakes(name);
  }

  return ErrorTextVariance::wider(generic->name, name, writer);
}

ClassDecl *GenericsVariance::sharedGeneric(const ClassDecl *first,
                                           llvm::ArrayRef<const Type *> members) {
  const ClassDecl *candidate = first;
  while (candidate != nullptr) {
    ClassDecl *generic = candidate->generic();
    if (generic != nullptr && this->instancesOf(members, generic).size() == members.size()) {
      return generic;
    }
    candidate = candidate->parent;
  }

  return nullptr;
}

const ClassDecl *GenericsVariance::memberInstance(const Type *member, const ClassDecl *generic) {
  const ClassDecl *classDecl = member->isClass() ? member->classDecl() : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  const ClassDecl *instance = this->instanceOf(classDecl, generic);
  if (instance == nullptr || instance->typeArgs.size() != generic->typeParams.size()) {
    return nullptr;
  }

  return instance;
}

std::vector<const ClassDecl *> GenericsVariance::instancesOf(llvm::ArrayRef<const Type *> members,
                                                             const ClassDecl *generic) {
  auto instances = std::vector<const ClassDecl *>();
  for (const Type *member : members) {
    const ClassDecl *instance = this->memberInstance(member, generic);
    if (instance == nullptr) {
      return instances;
    }
    instances.push_back(instance);
  }

  return instances;
}

std::vector<const Type *>
GenericsVariance::distinctArgs(llvm::ArrayRef<const ClassDecl *> instances, size_t index) {
  auto seen = std::vector<const Type *>();
  for (const ClassDecl *instance : instances) {
    const Type *arg = instance->typeArgs[index];
    if (!llvm::is_contained(seen, arg)) {
      seen.push_back(arg);
    }
  }

  return seen;
}

std::optional<std::vector<const Type *>>
GenericsVariance::joinedArgs(const ClassDecl *generic,
                             llvm::ArrayRef<const ClassDecl *> instances) {
  auto args = std::vector<const Type *>();
  for (size_t index = 0; index < generic->typeParams.size(); ++index) {
    auto seen = this->distinctArgs(instances, index);
    bool isShared = seen.size() == 1;
    if (!isShared && !this->isCovariant(generic, index)) {
      return std::nullopt;
    }
    const Type *arg = isShared ? seen.front() : Type::unionOf(seen);
    args.push_back(arg);
  }

  return args;
}

}
