#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/ast_walker/ast_walker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/class_bases/class_bases.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/classes/class_value_checker/class_value_checker.h"
#include "compiler/pipeline/check_program/topics/classes/decorator_checker/decorator_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_declarer/fs_declarer.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/topics/reflect/reflect_checker/reflect_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LateClasses::LateClasses(CheckState &owner)
    : state(owner) {}

bool LateClasses::holdsType(const Type *type, const Type *target,
                            llvm::SmallPtrSetImpl<const Type *> &seen) {
  if (type == target) {
    return true;
  }
  bool firstVisit = seen.insert(type).second;
  if (!firstVisit) {
    return false;
  }
  auto memberHolds = [&](const Type *member) {
    return LateClasses::holdsType(member, target, seen);
  };
  auto fieldHolds = [&](const Field &field) {
    return LateClasses::holdsType(field.type, target, seen);
  };
  switch (type->kind()) {
    case Type::Kind::Optional:
    case Type::Kind::Array:
    case Type::Kind::Promise:
      return LateClasses::holdsType(type->inner(), target, seen);
    case Type::Kind::Union:
      return llvm::any_of(type->members(), memberHolds);
    case Type::Kind::Dict:
      return LateClasses::holdsType(type->keyType(), target, seen) ||
             LateClasses::holdsType(type->valueType(), target, seen);
    case Type::Kind::Object:
      return llvm::any_of(type->fields(), fieldHolds);
    default:
      return false;
  }
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
    ClassChecker::ResultHasDecorators decorated = ClassChecker(this->state).hasDecorators(cls);
    if (decorated.isOk()) {
      this->state.getClassContext().enterClass(cls);
      DecoratorChecker(this->state).decorateClass(cls);
      this->state.getClassContext().leaveClass();
    }
  }
  expr->type = ClassValueChecker(this->state).classValueType(cls);

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
  if (FsDeclarer(this->state).isRuntimeClass(parent)) {
    std::string message = ErrorTextBaseClass::madeByFs(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::MADE_BY_FS,
        .message = message,
    });
  }

  return ok(parent);
}

void LateClasses::makeLateClass(ClassDecl *cls) {
  cls->checkOnly = ReflectChecker(this->state).onlyChecked();
  if (!cls->type) {
    cls->id = this->state.getClassStore().takeClassId();
    llvm::StringRef typeName = cls->name.empty() ? "(anonymous class)" : cls->name;
    cls->type = Type::classType(typeName, cls);
    this->state.getClassStore().addClass(cls);
  }
  if (this->state.getModuleStates().getCurrentModule() &&
      !this->state.getModuleStates().hasDeclModule(cls)) {
    this->state.getModuleStates().rememberDeclModule(
        cls, this->state.getModuleStates().getCurrentModule());
  }
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

  if (cls->baseExpr) {
    cls->baseExpr = ExprChecker(this->state).checkExpr(cls->baseExpr);
    ResultBaseClassUsable base = this->baseClass(cls->baseExpr->type);
    bool reported =
        base.isErr() && base.error().code == LateClassesBaseClassErrorCode::ALREADY_REPORTED;
    if (base.isErr() && !reported) {
      this->state.getDiagnostics().error(cls->baseExpr->loc(), base.error().message);
    }
    ClassDecl *parent = base.isOk() ? base.value() : nullptr;
    if (parent && cls->checkOnly) {
      cls->parent = parent;
      const_cast<Type *>(cls->type)->addBase(parent->type);
    } else if (parent) {
      cls->parent = parent;
      parent->children.push_back(cls);
      ClassBases(this->state).addBase(cls->type, parent->type);
    }
    cls->base.reset();
  }
  if (cls->name.empty() && cls->parent) {
    std::string extendsName = "(class extends " + cls->parent->type->name().str() + ")";
    const_cast<Type *>(cls->type)->rename(extendsName);
  }

  LateClassStore &late = this->state.getLateClassStore();
  late.enterDeclaringClass(cls);
  this->state.getClassContext().enterClass(cls);
  ClassDeclarer(this->state).resolveClass(cls);
  ClassDeclarer(this->state).declareMethods(cls);
  this->state.getClassContext().leaveClass();
  late.leaveDeclaringClass();
  std::vector<FunctionDecl *> methods = cls->methods;
  methods.insert(methods.end(), cls->constructors.begin(), cls->constructors.end());
  if (cls->init) {
    methods.push_back(cls->init);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      methods.push_back(f.method());
    }
  }
  Module *home = this->state.getModuleStates().findDeclModule(cls);
  if (home) {
    for (FunctionDecl *fn : methods) {
      this->state.getModuleStates().rememberDeclModule(fn, home);
    }
  }
  this->markLateOverrides(cls);
  if (this->state.getClassStore().hasLinkedClasses()) {
    ClassLinks(this->state).linkOverrides(cls);
    ClassLinks(this->state).buildVtable(cls);
  }
  InstanceRecord *instance = this->state.getGenericStore().getCheckingInstance();
  if (instance && instance->generic) {
    late.rememberLateOwner(cls, instance);
  }
  late.addLateClass(cls);
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
    f.init = ExprChecker(this->state).checkExpr(f.init, expected);
    classContext.markStaticSet(f.var());
    if (declared) {
      std::string label = ErrorTextStaticType::fieldLabel(f.name);
      f.init = TypeRelations(this->state).coerce(f.init, declared, label);
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
      ModuleDeclarer(this->state).useModule(home);
    }
    InstanceRecord *owner = late.findLateOwner(cls);
    InstanceRecord *outerInstance = this->state.getGenericStore().getCheckingInstance();
    std::vector<Diagnostics::Captured> *outerCapture = this->state.getDiagnostics().capture();
    if (owner) {
      this->state.getGenericStore().rememberCheckingInstance(owner);
      late.captureLateErrorsOf(owner, this->state.getDiagnostics());
    }
    this->state.getFlowState().replaceFlow(Flow());
    ClassChecker(this->state).checkClassBodies(cls);
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
    GenericInstantiator(this->state)
        .reportInstance(instance, instance->callLoc, instance->concrete);
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
  auto noteClassExpr = [&](const Expr *e) {
    auto *classExpr = dyn_cast<ClassExpr>(e);
    bool extends = classExpr && (classExpr->decl->baseExpr || classExpr->decl->base);
    if (extends) {
      this->noteMethodNames(classExpr->decl);
    }
  };
  for (Stmt *item : program.items) {
    auto *cls = dyn_cast<ClassDecl>(item);
    if (cls && cls->baseExpr) {
      this->noteMethodNames(cls);
    }
    AstWalker::visitExprs(item, noteClassExpr);
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
    TypeParams(this->state).bindTypeParams(outer->typeParams, outer->typeArgs);
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
  bool has = false;
  auto spotClassExpr = [&](const Expr *e) { has |= isa<ClassExpr>(e); };
  AstWalker::visitExprs(fn->body, spotClassExpr);
  late.rememberClassExpr(fn, has);

  return has;
}

bool LateClasses::isTemplate(const FuncSymbol *generic) {
  return generic && generic->isGeneric() && generic->decl && !generic->decl->isInstance &&
         this->hasClassExpr(generic->decl);
}

void LateClasses::checkTemplateInstance(InstanceRecord *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  BodyState saved = ReturnInferrer(this->state).enterBody(decl);
  this->state.getGenericStore().rememberCheckingInstance(instance);
  this->state.getDiagnostics().setCapture(&errors);
  if (!this->state.getModuleStates().isTopLevelDone()) {
    this->state.getFunctionStack().rememberEarlyFunction(decl->sym);
  }
  this->state.getFlowState().replaceFlow(Flow());
  if (decl->sym->ret) {
    FunctionChecker(this->state).checkFunctionBody(decl);
  } else {
    ReturnInferrer(this->state).checkAndInfer(decl);
  }
  ReturnInferrer(this->state).leaveBody(saved);
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
  llvm::SmallPtrSet<const Type *, 8> seen;
  bool typed = field.isStatic() && field.var() && field.var()->type;
  bool holds = typed && LateClasses::holdsType(field.var()->type, cls->type, seen);
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
  const_cast<Type *>(cls->type)->forgetFields();
  bool muted = this->state.getDiagnostics().setMuted(true);
  ClassLinks(this->state).linkOverrides(cls);
  this->state.getDiagnostics().setMuted(muted);
  ClassLinks(this->state).buildVtable(cls);
  for (ClassDecl *child : cls->children) {
    this->relink(child);
  }
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
  this->state.getProgramUse().addSubtype(mine);
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
