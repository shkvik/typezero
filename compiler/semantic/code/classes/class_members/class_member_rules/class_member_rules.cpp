#include "compiler/semantic/code/classes/class_members/class_member_rules/class_member_rules.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kObjectPlaceholder = "o";

}

ClassMemberRules::ClassMemberRules(SemanticState &owner, ClassFacts &sharedFacts)
    : facts(sharedFacts),
      state(owner) {}

NameExpr *ClassMemberRules::classValueName(NameExpr *name, ClassDecl *classDecl) {
  auto declared = this->checkClassDeclared(classDecl);
  if (declared.isErr()) {
    this->state.diagnostics.error(name->loc(), declared.error().message);
  }
  auto *finalDecl = this->facts.finalClass(classDecl);
  name->binding = NamedClass{.cls = finalDecl};
  name->type = this->facts.valueTypeOf(finalDecl);
  finalDecl->usedAsValue = true;

  return name;
}

bool ClassMemberRules::isFunctionWanted(llvm::ArrayRef<FunctionDecl *> variants,
                                        const Type *expected) const {
  return variants.size() > 1 && expected != nullptr && expected->nonOptional()->isFunction();
}

void ClassMemberRules::pushVariantTypes(llvm::ArrayRef<FunctionDecl *> variants,
                                        const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  if (variants.size() > 1) {
    tasks.pushTask(ReturnTypeOf{.function = variants.back()->sym});
  }
  if (!this->isFunctionWanted(variants, expected)) {
    return;
  }
  for (FunctionDecl *variant : llvm::reverse(variants)) {
    tasks.pushTask(FunctionTypeOf{.function = variant->sym});
  }
}

std::vector<const Type *>
ClassMemberRules::takeVariantTypes(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected) {
  if (!this->isFunctionWanted(variants, expected)) {
    return {};
  }
  auto types = std::vector<const Type *>(variants.size());
  for (size_t index = variants.size(); index > 0; --index) {
    types[index - 1] = this->state.walk.tasks.takeAnswer();
  }

  return types;
}

const Type *ClassMemberRules::takeLastVariantReturn(llvm::ArrayRef<FunctionDecl *> variants) {
  if (variants.size() < 2) {
    return nullptr;
  }

  return this->state.walk.tasks.takeAnswer();
}

ClassMemberRules::ResultVariantOfType
ClassMemberRules::variantForType(llvm::ArrayRef<FunctionDecl *> variants,
                                 llvm::ArrayRef<const Type *> types, const Type *lastReturn,
                                 const Type *expected, const MemberExpr *member) const {
  if (variants.size() == 1) {
    return ok(variants[0]->sym);
  }
  auto matching = this->variantsOfType(variants, types, expected);
  if (matching.size() == 1) {
    return ok(matching[0]->sym);
  }
  auto type = this->signatureText(variants.back(), lastReturn);
  auto object = this->shownObject(member);
  auto list = ErrorTextVariant::list(variants);
  auto message = ErrorTextVariantType::several(member->name, list, type, object);

  return err(CodeClassesRegistryVariantForTypeError{
      .code = CodeClassesRegistryVariantForTypeErrorCode::SEVERAL_VARIANTS,
      .message = message,
  });
}

std::vector<FunctionDecl *>
ClassMemberRules::variantsOfType(llvm::ArrayRef<FunctionDecl *> variants,
                                 llvm::ArrayRef<const Type *> types, const Type *expected) const {
  const auto *callback = this->state.walk.functions.getExpectedCallback();
  bool isFunctionWanted = expected != nullptr && expected->nonOptional()->isFunction();
  auto matching = std::vector<FunctionDecl *>();
  for (size_t index = 0; index < variants.size(); ++index) {
    FunctionDecl *variant = variants[index];
    bool isMatching = isFunctionWanted ? types[index] == expected->nonOptional()
                                       : this->isFittingCallback(variant->sym, callback);
    if (isMatching) {
      matching.push_back(variant);
    }
  }

  return matching;
}

ClassMemberRules::ResultFieldIsSetBefore ClassMemberRules::checkFieldSet(const ClassDecl *owner,
                                                                         llvm::StringRef name,
                                                                         bool isViaThis) const {
  bool isUnset = isViaThis && owner == this->state.walk.self.getInitializer()->cls() &&
                 !this->state.walk.flow.isFieldSet(name);
  if (isUnset) {
    auto message = ErrorTextClassField::readEarly(name);

    return err(CodeClassesRegistryFieldSetError{
        .code = CodeClassesRegistryFieldSetErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

ClassMemberRules::ResultFieldTypeIsKnown
ClassMemberRules::checkFieldTyped(const ClassField *field, llvm::StringRef name) const {
  if (field->resolved == nullptr) {
    auto message = ErrorTextClassField::laterType(name);

    return err(CodeClassesRegistryFieldTypedError{
        .code = CodeClassesRegistryFieldTypedErrorCode::TYPE_FROM_INITIALIZER,
        .message = message,
    });
  }

  return ok(field->resolved);
}

ClassMemberRules::ResultClassIsDeclared
ClassMemberRules::checkClassDeclared(const ClassDecl *classDecl) const {
  bool isEarly = !classDecl->declared &&
                 this->state.declaration.modules.getLoadingModule() != nullptr &&
                 !this->state.walk.functions.isInsideFunction() &&
                 this->state.declaration.modules.findDeclModule(classDecl) ==
                     this->state.declaration.modules.getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextClassOrder::usedEarly(classDecl->name);

    return err(CodeClassesRegistryClassDeclaredError{
        .code = CodeClassesRegistryClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool ClassMemberRules::isFittingCallback(const FuncSymbol *function,
                                         const Signature *callback) const {
  if (callback == nullptr) {
    return false;
  }
  size_t given = callback->params.size();
  bool isCountFitting =
      !function->hasRest && function->minArgs <= given && function->params.size() <= given;
  if (!isCountFitting) {
    return false;
  }
  for (size_t index = 0; index < function->params.size(); ++index) {
    const Type *param = function->params[index];
    const Type *passed = callback->params[index];
    bool isSame = param == passed || (param->isOptional() && param->inner() == passed);
    if (!isSame) {
      return false;
    }
  }

  return true;
}

std::string ClassMemberRules::signatureText(const FunctionDecl *variant,
                                            const Type *returned) const {
  FuncSymbol *function = variant->sym;
  size_t count = std::min(variant->params.size(), function->params.size());
  std::string text = "(";
  for (size_t index = 0; index < count; ++index) {
    if (index > 0) {
      text += ", ";
    }
    text += variant->params[index].label().str() + ": " + function->params[index]->name().str();
  }

  return text + ") => " + returned->name().str();
}

std::string ClassMemberRules::shownObject(const MemberExpr *member) const {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (name == nullptr) {
    return kObjectPlaceholder.str();
  }

  return name->name.str();
}

ClassMemberRules::OwnedStaticField ClassMemberRules::staticField(ClassDecl *classDecl,
                                                                 llvm::StringRef name) const {
  for (ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    ClassDecl *owner = current->staticOwner();
    auto *field = this->ownStaticField(owner, name);
    if (field != nullptr) {
      return OwnedStaticField{.field = field, .owner = owner};
    }
  }

  return OwnedStaticField{.field = nullptr, .owner = nullptr};
}

ClassField *ClassMemberRules::ownStaticField(ClassDecl *owner, llvm::StringRef name) const {
  for (ClassField &field : owner->fields) {
    if (field.isStatic() && field.name == name && field.var() != nullptr) {
      return &field;
    }
  }

  return nullptr;
}

}
