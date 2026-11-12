#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_checker/class_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/declare/module_declarer/module_declarer.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kAnonymousClassName = "(anonymous class)";

}

LateClasses::LateClasses(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classBases = &all.classBases;
  this->classLinks = &all.classLinks;
  this->fsDeclarer = &all.fsDeclarer;
  this->typeParams = &all.typeParams;
  this->exprChecker = &all.exprChecker;
  this->classChecker = &all.classChecker;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->moduleDeclarer = &all.moduleDeclarer;
  this->reflectChecker = &all.reflectChecker;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->decoratorChecker = &all.decoratorChecker;
  this->classValueChecker = &all.classValueChecker;
  this->genericInstantiator = &all.genericInstantiator;
}

bool LateClasses::isHoldingType(const Type *type, const Type *target) const {
  auto parts = TypeParts::collect(type, kHoldsRoute);

  return llvm::is_contained(parts.getTypes(), target);
}

Expr *LateClasses::checkClassExpr(ClassExpr *expr) {
  ClassDecl *classDecl = expr->decl;
  if (classDecl->type == nullptr) {
    this->makeLateClass(classDecl);
  }
  classDecl->usedAsValue = true;
  if (!classDecl->declared) {
    this->walkStaticFields(classDecl);
    this->walkClassDecorators(classDecl);
  }
  expr->type = this->classValueChecker->classValueType(classDecl);

  return expr;
}

void LateClasses::walkClassDecorators(ClassDecl *classDecl) {
  auto decorated = this->classChecker->hasDecorators(classDecl);
  if (decorated.isErr()) {
    return;
  }
  this->state.getClassContext().enterClass(classDecl);
  this->decoratorChecker->decorateClass(classDecl);
  this->state.getClassContext().leaveClass();
}

LateClasses::ResultBaseClassUsable LateClasses::baseClass(const Type *type) const {
  auto *parent = const_cast<ClassDecl *>(type->classRef());
  if (type->isError()) {
    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (type->isConstructor()) {
    auto message = ErrorTextBaseClass::atRun(type->name());

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::KNOWN_ONLY_AT_RUN,
        .message = message,
    });
  }
  if (!type->isClassRef()) {
    auto message = ErrorTextBaseClass::notClass(type->name());

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  if (parent->type == nullptr) {
    auto message = ErrorTextBaseClass::javascript(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (parent->isGeneric()) {
    auto message = ErrorTextBaseClass::generic(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::GENERIC_CLASS,
        .message = message,
    });
  }
  if (this->fsDeclarer->isRuntimeClass(parent)) {
    auto message = ErrorTextBaseClass::madeByFs(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::MADE_BY_FS,
        .message = message,
    });
  }

  return ok(parent);
}

void LateClasses::makeLateClass(ClassDecl *classDecl) {
  classDecl->checkOnly = this->reflectChecker->isOnlyChecked();
  this->registerLateClass(classDecl);
  this->rememberLateScope(classDecl);
  this->linkLateBase(classDecl);
  this->nameAnonymousClass(classDecl);
  this->declareLateMembers(classDecl);
  this->rememberMethodModules(classDecl);
  this->markClassOverrides(classDecl);
  if (this->state.getClassStore().hasLinkedClasses()) {
    this->classLinks->linkOverrides(classDecl);
    this->classLinks->buildVtable(classDecl);
  }
  auto &late = this->state.getLateClassStore();
  auto *instance = this->state.getGenericStore().getCheckingInstance();
  if (instance != nullptr && instance->generic != nullptr) {
    late.rememberLateOwner(classDecl, instance);
  }
  late.addLateClass(classDecl);
}

void LateClasses::registerLateClass(ClassDecl *classDecl) {
  if (classDecl->type == nullptr) {
    classDecl->id = this->state.getClassStore().takeClassId();
    llvm::StringRef typeName = classDecl->name.empty() ? kAnonymousClassName : classDecl->name;
    classDecl->type = Type::classType(typeName, classDecl);
    this->state.getClassStore().addClass(classDecl);
  }
  auto *current = this->state.getModuleStates().getCurrentModule();
  if (current != nullptr && !this->state.getModuleStates().hasDeclModule(classDecl)) {
    this->state.getModuleStates().rememberDeclModule(classDecl, current);
  }
}

void LateClasses::rememberLateScope(ClassDecl *classDecl) {
  const auto *outer = this->innerGenericCopy();
  if (outer != nullptr) {
    classDecl->outer = outer;
  }
  for (llvm::StringRef local : this->state.getScopeStack().getLocalVarNames()) {
    classDecl->locals.push_back(local);
  }
}

const FunctionDecl *LateClasses::innerGenericCopy() const {
  auto functions = this->state.getFunctionStack().getFunctions();
  for (const FunctionDecl *functionDecl : llvm::reverse(functions)) {
    if (functionDecl->isInstance && !functionDecl->typeArgs.empty()) {
      return functionDecl;
    }
  }

  return nullptr;
}

void LateClasses::linkLateBase(ClassDecl *classDecl) {
  if (classDecl->baseExpr == nullptr) {
    return;
  }
  classDecl->baseExpr = this->exprChecker->checkExpr(classDecl->baseExpr);
  auto base = this->baseClass(classDecl->baseExpr->type);
  bool isReported =
      base.isErr() && base.error().code == LateClassesBaseClassErrorCode::ALREADY_REPORTED;
  if (base.isErr() && !isReported) {
    this->state.getDiagnostics().error(classDecl->baseExpr->loc(), base.error().message);
  }
  if (base.isOk() && base.value() != nullptr) {
    this->attachLateParent(classDecl, base.value());
  }
  classDecl->base.reset();
}

void LateClasses::attachLateParent(ClassDecl *classDecl, ClassDecl *parent) {
  classDecl->parent = parent;
  if (classDecl->checkOnly) {
    const_cast<Type *>(classDecl->type)->addBase(parent->type);
    return;
  }
  parent->children.push_back(classDecl);
  this->classBases->addBase(classDecl->type, parent->type);
}

void LateClasses::nameAnonymousClass(ClassDecl *classDecl) const {
  if (!classDecl->name.empty() || classDecl->parent == nullptr) {
    return;
  }
  std::string extendsName = "(class extends " + classDecl->parent->type->name().str() + ")";
  const_cast<Type *>(classDecl->type)->rename(extendsName);
}

void LateClasses::declareLateMembers(ClassDecl *classDecl) {
  auto &late = this->state.getLateClassStore();
  late.enterDeclaringClass(classDecl);
  this->state.getClassContext().enterClass(classDecl);
  this->classDeclarer->resolveClass(classDecl);
  this->classDeclarer->declareMethods(classDecl);
  this->state.getClassContext().leaveClass();
  late.leaveDeclaringClass();
}

void LateClasses::rememberMethodModules(const ClassDecl *classDecl) {
  auto *home = this->state.getModuleStates().findDeclModule(classDecl);
  if (home == nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->classDeclarer->memberFunctions(classDecl)) {
    this->state.getModuleStates().rememberDeclModule(functionDecl, home);
  }
}

LateClasses::ResultStaticTypeKnown LateClasses::staticType(const ClassField &field,
                                                           const Type *type) const {
  if (type->isVoid() || type->isUndefined()) {
    auto message = ErrorTextStaticType::unknown(field.name);

    return err(LateClassesStaticTypeError{
        .code = LateClassesStaticTypeErrorCode::UNKNOWN_FROM_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

void LateClasses::walkStaticFields(ClassDecl *classDecl) {
  auto &classContext = this->state.getClassContext();
  classContext.enterClass(classDecl);
  classContext.enterStaticInit();
  classDecl->declared = true;
  classContext.forgetSetStatics();
  for (ClassField &field : classDecl->fields) {
    if (field.isStatic() && field.var() != nullptr) {
      this->walkStaticField(field);
    }
  }
  classContext.leaveStaticInit();
  classContext.leaveClass();
}

void LateClasses::walkStaticField(ClassField &field) {
  VarSymbol *variable = field.var();
  if (field.init == nullptr) {
    if (variable->type == nullptr) {
      variable->type = Type::error();
    }
    this->state.getClassContext().markStaticSet(variable);
    return;
  }
  const Type *declared = this->declaredStaticType(field);
  const Type *expected = declared != nullptr && !declared->isError() ? declared : nullptr;
  field.init = this->exprChecker->checkExpr(field.init, expected);
  this->state.getClassContext().markStaticSet(variable);
  if (declared != nullptr) {
    auto label = ErrorTextStaticType::fieldLabel(field.name);
    field.init = this->typeRelations->coerce(field.init, declared, label);
    return;
  }
  auto known = this->staticType(field, field.init->type);
  if (known.isErr()) {
    this->state.getDiagnostics().error(field.init->loc(), known.error().message);
  }
  const Type *type = known.isOk() ? known.value() : Type::error();
  field.resolved = type;
  variable->type = type;
}

const Type *LateClasses::declaredStaticType(const ClassField &field) const {
  const Type *declared = field.var()->type;
  bool isFromValue = field.resolved != nullptr && !field.type.has_value() && !field.optional();
  if (declared != nullptr && isFromValue) {
    return nullptr;
  }

  return declared;
}

void LateClasses::walkLateClasses() {
  auto &late = this->state.getLateClassStore();
  while (late.hasUncheckedLateClasses()) {
    auto *classDecl = late.takeUncheckedLateClass();
    if (!classDecl->checkOnly) {
      this->walkLateClass(classDecl);
    }
  }
  for (const LateError &error : late.getFirstLateErrors()) {
    if (!error.instance->failed) {
      this->reportLateError(error);
    }
  }
}

void LateClasses::walkLateClass(ClassDecl *classDecl) {
  auto &late = this->state.getLateClassStore();
  auto *home = this->state.getModuleStates().findDeclModule(classDecl);
  if (home != nullptr && this->state.getScopeStack().isTopLevel()) {
    this->moduleDeclarer->useModule(home);
  }
  auto *owner = late.findLateOwner(classDecl);
  auto *outerInstance = this->state.getGenericStore().getCheckingInstance();
  auto &diagnostics = this->state.getDiagnostics();
  if (owner != nullptr) {
    this->state.getGenericStore().rememberCheckingInstance(owner);
    diagnostics.pushList(DiagnosticRoute::OWNER);
  }
  this->state.getFlowState().replaceFlow(Flow());
  this->classChecker->walkClassBodies(classDecl);
  this->state.getGenericStore().rememberCheckingInstance(outerInstance);
  if (owner != nullptr) {
    auto classDiagnostics = diagnostics.popList();
    late.addLateErrors(owner, classDiagnostics);
  }
}

void LateClasses::reportLateError(const LateError &error) {
  InstanceRecord *instance = error.instance;
  instance->failed = true;
  instance->message = error.first.message;
  instance->errorLoc = error.first.loc;
  this->genericInstantiator->reportInstance(instance, instance->callLoc, instance->concrete);
}

void LateClasses::rememberEmittedLateClasses() {
  for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
    bool isMadeLate = classDecl->expression() && classDecl->type != nullptr;
    bool isLate = classDecl->generic() != nullptr || isMadeLate;
    if (isLate && classDecl->isEmitted()) {
      this->state.getInventoryStore().addLateClass(classDecl);
    }
  }
}

void LateClasses::noteMethodNames(const ClassDecl *classDecl) {
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->isStatic()) {
      this->state.getLateClassStore().addLateMethodName(functionDecl->name);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    bool isArrowMethod =
        !field.isStatic() && field.init != nullptr && isa<FunctionExpr>(field.init);
    if (isArrowMethod) {
      this->state.getLateClassStore().addLateMethodName(field.name);
    }
  }
}

void LateClasses::markLateOverrides(SourceFile &program) {
  for (Stmt *item : program.items) {
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (classDecl != nullptr && classDecl->baseExpr != nullptr) {
      this->noteMethodNames(classDecl);
    }
  }
  auto nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *classExpr = dyn_cast<ClassExpr>(placed.expr);
    if (classExpr != nullptr && this->isExtending(classExpr->decl)) {
      this->noteMethodNames(classExpr->decl);
    }
  }
  for (ClassDecl *classDecl : this->state.getClassStore().getClasses()) {
    this->markClassOverrides(classDecl);
  }
}

bool LateClasses::isExtending(const ClassDecl *classDecl) const {
  return classDecl->baseExpr != nullptr || classDecl->base.has_value();
}

void LateClasses::markClassOverrides(ClassDecl *classDecl) {
  auto &late = this->state.getLateClassStore();
  if (!late.hasLateMethodNames() || classDecl->literal() != nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->classLinks->ownMethods(classDecl)) {
    Overriding *overriding = functionDecl->overriding();
    if (late.isLateMethodName(functionDecl->name) && overriding != nullptr) {
      overriding->overridden = true;
      overriding->openToLate = true;
    }
  }
}

void LateClasses::bindOuterTypeParams(const ClassDecl *classDecl) {
  const FunctionDecl *outer = classDecl != nullptr ? classDecl->outer : nullptr;
  if (outer != nullptr) {
    this->typeParams->bindTypeParams(outer->typeParams, outer->typeArgs);
  }
}

bool LateClasses::hasClassExpr(const FunctionDecl *functionDecl) {
  if (functionDecl == nullptr || functionDecl->body == nullptr) {
    return false;
  }
  auto &late = this->state.getLateClassStore();
  if (late.isClassExprKnown(functionDecl)) {
    return late.hasClassExpr(functionDecl);
  }
  auto nodes = PlacedNodes::collect({functionDecl->body}, {});
  bool hasClass = this->hasClassExprIn(nodes);
  late.rememberClassExpr(functionDecl, hasClass);

  return hasClass;
}

bool LateClasses::hasClassExprIn(const PlacedNodes &nodes) const {
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (isa<ClassExpr>(placed.expr)) {
      return true;
    }
  }

  return false;
}

bool LateClasses::isTemplate(const FuncSymbol *generic) {
  if (generic == nullptr || !generic->isGeneric() || generic->decl == nullptr) {
    return false;
  }

  return !generic->decl->isInstance && this->hasClassExpr(generic->decl);
}

void LateClasses::walkTemplateInstance(InstanceRecord *instance) {
  instance->checking = true;
  auto &diagnostics = this->state.getDiagnostics();
  FunctionDecl *functionDecl = instance->decl;
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  diagnostics.pushList(DiagnosticRoute::OWNER);
  if (!this->state.getModuleStates().isTopLevelDone()) {
    this->state.getFunctionStack().rememberEarlyFunction(functionDecl->sym);
  }
  this->state.getFlowState().replaceFlow(Flow());
  if (functionDecl->sym->hasReturnType()) {
    this->functionChecker->walkFunctionBody(functionDecl);
  } else {
    this->returnInferrer->inferFromBody(functionDecl);
  }
  auto instanceDiagnostics = diagnostics.popList();
  this->returnInferrer->leaveBody(saved);
  instance->checking = false;
  instance->checked = true;
  const auto *firstError = diagnostics.findFirstError(instanceDiagnostics);
  if (firstError == nullptr) {
    return;
  }
  instance->failed = true;
  instance->message = firstError->message;
  instance->errorLoc = firstError->loc;
}

LateClasses::ResultNoOldObjectHeld LateClasses::oldObject(const ClassDecl *classDecl,
                                                          const ClassField &field) const {
  bool isTyped = field.isStatic() && field.var() != nullptr && field.var()->type != nullptr;
  bool isHolding = isTyped && this->isHoldingType(field.var()->type, classDecl->type);
  if (isHolding) {
    auto message = ErrorTextOldObject::held(classDecl->name, field.name);

    return err(LateClassesOldObjectError{
        .code = LateClassesOldObjectErrorCode::HOLDS_OLD_OBJECT,
        .message = message,
    });
  }

  return ok(&field);
}

LateClasses::ResultFieldNameIsFree LateClasses::replacedField(const ClassDecl *classDecl,
                                                              const ClassDecl *replacement,
                                                              const ClassDecl *child,
                                                              const ClassField &field) const {
  bool isClash = !field.isStatic() && replacement->field(field.name) != nullptr;
  if (isClash) {
    auto message = ErrorTextReplacedField::clash(child->name, classDecl->name, field.name);

    return err(LateClassesReplacedFieldError{
        .code = LateClassesReplacedFieldErrorCode::FIELD_IN_REPLACEMENT,
        .message = message,
    });
  }

  return ok(&field);
}

void LateClasses::relink(ClassDecl *classDecl) {
  this->relinkClass(classDecl);
  for (ClassDecl *below : ClassTree::descendants(classDecl)) {
    this->relinkClass(below);
  }
}

void LateClasses::relinkClass(ClassDecl *classDecl) {
  const_cast<Type *>(classDecl->type)->forgetFields();
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  this->classLinks->linkOverrides(classDecl);
  this->state.getDiagnostics().popList();
  this->classLinks->buildVtable(classDecl);
}

void LateClasses::replaceClass(ClassDecl *classDecl, ClassDecl *replacement, llvm::SMLoc loc) {
  for (const ClassField &field : classDecl->fields) {
    auto kept = this->oldObject(classDecl, field);
    if (kept.isErr()) {
      auto here = ErrorTextOldObject::decorator();
      this->state.getDiagnostics().error(field.loc, kept.error().message);
      this->state.getDiagnostics().note(loc, here);
      return;
    }
  }

  this->swapTypes(classDecl, replacement);
  auto declared = this->declaredChildren(classDecl, replacement);
  for (ClassDecl *child : declared) {
    for (const ClassField &field : child->fields) {
      auto unique = this->replacedField(classDecl, replacement, child, field);
      if (unique.isErr()) {
        this->state.getDiagnostics().error(field.loc, unique.error().message);
        return;
      }
    }
    this->moveChild(classDecl, replacement, child);
  }
  classDecl->replacement = replacement;
  replacement->usedAsValue = true;
}

void LateClasses::swapTypes(ClassDecl *classDecl, ClassDecl *replacement) {
  auto *mine = const_cast<Type *>(classDecl->type);
  auto *theirs = const_cast<Type *>(replacement->type);
  std::string name = mine->name().str();
  Type::swapClasses(mine, theirs);
  classDecl->type = theirs;
  replacement->type = mine;
  theirs->rename(name + " (as declared)");
  this->state.getInventoryStore().addSubtype(mine);
}

std::vector<ClassDecl *> LateClasses::declaredChildren(const ClassDecl *classDecl,
                                                       const ClassDecl *replacement) {
  auto *declaredType = const_cast<Type *>(classDecl->type);
  auto *replacementType = const_cast<Type *>(replacement->type);
  auto declared = std::vector<ClassDecl *>();
  for (ClassDecl *child : classDecl->children) {
    if (child == replacement) {
      continue;
    }
    if (replacement->derivesFrom(child)) {
      const_cast<Type *>(child->type)->replaceBase(replacementType, declaredType);
      continue;
    }
    declared.push_back(child);
  }

  return declared;
}

void LateClasses::moveChild(ClassDecl *classDecl, ClassDecl *replacement, ClassDecl *child) {
  llvm::erase(classDecl->children, child);
  replacement->children.push_back(child);
  child->parent = replacement;
  this->relink(child);
}

}
