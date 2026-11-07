#include "compiler/instances/late_classes/late_classes.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_checker/class_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/module_declarer/module_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LateClasses::LateClasses(Checker &owner)
    : checker(owner) {}

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
    ClassChecker::ResultHasDecorators decorated = this->checker.classChecker().hasDecorators(cls);
    if (decorated.isOk()) {
      ClassDecl *outer = std::exchange(this->checker.currentClass_, cls);
      this->checker.decoratorChecker().decorateClass(cls);
      this->checker.currentClass_ = outer;
    }
  }
  expr->type = this->checker.classValueChecker().classValueType(cls);

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
  if (this->checker.fsDeclarer().isRuntimeClass(parent)) {
    std::string message = ErrorTextBaseClass::madeByFs(parent->name);

    return err(LateClassesBaseClassError{
        .code = LateClassesBaseClassErrorCode::MADE_BY_FS,
        .message = message,
    });
  }

  return ok(parent);
}

void LateClasses::makeLateClass(ClassDecl *cls) {
  cls->checkOnly = this->checker.reflectChecker().onlyChecked();
  if (!cls->type) {
    cls->id = ++this->checker.classCount_;
    llvm::StringRef typeName = cls->name.empty() ? "(anonymous class)" : cls->name;
    cls->type = Type::classType(typeName, cls);
    this->checker.classes_.push_back(cls);
  }
  if (this->checker.module_ && !this->checker.declModules_.count(cls)) {
    this->checker.declModules_[cls] = this->checker.module_;
  }
  for (FunctionDecl *fn : llvm::reverse(this->checker.functions_)) {
    if (fn->isInstance && !fn->typeArgs.empty()) {
      cls->outer = fn;
      break;
    }
  }
  for (size_t i = 1; i < this->checker.scopes_.size(); ++i) {
    for (const auto &entry : this->checker.scopes_[i]) {
      if (entry.second.var()) {
        cls->locals.push_back(entry.second.var()->name);
      }
    }
  }

  if (cls->baseExpr) {
    cls->baseExpr = this->checker.exprChecker().checkExpr(cls->baseExpr);
    ResultBaseClassUsable base = this->baseClass(cls->baseExpr->type);
    bool reported =
        base.isErr() && base.error().code == LateClassesBaseClassErrorCode::ALREADY_REPORTED;
    if (base.isErr() && !reported) {
      this->checker.diags_.error(cls->baseExpr->loc(), base.error().message);
    }
    ClassDecl *parent = base.isOk() ? base.value() : nullptr;
    if (parent && cls->checkOnly) {
      cls->parent = parent;
      const_cast<Type *>(cls->type)->addBase(parent->type);
    } else if (parent) {
      cls->parent = parent;
      parent->children.push_back(cls);
      this->checker.classBases().addBase(cls->type, parent->type);
    }
    cls->base.reset();
  }
  if (cls->name.empty() && cls->parent) {
    std::string extendsName = "(class extends " + cls->parent->type->name().str() + ")";
    const_cast<Type *>(cls->type)->rename(extendsName);
  }

  ClassDecl *outerDeclaring = std::exchange(this->checker.declaringClass_, cls);
  ClassDecl *outerClass = std::exchange(this->checker.currentClass_, cls);
  this->checker.classDeclarer().resolveClass(cls);
  this->checker.classDeclarer().declareMethods(cls);
  this->checker.currentClass_ = outerClass;
  this->checker.declaringClass_ = outerDeclaring;
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
  Module *home = this->checker.declModules_.lookup(cls);
  if (home) {
    for (FunctionDecl *fn : methods) {
      this->checker.declModules_[fn] = home;
    }
  }
  this->markLateOverrides(cls);
  if (this->checker.classesLinked_) {
    this->checker.classLinks().linkOverrides(cls);
    this->checker.classLinks().buildVtable(cls);
  }
  Checker::Instance *instance = this->checker.checkingInstance_;
  if (instance && instance->generic) {
    this->checker.lateOwners_[cls] = instance;
  }
  this->checker.lateClasses_.push_back(cls);
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
  ClassDecl *outer = std::exchange(this->checker.currentClass_, cls);
  bool outerStatic = std::exchange(this->checker.staticInit_, true);
  cls->declared = true;
  this->checker.staticsSet_.clear();
  for (ClassField &f : cls->fields) {
    if (!f.isStatic() || !f.var()) {
      continue;
    }
    if (!f.init) {
      if (!f.var()->type) {
        f.var()->type = Type::error();
      }
      this->checker.staticsSet_.insert(f.var());
      continue;
    }
    const Type *declared = f.var()->type;
    if (declared && f.resolved && !f.type && !f.optional()) {
      declared = nullptr;
    }
    const Type *expected = declared && !declared->isError() ? declared : nullptr;
    f.init = this->checker.exprChecker().checkExpr(f.init, expected);
    this->checker.staticsSet_.insert(f.var());
    if (declared) {
      std::string label = ErrorTextStaticType::fieldLabel(f.name);
      f.init = this->checker.typeRelations().coerce(f.init, declared, label);
      continue;
    }
    ResultStaticTypeKnown known = LateClasses::staticType(f, f.init->type);
    if (known.isErr()) {
      this->checker.diags_.error(f.init->loc(), known.error().message);
    }
    const Type *type = known.isOk() ? known.value() : Type::error();
    f.resolved = type;
    f.var()->type = type;
  }
  this->checker.staticInit_ = outerStatic;
  this->checker.currentClass_ = outer;
}

void LateClasses::checkLateClasses() {
  for (; this->checker.lateChecked_ < this->checker.lateClasses_.size();
       ++this->checker.lateChecked_) {
    ClassDecl *cls = this->checker.lateClasses_[this->checker.lateChecked_];
    if (cls->checkOnly) {
      continue;
    }
    Module *home = this->checker.declModules_.lookup(cls);
    if (home && this->checker.scopes_.size() == 1) {
      this->checker.moduleDeclarer().useModule(home);
    }
    Checker::Instance *owner = this->checker.lateOwners_.lookup(cls);
    Checker::Instance *outerInstance = this->checker.checkingInstance_;
    std::vector<Diagnostics::Captured> *outerCapture = this->checker.diags_.capture();
    if (owner) {
      this->checker.checkingInstance_ = owner;
      this->checker.diags_.setCapture(&this->checker.lateErrors_[owner]);
    }
    this->checker.flow_ = Checker::Flow();
    this->checker.classChecker().checkClassBodies(cls);
    this->checker.checkingInstance_ = outerInstance;
    this->checker.diags_.setCapture(outerCapture);
  }
  for (auto &[instance, errors] : this->checker.lateErrors_) {
    if (errors.empty() || instance->failed) {
      continue;
    }
    instance->failed = true;
    instance->message = errors.front().message;
    instance->errorLoc = errors.front().loc;
    this->checker.genericInstantiator().reportInstance(instance, instance->callLoc,
                                                       instance->concrete);
  }
}

void LateClasses::noteMethodNames(const ClassDecl *cls) {
  for (const FunctionDecl *fn : cls->methods) {
    if (!fn->isStatic()) {
      this->lateMethodNames.insert(fn->name);
    }
  }
  for (const ClassField &f : cls->fields) {
    bool arrowMethod = !f.isStatic() && f.init && isa<FunctionExpr>(f.init);
    if (arrowMethod) {
      this->lateMethodNames.insert(f.name);
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
    visitExprs(item, noteClassExpr);
  }
  for (ClassDecl *cls : this->checker.classes_) {
    this->markLateOverrides(cls);
  }
}

void LateClasses::markLateOverrides(ClassDecl *cls) {
  if (this->lateMethodNames.empty() || cls->literal()) {
    return;
  }
  for (FunctionDecl *fn : ClassLinks::ownMethods(cls)) {
    Overriding *overriding = fn->overriding();
    if (this->lateMethodNames.contains(fn->name) && overriding) {
      overriding->overridden = true;
      overriding->openToLate = true;
    }
  }
}

void LateClasses::bindOuterTypeParams(const ClassDecl *cls) {
  const FunctionDecl *outer = cls ? cls->outer : nullptr;
  if (outer) {
    this->checker.typeParams().bindTypeParams(outer->typeParams, outer->typeArgs);
  }
}

bool LateClasses::hasClassExpr(const FunctionDecl *fn) {
  if (!fn || !fn->body) {
    return false;
  }
  auto found = this->templates.find(fn);
  if (found != this->templates.end()) {
    return found->second;
  }
  bool has = false;
  auto spotClassExpr = [&](const Expr *e) { has |= isa<ClassExpr>(e); };
  visitExprs(fn->body, spotClassExpr);
  this->templates[fn] = has;

  return has;
}

bool LateClasses::isTemplate(const FuncSymbol *generic) {
  return generic && generic->isGeneric() && generic->decl && !generic->decl->isInstance &&
         this->hasClassExpr(generic->decl);
}

void LateClasses::checkTemplateInstance(Checker::Instance *instance) {
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  FunctionDecl *decl = instance->decl;
  Checker::BodyState saved = this->checker.returnInferrer().enterBody(decl);
  this->checker.checkingInstance_ = instance;
  this->checker.diags_.setCapture(&errors);
  if (!this->checker.topLevelDone_) {
    this->checker.early_ = decl->sym;
  }
  this->checker.flow_ = Checker::Flow();
  if (decl->sym->ret) {
    this->checker.functionChecker().checkFunctionBody(decl);
  } else {
    this->checker.returnInferrer().checkAndInfer(decl);
  }
  this->checker.returnInferrer().leaveBody(saved);
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
  bool muted = this->checker.diags_.setMuted(true);
  this->checker.classLinks().linkOverrides(cls);
  this->checker.diags_.setMuted(muted);
  this->checker.classLinks().buildVtable(cls);
  for (ClassDecl *child : cls->children) {
    this->relink(child);
  }
}

void LateClasses::replaceClass(ClassDecl *cls, ClassDecl *replacement, llvm::SMLoc loc) {
  for (const ClassField &f : cls->fields) {
    ResultNoOldObjectHeld kept = LateClasses::oldObject(cls, f);
    if (kept.isErr()) {
      std::string here = ErrorTextOldObject::decorator();
      this->checker.diags_.error(f.loc, kept.error().message);
      this->checker.diags_.note(loc, here);
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
  if (!llvm::is_contained(this->checker.file_->subtypes, mine)) {
    this->checker.file_->subtypes.push_back(mine);
  }
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
        this->checker.diags_.error(f.loc, unique.error().message);
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
