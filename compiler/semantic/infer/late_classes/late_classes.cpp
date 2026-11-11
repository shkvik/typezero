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

bool LateClasses::holdsType(const Type *type, const Type *target) {
  TypeParts parts = TypeParts::collect(type, kHoldsRoute);

  return llvm::is_contained(parts.getTypes(), target);
}

Expr *LateClasses::checkClassExpr(ClassExpr *expr, const Type *expected) {
  (void)expected;
  ClassDecl *cls = expr->decl;
  if (!cls->type) {
    this->makeLateClass(cls);
  }
  cls->usedAsValue = true;
  if (!cls->declared) {
    this->checkStaticFields(cls);
    ClassChecker::ResultHasDecorators decorated = this->classChecker->hasDecorators(cls);
    if (decorated.isOk()) {
      this->state.getClassContext().enterClass(cls);
      this->decoratorChecker->decorateClass(cls);
      this->state.getClassContext().leaveClass();
    }
  }
  expr->type = this->classValueChecker->classValueType(cls);

  return expr;
}

LateClasses::ResultBaseClassUsable LateClasses::baseClass(const Type *type) const {
  ClassDecl *parent = const_cast<ClassDecl *>(type->classRef());
  if (type->isError()) {
    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (type->isConstructor()) {
    std::string message = ErrorTextBaseClass::atRun(type->name());

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::KNOWN_ONLY_AT_RUN,
        .message = message,
    });
  }
  if (!type->isClassRef()) {
    std::string message = ErrorTextBaseClass::notClass(type->name());

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }
  if (!parent->type) {
    std::string message = ErrorTextBaseClass::javascript(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (parent->isGeneric()) {
    std::string message = ErrorTextBaseClass::generic(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::GENERIC_CLASS,
        .message = message,
    });
  }
  if (this->fsDeclarer->isRuntimeClass(parent)) {
    std::string message = ErrorTextBaseClass::madeByFs(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::MADE_BY_FS,
        .message = message,
    });
  }

  return ok(parent);
}

void LateClasses::makeLateClass(ClassDecl *cls) {
  cls->checkOnly = this->reflectChecker->onlyChecked();
  this->registerLateClass(cls);
  this->rememberLateScope(cls);
  this->linkLateBase(cls);
  LateClasses::nameAnonymousClass(cls);
  this->declareLateMembers(cls);
  this->rememberMethodModules(cls);
  this->markLateOverrides(cls);
  if (this->state.getClassStore().hasLinkedClasses()) {
    this->classLinks->linkOverrides(cls);
    this->classLinks->buildVtable(cls);
  }
  LateClassStore &late = this->state.getLateClassStore();
  InstanceRecord *instance = this->state.getGenericStore().getCheckingInstance();
  if (instance != nullptr && instance->generic != nullptr) {
    late.rememberLateOwner(cls, instance);
  }
  late.addLateClass(cls);
}

void LateClasses::registerLateClass(ClassDecl *cls) {
  if (cls->type == nullptr) {
    cls->id = this->state.getClassStore().takeClassId();
    llvm::StringRef typeName = cls->name.empty() ? "(anonymous class)" : cls->name;
    cls->type = Type::classType(typeName, cls);
    this->state.getClassStore().addClass(cls);
  }
  Module *current = this->state.getModuleStates().getCurrentModule();
  if (current != nullptr && !this->state.getModuleStates().hasDeclModule(cls)) {
    this->state.getModuleStates().rememberDeclModule(cls, current);
  }
}

void LateClasses::rememberLateScope(ClassDecl *cls) {
  std::vector<FunctionDecl *> functions = this->state.getFunctionStack().getFunctions();
  for (FunctionDecl *fn : llvm::reverse(functions)) {
    if (fn->isInstance && !fn->typeArgs.empty()) {
      cls->outer = fn;
      break;
    }
  }
  for (llvm::StringRef local : this->state.getScopeStack().getLocalVarNames()) {
    cls->locals.push_back(local);
  }
}

void LateClasses::linkLateBase(ClassDecl *cls) {
  if (cls->baseExpr == nullptr) {
    return;
  }
  cls->baseExpr = this->exprChecker->checkExpr(cls->baseExpr);
  ResultBaseClassUsable base = this->baseClass(cls->baseExpr->type);
  bool reported =
      base.isErr() && base.error().code == LateClassesBaseClassErrorCode::ALREADY_REPORTED;
  if (base.isErr() && !reported) {
    this->state.getDiagnostics().error(cls->baseExpr->loc(), base.error().message);
  }
  ClassDecl *parent = base.isOk() ? base.value() : nullptr;
  if (parent != nullptr && cls->checkOnly) {
    cls->parent = parent;
    const_cast<Type *>(cls->type)->addBase(parent->type);
  } else if (parent != nullptr) {
    cls->parent = parent;
    parent->children.push_back(cls);
    this->classBases->addBase(cls->type, parent->type);
  }
  cls->base.reset();
}

void LateClasses::nameAnonymousClass(ClassDecl *cls) {
  if (!cls->name.empty() || cls->parent == nullptr) {
    return;
  }
  std::string extendsName = "(class extends " + cls->parent->type->name().str() + ")";
  const_cast<Type *>(cls->type)->rename(extendsName);
}

void LateClasses::declareLateMembers(ClassDecl *cls) {
  LateClassStore &late = this->state.getLateClassStore();
  late.enterDeclaringClass(cls);
  this->state.getClassContext().enterClass(cls);
  this->classDeclarer->resolveClass(cls);
  this->classDeclarer->declareMethods(cls);
  this->state.getClassContext().leaveClass();
  late.leaveDeclaringClass();
}

void LateClasses::rememberMethodModules(const ClassDecl *cls) {
  Module *home = this->state.getModuleStates().findDeclModule(cls);
  if (home == nullptr) {
    return;
  }
  std::vector<FunctionDecl *> methods = cls->methods;
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init != nullptr) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method() != nullptr) {
      methods.push_back(f.method());
    }
  }
  for (FunctionDecl *fn : methods) {
    this->state.getModuleStates().rememberDeclModule(fn, home);
  }
}

LateClasses::ResultStaticTypeKnown LateClasses::staticType(const ClassField &field,
                                                           const Type *type) {
  if (type->isVoid() || type->isUndefined()) {
    std::string message = ErrorTextStaticType::unknown(field.name);

    return err(LateClassesStaticTypeError{
        .code = LateClassesStaticTypeErrorCode::UNKNOWN_FROM_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

void LateClasses::checkStaticFields(ClassDecl *cls) {
  ClassContext &classContext = this->state.getClassContext();
  classContext.enterClass(cls);
  classContext.enterStaticInit();
  cls->declared = true;
  classContext.forgetSetStatics();
  for (ClassField &f : cls->fields) {
    if (!f.isStatic() || !f.var()) {
      continue;
    }
    if (!f.init) {
      if (!f.var()->type) {
        f.var()->type = Type::error();
      }
      classContext.markStaticSet(f.var());
      continue;
    }
    const Type *declared = f.var()->type;
    if (declared && f.resolved && !f.type && !f.optional()) {
      declared = nullptr;
    }
    const Type *expected = declared && !declared->isError() ? declared : nullptr;
    f.init = this->exprChecker->checkExpr(f.init, expected);
    classContext.markStaticSet(f.var());
    if (declared) {
      std::string label = ErrorTextStaticType::fieldLabel(f.name);
      f.init = this->typeRelations->coerce(f.init, declared, label);
      continue;
    }
    ResultStaticTypeKnown known = LateClasses::staticType(f, f.init->type);
    if (known.isErr()) {
      this->state.getDiagnostics().error(f.init->loc(), known.error().message);
    }
    const Type *type = known.isOk() ? known.value() : Type::error();
    f.resolved = type;
    f.var()->type = type;
  }
  classContext.leaveStaticInit();
  classContext.leaveClass();
}

void LateClasses::checkLateClasses() {
  LateClassStore &late = this->state.getLateClassStore();
  while (late.hasUncheckedLateClasses()) {
    ClassDecl *cls = late.takeUncheckedLateClass();
    if (cls->checkOnly) {
      continue;
    }
    Module *home = this->state.getModuleStates().findDeclModule(cls);
    if (home && this->state.getScopeStack().isTopLevel()) {
      this->moduleDeclarer->useModule(home);
    }
    InstanceRecord *owner = late.findLateOwner(cls);
    InstanceRecord *outerInstance = this->state.getGenericStore().getCheckingInstance();
    std::vector<Diagnostics::Captured> *outerCapture = this->state.getDiagnostics().capture();
    if (owner) {
      this->state.getGenericStore().rememberCheckingInstance(owner);
      late.captureLateErrorsOf(owner, this->state.getDiagnostics());
    }
    this->state.getFlowState().replaceFlow(Flow());
    this->classChecker->checkClassBodies(cls);
    this->state.getGenericStore().rememberCheckingInstance(outerInstance);
    this->state.getDiagnostics().setCapture(outerCapture);
  }
  for (const LateError &error : late.getFirstLateErrors()) {
    InstanceRecord *instance = error.instance;
    if (instance->failed) {
      continue;
    }
    instance->failed = true;
    instance->message = error.first.message;
    instance->errorLoc = error.first.loc;
    this->genericInstantiator->reportInstance(instance, instance->callLoc, instance->concrete);
  }
}

void LateClasses::rememberEmittedLateClasses() {
  for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
    bool late = cls->generic() || (cls->expression() && cls->type != nullptr);
    if (late && cls->isEmitted()) {
      this->state.getInventoryStore().addLateClass(cls);
    }
  }
}

void LateClasses::noteMethodNames(const ClassDecl *cls) {
  for (const FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic()) {
      this->state.getLateClassStore().addLateMethodName(fn->name);
    }
  }
  for (const ClassField &f : cls->fields) {
    bool arrowMethod = !f.isStatic() && f.init && isa<FunctionExpr>(f.init);
    if (arrowMethod) {
      this->state.getLateClassStore().addLateMethodName(f.name);
    }
  }
}

void LateClasses::markLateOverrides(SourceFile &program) {
  for (Stmt *item : program.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls && cls->baseExpr) {
      this->noteMethodNames(cls);
    }
  }
  PlacedNodes nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *classExpr = dyn_cast<ClassExpr>(placed.expr);
    bool extends = classExpr && (classExpr->decl->baseExpr || classExpr->decl->base);
    if (extends) {
      this->noteMethodNames(classExpr->decl);
    }
  }
  for (ClassDecl *cls : this->state.getClassStore().getClasses()) {
    this->markLateOverrides(cls);
  }
}

void LateClasses::markLateOverrides(ClassDecl *cls) {
  LateClassStore &late = this->state.getLateClassStore();
  if (!late.hasLateMethodNames() || cls->literal()) {
    return;
  }
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    Overriding *overriding = fn->overriding();
    if (late.isLateMethodName(fn->name) && overriding) {
      overriding->overridden = true;
      overriding->openToLate = true;
    }
  }
}

void LateClasses::bindOuterTypeParams(const ClassDecl *cls) {
  const FunctionDecl *outer = cls ? cls->outer : nullptr;
  if (outer) {
    this->typeParams->bindTypeParams(outer->typeParams, outer->typeArgs);
  }
}

bool LateClasses::hasClassExpr(const FunctionDecl *fn) {
  if (!fn || !fn->body) {
    return false;
  }
  LateClassStore &late = this->state.getLateClassStore();
  if (late.isClassExprKnown(fn)) {
    return late.hasClassExpr(fn);
  }
  PlacedNodes nodes = PlacedNodes::collect({fn->body}, {});
  bool has = LateClasses::hasClassExprIn(nodes);
  late.rememberClassExpr(fn, has);

  return has;
}

bool LateClasses::hasClassExprIn(const PlacedNodes &nodes) {
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (isa<ClassExpr>(placed.expr)) {
      return true;
    }
  }

  return false;
}

bool LateClasses::isTemplate(const FuncSymbol *generic) {
  return generic && generic->isGeneric() && generic->decl && !generic->decl->isInstance &&
         this->hasClassExpr(generic->decl);
}

void LateClasses::checkTemplateInstance(InstanceRecord *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  BodyState saved = this->returnInferrer->enterBody(decl);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  this->state.getDiagnostics().setCapture(&errors);
  if (!this->state.getModuleStates().isTopLevelDone()) {
    this->state.getFunctionStack().rememberEarlyFunction(decl->sym);
  }
  this->state.getFlowState().replaceFlow(Flow());
  if (decl->sym->hasReturnType()) {
    this->functionChecker->checkFunctionBody(decl);
  } else {
    this->returnInferrer->checkAndInfer(decl);
  }
  this->returnInferrer->leaveBody(saved);
  instance->checking = false;
  instance->checked = true;
  if (errors.empty()) {
    return;
  }
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

LateClasses::ResultNoOldObjectHeld LateClasses::oldObject(const ClassDecl *cls,
                                                          const ClassField &field) {
  bool typed = field.isStatic() && field.var() && field.var()->type;
  bool holds = typed && LateClasses::holdsType(field.var()->type, cls->type);
  if (holds) {
    std::string message = ErrorTextOldObject::held(cls->name, field.name);

    return err(LateClassesOldObjectError{
        .code = LateClassesOldObjectErrorCode::HOLDS_OLD_OBJECT,
        .message = message,
    });
  }

  return ok(&field);
}

LateClasses::ResultFieldNameIsFree LateClasses::replacedField(const ClassDecl *cls,
                                                              const ClassDecl *replacement,
                                                              const ClassDecl *child,
                                                              const ClassField &field) {
  bool clash = !field.isStatic() && replacement->field(field.name);
  if (clash) {
    std::string message = ErrorTextReplacedField::clash(child->name, cls->name, field.name);

    return err(LateClassesReplacedFieldError{
        .code = LateClassesReplacedFieldErrorCode::FIELD_IN_REPLACEMENT,
        .message = message,
    });
  }

  return ok(&field);
}

void LateClasses::relink(ClassDecl *cls) {
  this->relinkClass(cls);
  for (ClassDecl *below : ClassTree::descendants(cls)) {
    this->relinkClass(below);
  }
}

void LateClasses::relinkClass(ClassDecl *cls) {
  const_cast<Type *>(cls->type)->forgetFields();
  bool muted = this->state.getDiagnostics().setMuted(true);
  this->classLinks->linkOverrides(cls);
  this->state.getDiagnostics().setMuted(muted);
  this->classLinks->buildVtable(cls);
}

void LateClasses::replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc) {
  for (const ClassField &f : cls->fields) {
    ResultNoOldObjectHeld kept = LateClasses::oldObject(cls, f);
    if (kept.isErr()) {
      std::string here = ErrorTextOldObject::decorator();
      this->state.getDiagnostics().error(f.loc, kept.error().message);
      this->state.getDiagnostics().note(loc, here);
      return;
    }
  }

  Type *mine = const_cast<Type *>(cls->type);
  Type *theirs = const_cast<Type *>(replacement->type);
  std::string name = mine->name().str();
  Type::swapClasses(mine, theirs);
  cls->type = theirs;
  replacement->type = mine;
  theirs->rename(name + " (as declared)");
  this->state.getInventoryStore().addSubtype(mine);
  std::vector<ClassDecl *> declared;
  for (ClassDecl *child : cls->children) {
    if (child == replacement) {
      continue;
    }
    if (replacement->derivesFrom(child)) {
      const_cast<Type *>(child->type)->replaceBase(mine, theirs);
      continue;
    }
    declared.push_back(child);
  }
  for (ClassDecl *child : declared) {
    for (const ClassField &f : child->fields) {
      ResultFieldNameIsFree unique = LateClasses::replacedField(cls, replacement, child, f);
      if (unique.isErr()) {
        this->state.getDiagnostics().error(f.loc, unique.error().message);
        return;
      }
    }
    llvm::erase(cls->children, child);
    replacement->children.push_back(child);
    child->parent = replacement;
    this->relink(child);
  }
  cls->replacement = replacement;
  replacement->usedAsValue = true;
}

}
