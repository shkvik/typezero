#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include <map>

namespace tz {

GenericWidening::GenericWidening(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeRelations = &all.typeRelations;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

bool GenericWidening::isMentioning(const TypeRef &ref, llvm::StringRef name) {
  for (const TypeRef *part : TypeRefParts::collect(ref)) {
    if (part->kind() == TypeRefKind::NAME_REF && part->name == name) {
      return true;
    }
  }

  return false;
}

std::map<std::pair<const ClassDecl *, size_t>, bool> &GenericWidening::knownVariance() {
  static std::map<std::pair<const ClassDecl *, size_t>, bool> known;

  return known;
}

bool GenericWidening::isParamMentioning(const Param &param, llvm::StringRef name) {
  return param.type.has_value() && GenericWidening::isMentioning(*param.type, name);
}

bool GenericWidening::isTaking(const FunctionDecl *method, llvm::StringRef name) {
  if (method->isStatic() || method->access() != Access::Public) {
    return false;
  }
  for (const Param &param : method->params) {
    if (GenericWidening::isParamMentioning(param, name)) {
      return true;
    }
  }

  return false;
}

const FunctionDecl *GenericWidening::publicTaker(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *method : generic->methods) {
    if (GenericWidening::isTaking(method, name)) {
      return method;
    }
  }

  return nullptr;
}

const ClassField *GenericWidening::settableField(const ClassDecl *generic, llvm::StringRef name) {
  for (const ClassField &field : generic->fields) {
    bool isOpen = !field.isStatic() && field.access == Access::Public && !field.readonly;
    if (isOpen && field.type.has_value() && GenericWidening::isMentioning(*field.type, name)) {
      return &field;
    }
  }

  return nullptr;
}

bool GenericWidening::hasOpenPropertyParam(const ClassDecl *generic, llvm::StringRef name) {
  for (const FunctionDecl *constructor : generic->constructors) {
    for (const Param &param : constructor->params) {
      bool isOpen = param.property.has_value() && param.property->access == Access::Public &&
                    !param.property->readonly;
      if (isOpen && GenericWidening::isParamMentioning(param, name)) {
        return true;
      }
    }
  }

  return false;
}

std::string GenericWidening::writerOf(const ClassDecl *generic, llvm::StringRef name) const {
  const FunctionDecl *taker = GenericWidening::publicTaker(generic, name);
  if (taker != nullptr) {
    return ErrorTextVariance::takes(taker->name, name);
  }
  const ClassField *field = GenericWidening::settableField(generic, name);
  if (field != nullptr) {
    return ErrorTextVariance::settable(field->name);
  }

  return "";
}

bool GenericWidening::isWrittenFromOutside(const ClassDecl *generic, llvm::StringRef name) {
  if (GenericWidening::publicTaker(generic, name) != nullptr) {
    return true;
  }
  if (GenericWidening::settableField(generic, name) != nullptr) {
    return true;
  }

  return GenericWidening::hasOpenPropertyParam(generic, name);
}

bool GenericWidening::isReferenceFitting(const Type *own, const Type *wide) const {
  return WideningRelations::isReferenceHolding(ReferenceFitsGoal{.own = own, .wide = wide});
}

bool GenericWidening::isCovariant(const ClassDecl *generic, size_t index) {
  auto ready = GenericWidening::readyVariance(generic, index);
  if (ready.has_value()) {
    return *ready;
  }
  auto frames = std::vector<VarianceFrame>();
  GenericWidening::startVariance(frames, generic, index);
  while (!frames.empty()) {
    GenericWidening::varianceStep(frames);
  }

  return GenericWidening::knownVariance()[std::make_pair(generic, index)];
}

std::optional<bool> GenericWidening::readyVariance(const ClassDecl *generic, size_t index) {
  if (index >= generic->typeParams.size()) {
    return false;
  }
  auto &known = GenericWidening::knownVariance();
  auto found = known.find(std::make_pair(generic, index));
  if (found == known.end()) {
    return std::nullopt;
  }

  return found->second;
}

void GenericWidening::startVariance(std::vector<VarianceFrame> &frames, const ClassDecl *generic,
                                    size_t index) {
  auto &known = GenericWidening::knownVariance();
  auto key = std::make_pair(generic, index);
  known[key] = false;
  llvm::StringRef name = generic->typeParams[index].name;
  if (GenericWidening::isWrittenFromOutside(generic, name)) {
    return;
  }
  if (!generic->base.has_value() || !GenericWidening::isMentioning(*generic->base, name)) {
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

void GenericWidening::varianceStep(std::vector<VarianceFrame> &frames) {
  VarianceFrame &frame = frames.back();
  llvm::StringRef name = frame.generic->typeParams[frame.index].name;
  const std::vector<TypeRef> &args = frame.generic->base->args;
  while (frame.nextArg < args.size()) {
    size_t index = frame.nextArg;
    auto passing = GenericWidening::argumentVariance(frame.baseGeneric, args[index], name, index);
    if (passing == Variance::UNSAFE) {
      return GenericWidening::finishVariance(frames, Variance::UNSAFE);
    }
    if (passing == Variance::WAITING) {
      return GenericWidening::startVariance(frames, frame.baseGeneric, index);
    }
    frame.nextArg += 1;
  }

  return GenericWidening::finishVariance(frames, Variance::SAFE);
}

GenericWidening::Variance GenericWidening::argumentVariance(const ClassDecl *baseGeneric,
                                                            const TypeRef &arg,
                                                            llvm::StringRef name, size_t index) {
  if (!GenericWidening::isMentioning(arg, name)) {
    return Variance::SAFE;
  }
  bool isDirect = arg.kind() == TypeRefKind::NAME_REF && arg.name == name;
  if (!isDirect) {
    return Variance::UNSAFE;
  }
  auto passed = GenericWidening::readyVariance(baseGeneric, index);
  if (!passed.has_value()) {
    return Variance::WAITING;
  }

  return *passed ? Variance::SAFE : Variance::UNSAFE;
}

void GenericWidening::finishVariance(std::vector<VarianceFrame> &frames, Variance variance) {
  const VarianceFrame &frame = frames.back();
  auto key = std::make_pair(frame.generic, frame.index);
  GenericWidening::knownVariance()[key] = variance == Variance::SAFE;
  frames.pop_back();
}

const ClassDecl *GenericWidening::instanceOf(const ClassDecl *classDecl, const ClassDecl *generic) {
  const ClassDecl *current = classDecl;
  while (current != nullptr) {
    if (current->generic() == generic) {
      return current;
    }
    current = current->parent;
  }

  return nullptr;
}

bool GenericWidening::isWidening(const Type *from, const Type *to) {
  return WideningRelations::isReferenceHolding(WidensGoal{.from = from, .to = to});
}

const ClassDecl *GenericWidening::genericTarget(const Type *from, const Type *to) const {
  if (!from->isClass() || !to->isClass() || from->classDecl() == nullptr) {
    return nullptr;
  }
  const ClassDecl *target = to->classDecl();
  if (target == nullptr || target->generic() == nullptr) {
    return nullptr;
  }

  return target;
}

std::string GenericWidening::whyNot(const Type *from, const Type *to) const {
  const ClassDecl *target = this->genericTarget(from, to);
  if (target == nullptr) {
    return "";
  }
  const auto *instance = GenericWidening::instanceOf(from->classDecl(), target->generic());
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

std::string GenericWidening::argumentWhyNot(const ClassDecl *generic, const Type *own,
                                            const Type *wide, size_t index) const {
  if (own == wide) {
    return "";
  }
  if (!GenericWidening::isCovariant(generic, index)) {
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

std::string GenericWidening::invariantText(const ClassDecl *generic, size_t index) const {
  llvm::StringRef name = generic->typeParams[index].name;
  auto writer = this->writerOf(generic, name);
  if (writer.empty()) {
    writer = ErrorTextVariance::baseTakes(name);
  }

  return ErrorTextVariance::wider(generic->name, name, writer);
}

Expr *GenericWidening::joined(Expr *expr) {
  const Type *type = expr->type;
  if (!type->isUnion()) {
    return expr;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  const ClassDecl *first = members.front()->classDecl();
  if (!members.front()->isClass() || first == nullptr) {
    return expr;
  }
  ClassDecl *generic = this->sharedGeneric(first, members);
  if (generic == nullptr) {
    return expr;
  }
  auto instances = this->instancesOf(members, generic);

  return this->widenToGeneric(expr, generic, instances);
}

ClassDecl *GenericWidening::sharedGeneric(const ClassDecl *first,
                                          llvm::ArrayRef<const Type *> members) const {
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

const ClassDecl *GenericWidening::memberInstance(const Type *member,
                                                 const ClassDecl *generic) const {
  const ClassDecl *classDecl = member->isClass() ? member->classDecl() : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  const ClassDecl *instance = GenericWidening::instanceOf(classDecl, generic);
  if (instance == nullptr || instance->typeArgs.size() != generic->typeParams.size()) {
    return nullptr;
  }

  return instance;
}

std::vector<const ClassDecl *> GenericWidening::instancesOf(llvm::ArrayRef<const Type *> members,
                                                            const ClassDecl *generic) const {
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

Expr *GenericWidening::widenToGeneric(Expr *expr, ClassDecl *generic,
                                      llvm::ArrayRef<const ClassDecl *> instances) {
  auto args = this->joinedArgs(generic, instances);
  if (!args.has_value()) {
    return expr;
  }
  auto widened = this->genericClassInstantiator->instantiateClass(generic, *args, expr->loc());
  if (widened.isErr()) {
    this->reportInstantiation(widened.error(), generic, expr->loc());

    return expr;
  }
  const Type *wideType = widened.value()->type;
  if (!TypeRelations::isImplicitlyConvertible(expr->type, wideType)) {
    return expr;
  }

  return this->typeRelations->convert(expr, wideType);
}

void GenericWidening::reportInstantiation(
    const GenericClassInstantiatorInstantiateClassError &error, const ClassDecl *generic,
    llvm::SMLoc use) {
  if (error.code == GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    return;
  }
  bool isEndless =
      error.code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
  llvm::SMLoc at = isEndless ? generic->loc() : use;
  this->state.getDiagnostics().error(at, error.message);
}

std::vector<const Type *> GenericWidening::distinctArgs(llvm::ArrayRef<const ClassDecl *> instances,
                                                        size_t index) const {
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
GenericWidening::joinedArgs(const ClassDecl *generic,
                            llvm::ArrayRef<const ClassDecl *> instances) const {
  auto args = std::vector<const Type *>();
  for (size_t index = 0; index < generic->typeParams.size(); ++index) {
    auto seen = this->distinctArgs(instances, index);
    bool isShared = seen.size() == 1;
    if (!isShared && !GenericWidening::isCovariant(generic, index)) {
      return std::nullopt;
    }
    const Type *arg = isShared ? seen.front() : Type::unionOf(seen);
    args.push_back(arg);
  }

  return args;
}

}
