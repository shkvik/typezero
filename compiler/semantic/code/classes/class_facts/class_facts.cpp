#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/program/library/builtin_names/access_keyword_table/access_keyword_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

ClassFacts::ClassFacts(SemanticState &owner)
    : state(owner) {}

const Type *ClassFacts::valueTypeOf(ClassDecl *classDecl) const {
  auto *finalDecl = this->finalClass(classDecl);

  return this->classRefType(finalDecl);
}

ClassFacts::ResultInitThisFound ClassFacts::isInitThis(const Expr *expr) const {
  const Expr *plain = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(plain);
  bool isInConstructor =
      this->state.walk.self.getInitializerThis() != nullptr &&
      this->state.walk.functions.getCurrentDecl() == this->state.walk.self.getInitializer();
  if (!isInConstructor) {
    auto message = ErrorTextInitThis::outside();

    return err(CodeClassesRegistryIsInitThisError{
        .code = CodeClassesRegistryIsInitThisErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  if (name == nullptr || name->var() != this->state.walk.self.getInitializerThis()) {
    auto message = ErrorTextInitThis::notThis();

    return err(CodeClassesRegistryIsInitThisError{
        .code = CodeClassesRegistryIsInitThisErrorCode::NOT_THIS,
        .message = message,
    });
  }

  return ok(name);
}

ClassFacts::ResultObjectIsReady ClassFacts::checkThisReady(const Flow &flow) const {
  auto unset = this->firstUnset(flow);
  if (flow.beforeSuper) {
    auto message = ErrorTextThisReady::noSuper();

    return err(CodeClassesRegistryThisReadyError{
        .code = CodeClassesRegistryThisReadyErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  if (unset.isOk()) {
    auto message = ErrorTextThisReady::unset(unset.value());

    return err(CodeClassesRegistryThisReadyError{
        .code = CodeClassesRegistryThisReadyErrorCode::FIELD_NOT_SET,
        .message = message,
    });
  }

  return ok(&flow);
}

ClassFacts::ResultFieldUnsetYet ClassFacts::firstUnset(const Flow &flow) const {
  if (this->state.walk.self.getInitializer() == nullptr) {
    auto message = ErrorTextFirstUnset::outside();

    return err(CodeClassesRegistryFirstUnsetError{
        .code = CodeClassesRegistryFirstUnsetErrorCode::NOT_IN_CONSTRUCTOR,
        .message = message,
    });
  }
  const ClassDecl *initialized = this->state.walk.self.getInitializer()->cls();
  const std::vector<ClassField> &fields = initialized->fields;
  auto unset =
      llvm::find_if(fields, std::bind_front(&ClassFacts::isUnsetField, this, std::cref(flow)));
  if (unset == fields.end()) {
    auto message = ErrorTextFirstUnset::allSet(initialized->name);

    return err(CodeClassesRegistryFirstUnsetError{
        .code = CodeClassesRegistryFirstUnsetErrorCode::ALL_FIELDS_SET,
        .message = message,
    });
  }

  return ok(unset->name.str());
}

bool ClassFacts::isUnsetField(const Flow &flow, const ClassField &field) const {
  return !field.isStatic() && field.method() == nullptr && !field.optional() &&
         !llvm::is_contained(flow.setFields, field.name);
}

std::vector<FunctionDecl *> ClassFacts::constructorsOf(const ClassDecl *classDecl) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (!current->constructors.empty()) {
      return current->constructors;
    }
  }

  return {};
}

std::vector<FunctionDecl *> ClassFacts::ownMethods(const ClassDecl *classDecl) const {
  auto methods = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->isStatic()) {
      methods.push_back(functionDecl);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

bool ClassFacts::isOverridingTransitively(const FunctionDecl *functionDecl,
                                          const FunctionDecl *base) const {
  for (const FunctionDecl *current = functionDecl; current != nullptr;
       current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

FunctionDecl *ClassFacts::soleImplementation(const ClassDecl *classDecl,
                                             FunctionDecl *variant) const {
  if (variant->openToLate()) {
    return nullptr;
  }
  if (!variant->overridden() && !variant->isAbstract()) {
    return variant;
  }
  if (variant->generic != nullptr && variant->overridden()) {
    return nullptr;
  }
  auto implementations = this->implementationsBelow(classDecl, variant);
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only != nullptr ? 1 : 0;
  for (const OwnedMethod &implementation : implementations) {
    only = implementation.method;
    count += implementation.owner->isEmitted() ? 1 : 2;
  }

  return count == 1 ? only : nullptr;
}

std::vector<ClassFacts::OwnedMethod>
ClassFacts::implementationsBelow(const ClassDecl *classDecl, const FunctionDecl *variant) const {
  auto implementations = std::vector<OwnedMethod>();
  if (classDecl == nullptr) {
    return implementations;
  }
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    this->addImplementations(child, variant, implementations);
  }

  return implementations;
}

void ClassFacts::addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                                    std::vector<OwnedMethod> &implementations) const {
  for (FunctionDecl *functionDecl : this->ownMethods(child)) {
    bool isImplementing = !functionDecl->isAbstract() && functionDecl != variant &&
                          this->isOverridingTransitively(functionDecl, variant);
    if (isImplementing) {
      implementations.push_back(OwnedMethod{.method = functionDecl, .owner = child});
    }
  }
}

bool ClassFacts::isSameClass(const ClassDecl *first, const ClassDecl *second) const {
  return first != nullptr && second != nullptr && first->staticOwner() == second->staticOwner();
}

bool ClassFacts::isDerivedFrom(const ClassDecl *from, const ClassDecl *base) const {
  for (const ClassDecl *current = from; current != nullptr; current = current->parent) {
    if (this->isSameClass(current, base)) {
      return true;
    }
  }

  return false;
}

ClassFacts::ResultMemberAccessible ClassFacts::checkAccess(const ClassDecl *classDecl,
                                                           Access access, llvm::StringRef name,
                                                           const Expr *receiver) {
  const ClassDecl *currentClass = this->state.walk.self.getCurrentClass();
  if (access == Access::Public || this->isSameClass(currentClass, classDecl)) {
    return ok(classDecl);
  }
  if (access == Access::Protected && this->isDerivedFrom(currentClass, classDecl)) {
    return this->checkProtectedReceiver(classDecl, name, receiver);
  }
  auto word = AccessKeywordTable::getName(access);
  if (access == Access::Protected) {
    auto message = ErrorTextAccess::hiddenProtected(name, word, classDecl->name);

    return err(CodeClassesRegistryCheckAccessError{
        .code = CodeClassesRegistryCheckAccessErrorCode::PROTECTED_MEMBER,
        .message = message,
    });
  }
  auto message = ErrorTextAccess::hiddenPrivate(name, word, classDecl->name);

  return err(CodeClassesRegistryCheckAccessError{
      .code = CodeClassesRegistryCheckAccessErrorCode::PRIVATE_MEMBER,
      .message = message,
  });
}

ClassFacts::ResultMemberAccessible ClassFacts::checkProtectedReceiver(const ClassDecl *classDecl,
                                                                      llvm::StringRef name,
                                                                      const Expr *receiver) const {
  const ClassDecl *currentClass = this->state.walk.self.getCurrentClass();
  const Expr *uncast = receiver != nullptr ? ExprParts::withoutCasts(receiver) : nullptr;
  auto *self = dyn_cast_or_null<NameExpr>(uncast);
  const ClassDecl *other =
      uncast != nullptr && uncast->type != nullptr ? this->classOf(uncast->type) : nullptr;
  bool isSelf = self != nullptr && (self->name == KeywordTable::getName(Keyword::THIS) ||
                                    self->name == KeywordTable::getName(Keyword::SUPER));
  bool isReachable =
      uncast == nullptr || isSelf || other == nullptr || this->isDerivedFrom(other, currentClass);
  if (isReachable) {
    return ok(classDecl);
  }
  auto message = ErrorTextAccess::elsewhere(name, classDecl->name, currentClass->name, other->name);

  return err(CodeClassesRegistryCheckAccessError{
      .code = CodeClassesRegistryCheckAccessErrorCode::PROTECTED_ELSEWHERE,
      .message = message,
  });
}

ClassDecl *ClassFacts::finalClass(ClassDecl *classDecl) const {
  ClassDecl *current = classDecl;
  while (current != nullptr && current->replacement != nullptr) {
    current = current->replacement;
  }

  return current;
}

const Type *ClassFacts::classRefType(const ClassDecl *classDecl) const {
  bool isAnonymous = classDecl->name.empty() && classDecl->type != nullptr;
  llvm::StringRef name = isAnonymous ? classDecl->type->name() : classDecl->name;

  return Type::classRef(classDecl, name);
}

std::vector<FunctionDecl *> ClassFacts::signatureVariants(const Type *interfaceType,
                                                          const Overload &overload) {
  auto &classes = this->state.declaration.classes;
  auto known = this->state.code.overloads.getSignatureVariants(interfaceType, overload.name);
  if (!known.empty()) {
    return known;
  }
  auto variants = std::vector<FunctionDecl *>();
  for (const Type *signature : overload.signatures) {
    auto *variant = this->signatureVariant(overload, signature);
    variants.push_back(variant);
  }
  this->state.code.overloads.rememberSignatureVariants(interfaceType, overload.name, variants);

  return variants;
}

FunctionDecl *ClassFacts::signatureVariant(const Overload &overload, const Type *signature) {
  auto name = this->state.ast.keepName(overload.name);
  auto *functionDecl = this->state.ast.make<FunctionDecl>(llvm::SMLoc(), name);
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->decl = functionDecl;
  function->params.assign(signature->params().begin(), signature->params().end());
  function->minArgs = signature->minArgs();
  function->hasRest = signature->hasRest();
  function->setReturnType(signature->ret());
  for (size_t index = 0; index < signature->params().size(); ++index) {
    auto param = this->signatureParam(signature, index);
    functionDecl->params.push_back(param);
  }
  functionDecl->sym = function;

  return functionDecl;
}

Param ClassFacts::signatureParam(const Type *signature, size_t index) {
  auto letter = std::string(1, char('a' + index % 26));
  auto param = Param();
  param.name = this->state.ast.keepName(letter);
  param.declaredType = signature->params()[index];
  bool isRest = signature->hasRest() && index + 1 == signature->params().size();
  if (isRest) {
    param.form = RestParam{};
  } else if (index >= signature->minArgs()) {
    param.form = OptionalParam{};
  }

  return param;
}

int ClassFacts::signatureIndex(llvm::ArrayRef<FunctionDecl *> variants,
                               const FuncSymbol *function) const {
  auto found = llvm::find(variants, function->decl);

  return int(found - variants.begin());
}

ClassDecl *ClassFacts::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
