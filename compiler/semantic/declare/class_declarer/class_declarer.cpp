#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver_errors.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

namespace tz {

ClassDeclarer::ClassDeclarer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->classBases = &all.classBases;
  this->typeResolver = &all.typeResolver;
  this->functionChecker = &all.functionChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

ClassDecl *ClassDeclarer::classOf(const Type *type) {
  return type ? const_cast<ClassDecl *>(type->classDecl()) : nullptr;
}

std::vector<FunctionDecl *> ClassDeclarer::methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                        bool isStatic) {
  std::vector<FunctionDecl *> found;
  for (const ClassDecl *c = cls; c; c = c->parent) {
    std::vector<FunctionDecl *> own;
    const ClassDecl *holder = isStatic ? c->staticOwner() : c;
    for (FunctionDecl *fn : holder->methods) {
      if (fn->name == name && fn->isStatic() == isStatic) {
        own.push_back(fn);
      }
    }
    if (!isStatic) {
      for (const ClassField &f : c->fields) {
        if (f.method() && f.name == name) {
          own.push_back(f.method());
        }
      }
    }
    for (FunctionDecl *fn : own) {
      auto overridden = [&](const FunctionDecl *v) {
        if (!isStatic) {
          return ClassLinks::overridesTransitively(v, fn);
        }
        const FuncSymbol *a = v->sym;
        const FuncSymbol *b = fn->sym;

        return a && b && a->params == b->params && a->minArgs == b->minArgs &&
               a->hasRest == b->hasRest;
      };
      bool replaced = llvm::any_of(found, overridden);
      if (!replaced) {
        found.push_back(fn);
      }
    }
  }

  return found;
}

void ClassDeclarer::declareClass(ClassDecl *cls) {
  cls->id = this->state.getClassStore().takeClassId();
  cls->type = Type::classType(cls->name, cls);
  this->state.getClassStore().addClass(cls);
  Scopes::ResultEntryDeclared added = this->scopes->declare(cls->name, {ClassEntry{.cls = cls}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(cls->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->scopes->lookup(cls->name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
}

const ClassField *ClassDeclarer::earlierField(const ClassDecl *cls, size_t index) {
  const ClassField &f = cls->fields[index];
  for (size_t j = 0; j < index; ++j) {
    const ClassField &other = cls->fields[j];
    if (other.name == f.name && other.isStatic() == f.isStatic()) {
      return &other;
    }
  }

  return nullptr;
}

ClassDeclarer::ResultFieldNameUnique
ClassDeclarer::uniqueField(const ClassDecl *cls, const ClassField &f, const ClassField *earlier) {
  if (earlier) {
    std::string message = ErrorTextUniqueField::duplicate(f.name, cls->name);

    return err(ClassDeclarerUniqueFieldError{
        .code = ClassDeclarerUniqueFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(&f);
}

ClassDeclarer::ResultFieldNameIsFree
ClassDeclarer::fieldOrMethod(const ClassDecl *cls, const ClassField &f, const FunctionDecl *fn) {
  if (fn->name == f.name && fn->isStatic() == f.isStatic()) {
    std::string message = ErrorTextFieldOrMethod::shared(f.name, cls->name);

    return err(ClassDeclarerFieldOrMethodError{
        .code = ClassDeclarerFieldOrMethodErrorCode::FIELD_AND_METHOD,
        .message = message,
    });
  }

  return ok(&f);
}

void ClassDeclarer::checkMemberName(const ClassDecl *cls, size_t index) {
  const ClassField &f = cls->fields[index];
  const ClassField *earlier = ClassDeclarer::earlierField(cls, index);
  ResultFieldNameUnique unique = ClassDeclarer::uniqueField(cls, f, earlier);
  if (unique.isErr()) {
    std::string previous = ErrorTextUniqueField::previous();
    this->state.getDiagnostics().error(f.loc, unique.error().message);
    this->state.getDiagnostics().note(earlier->loc, previous);
  }
  for (FunctionDecl *fn : cls->methods) {
    ResultFieldNameIsFree distinct = ClassDeclarer::fieldOrMethod(cls, f, fn);
    if (distinct.isErr()) {
      std::string field = ErrorTextFieldOrMethod::field();
      this->state.getDiagnostics().error(fn->loc(), distinct.error().message);
      this->state.getDiagnostics().note(f.loc, field);
    }
  }
}

ClassDeclarer::ResultFieldNotLateSet ClassDeclarer::setLater(const ClassField &f) {
  if (f.definite()) {
    std::string message = ErrorTextSetLater::definite(f.name);

    return err(ClassDeclarerSetLaterError{
        .code = ClassDeclarerSetLaterErrorCode::SET_LATER,
        .message = message,
    });
  }

  return ok(&f);
}

ClassDeclarer::ResultFieldTypeIsSeen ClassDeclarer::typeSeen(const ClassField &f) {
  bool untypedValue = !f.type && f.init != nullptr && SharedHelpers::literalType(f.init) == nullptr;
  if (untypedValue && f.isStatic()) {
    std::string message = ErrorTextTypeSeen::shared(f.name);

    return err(ClassDeclarerTypeSeenError{
        .code = ClassDeclarerTypeSeenErrorCode::STATIC_VALUE_NOT_LITERAL,
        .message = message,
    });
  }
  bool function = llvm::isa_and_nonnull<FunctionExpr>(f.init);
  if (untypedValue && function && f.method() == nullptr) {
    std::string message = ErrorTextTypeSeen::function(f.name);

    return err(ClassDeclarerTypeSeenError{
        .code = ClassDeclarerTypeSeenErrorCode::FUNCTION_FIELD_UNTYPED,
        .message = message,
    });
  }

  return ok(&f);
}

ClassDeclarer::ResultFieldHasNoValue ClassDeclarer::fieldValue(const ClassDecl *cls,
                                                               const ClassField &f) {
  bool function = llvm::isa_and_nonnull<FunctionExpr>(f.init);
  bool valued = !f.isStatic() && f.init != nullptr && !function;
  if (!valued) {
    return ok(&f);
  }
  if (!cls->constructors.empty()) {
    std::string message = ErrorTextDeclaredValue::constructor(f.name);

    return err(ClassDeclarerFieldValueError{
        .code = ClassDeclarerFieldValueErrorCode::VALUE_OUTSIDE_CONSTRUCTOR,
        .message = message,
    });
  }
  bool literal = SharedHelpers::literalType(f.init) != nullptr;
  if (!f.type || !literal) {
    std::string message = ErrorTextDeclaredValue::literal(f.name);

    return err(ClassDeclarerFieldValueError{
        .code = ClassDeclarerFieldValueErrorCode::START_VALUE_NOT_LITERAL,
        .message = message,
    });
  }

  return ok(&f);
}

ClassDeclarer::ResultFieldTypeSource ClassDeclarer::typedField(const ClassDecl *cls,
                                                               const ClassField &f) {
  const Param *property = f.parameter() ? ClassDeclarer::propertyParam(cls, f.name) : nullptr;
  if (property != nullptr && !property->type) {
    std::string message = ErrorTextTypedField::property(f.name);

    return err(ClassDeclarerTypedFieldError{
        .code = ClassDeclarerTypedFieldErrorCode::PROPERTY_NO_TYPE,
        .message = message,
    });
  }
  if (!f.parameter() && !f.type && !f.init) {
    std::string message = ErrorTextTypedField::missing(f.name);

    return err(ClassDeclarerTypedFieldError{
        .code = ClassDeclarerTypedFieldErrorCode::NO_TYPE,
        .message = message,
    });
  }

  return ok(&f);
}

const Param *ClassDeclarer::propertyParam(const ClassDecl *cls, llvm::StringRef name) {
  for (FunctionDecl *ctor : cls->constructors) {
    for (const Param &p : ctor->params) {
      if (p.property && p.name == name) {
        return &p;
      }
    }
  }

  return nullptr;
}

const Type *ClassDeclarer::declaredFieldType(const ClassDecl *cls, ClassField &f) {
  if (f.parameter()) {
    const Param *param = ClassDeclarer::propertyParam(cls, f.name);
    if (param->optional()) {
      f.presence = FieldPresence::OPTIONAL;
    }
    return this->typeResolver->resolveType(*param->type, false);
  }
  if (f.type) {
    return this->typeResolver->resolveType(*f.type, false);
  }
  const Type *literal = SharedHelpers::literalType(f.init);
  bool fromValueLater = f.isStatic() || llvm::isa<FunctionExpr>(f.init);
  if (literal != nullptr || fromValueLater) {
    return literal;
  }

  return Type::error();
}

void ClassDeclarer::arrowMethod(ClassDecl *cls, ClassField &f, const Type *type) {
  auto *function = llvm::dyn_cast_or_null<FunctionExpr>(f.init);
  if (function && function->fn->name.empty()) {
    function->fn->name = f.name;
  }
  bool method = function && function->fn->isArrow() && !f.isStatic() && !f.parameter() &&
                !f.optional() && !f.redeclared &&
                !this->state.getFlowState().isFieldAssigned(f.name) &&
                (!type || type->isFunction());
  if (!method) {
    return;
  }
  FunctionDecl *fn = function->fn;
  fn->name = f.name;
  ClassMember *member = fn->member();
  if (member) {
    member->cls = cls;
    member->access = f.access;
    member->isOverride = f.isOverride;
  } else {
    fn->role = ClassMember{.cls = cls, .access = f.access, .isOverride = f.isOverride};
  }
  fn->place = FunctionPlace::DECLARED;
  f.form = ArrowMethodField{.method = fn};
  f.resolved = nullptr;
}

ClassDeclarer::ResultStaticTypeFixed
ClassDeclarer::staticTypeParams(const ClassDecl *cls, const ClassField &f, const Type *type) {
  if (f.isStatic() && type && !cls->typeArgs.empty() && TypeQueries::mentionsParam(type)) {
    std::string message = ErrorTextStaticParams::shared(f.name, cls->name);

    return err(ClassDeclarerStaticTypeParamsError{
        .code = ClassDeclarerStaticTypeParamsErrorCode::USES_TYPE_PARAMS,
        .message = message,
    });
  }

  return ok(type);
}

ClassDeclarer::ResultStaticGetsValue ClassDeclarer::staticValue(const ClassField &f,
                                                                const Type *type) {
  if (!f.init && !f.optional() && type && !type->isError()) {
    std::string message = ErrorTextStaticValue::missing(f.name, type->name());

    return err(ClassDeclarerStaticValueError{
        .code = ClassDeclarerStaticValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(&f);
}

void ClassDeclarer::declareStaticVar(const ClassDecl *cls, const ClassField &f, StaticField *shared,
                                     const Type *type) {
  VarSymbol *var = this->state.getAst().newVar();
  std::string name = (cls->name + "." + f.name).str();
  var->name = this->state.getAst().keepName(name);
  var->loc = f.loc;
  var->type = type;
  var->isConst = f.readonly;
  var->place = GlobalVar{.usedInFunction = true};
  shared->var = var;
  auto *literal = f.readonly ? llvm::dyn_cast_or_null<IntLitExpr>(f.init) : nullptr;
  bool fitsInt = literal && literal->value <= uint64_t(std::numeric_limits<int64_t>::max());
  if (fitsInt) {
    this->state.getConstantStore().rememberConstVar(var, int64_t(literal->value));
  }
  ResultStaticGetsValue valued = ClassDeclarer::staticValue(f, type);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(f.loc, valued.error().message);
  }
}

void ClassDeclarer::resolveField(ClassDecl *cls, ClassField &f) {
  ResultFieldNotLateSet early = ClassDeclarer::setLater(f);
  if (early.isErr()) {
    this->state.getDiagnostics().error(f.loc, early.error().message);
  }
  ResultFieldHasNoValue valueless = ClassDeclarer::fieldValue(cls, f);
  if (valueless.isErr()) {
    this->state.getDiagnostics().error(f.loc, valueless.error().message);
  }
  const Type *type = nullptr;
  ResultFieldTypeSource source = ClassDeclarer::typedField(cls, f);
  if (source.isErr()) {
    this->state.getDiagnostics().error(f.loc, source.error().message);
    type = Type::error();
  } else {
    type = this->declaredFieldType(cls, f);
  }
  if (type && f.optional()) {
    type = Type::optional(type);
  }
  f.resolved = type;
  this->arrowMethod(cls, f, type);
  ResultFieldTypeIsSeen seen = ClassDeclarer::typeSeen(f);
  if (seen.isErr()) {
    this->state.getDiagnostics().error(f.loc, seen.error().message);
    type = Type::error();
    f.resolved = type;
  }

  ResultStaticTypeFixed fixed = ClassDeclarer::staticTypeParams(cls, f, type);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(f.loc, fixed.error().message);
    type = Type::error();
    f.resolved = type;
  }
  StaticField *shared = f.form.as<StaticField>();
  if (shared) {
    this->declareStaticVar(cls, f, shared, type);
  }
}

void ClassDeclarer::resolveClass(ClassDecl *cls) {
  if (cls->stage != ClassStage::UNRESOLVED) {
    return;
  }
  cls->stage = ClassStage::RESOLVED;
  this->genericClassChecker->declareClassTypeParams(cls);
  this->state.getScopeStack().openTypeScope();
  this->genericClassChecker->bindClassTypeParams(cls);
  this->classBases->resolveClassBases(cls);
  for (size_t i = 0; i < cls->fields.size(); ++i) {
    this->checkMemberName(cls, i);
  }
  for (ClassField &f : cls->fields) {
    this->resolveField(cls, f);
  }
  this->state.getScopeStack().closeTypeScope();
}

ClassDeclarer::ResultParamTypePassed ClassDeclarer::paramType(const Param &param, const Type *type,
                                                              const Type *declared) {
  if (!type) {
    std::string message = ErrorTextParamType::extra(param.label(), declared->name());

    return err(ClassDeclarerParamTypeError{
        .code = ClassDeclarerParamTypeErrorCode::MORE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(type);
}

void ClassDeclarer::declareTyped(FunctionDecl *fn, const Type *declared) {
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *given = i < declared->params().size() ? declared->params()[i] : nullptr;
    const Type *written = param.type ? this->typeResolver->resolveType(*param.type, false) : given;
    ResultParamTypePassed passed = ClassDeclarer::paramType(param, written, declared);
    if (passed.isErr()) {
      this->state.getDiagnostics().error(param.loc, passed.error().message);
    }
    const Type *type = passed.isOk() ? passed.value() : Type::error();
    if (param.optional() && !type->isOptional()) {
      type = Type::optional(type);
    }
    param.declaredType = param.defaultValue() ? type->nonOptional() : type;
    sym->params.push_back(type);
  }
  for (size_t i = fn->params.size(); i < declared->params().size(); ++i) {
    Param hidden;
    hidden.name = this->state.getAst().hiddenName();
    hidden.loc = fn->loc();
    hidden.declaredType = declared->params()[i];
    fn->params.push_back(hidden);
    sym->params.push_back(declared->params()[i]);
  }
  sym->minArgs = declared->minArgs();
  sym->hasRest = declared->hasRest();
  sym->setReturnType(declared->ret());
  sym->discardsResult = declared->ret()->isVoid();
}

ClassDeclarer::ResultAbstractReturns ClassDeclarer::abstractReturn(const ClassDecl *cls,
                                                                   const FunctionDecl *fn) {
  if (fn->isAbstract() && !fn->returnType && cls->isGeneric()) {
    std::string message = ErrorTextAbstractReturn::missing(fn->name, cls->name);

    return err(ClassDeclarerAbstractReturnError{
        .code = ClassDeclarerAbstractReturnErrorCode::NO_RETURN_TYPE,
        .message = message,
    });
  }

  return ok(fn);
}

ClassDeclarer::ResultGenericIsDirect ClassDeclarer::genericMethod(const FunctionDecl *fn) {
  bool generic = !fn->typeParams.empty();
  if (generic && fn->isAbstract()) {
    std::string message = ErrorTextGenericMethod::abstract();

    return err(ClassDeclarerGenericMethodError{
        .code = ClassDeclarerGenericMethodErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (generic && fn->isOverride()) {
    std::string message = ErrorTextGenericMethod::overrides();

    return err(ClassDeclarerGenericMethodError{
        .code = ClassDeclarerGenericMethodErrorCode::OVERRIDE,
        .message = message,
    });
  }

  return ok(fn);
}

void ClassDeclarer::numberVariants(llvm::ArrayRef<FunctionDecl *> functions) {
  for (size_t i = 0; i < functions.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      bool sameName = functions[j]->name == functions[i]->name &&
                      functions[j]->isStatic() == functions[i]->isStatic();
      if (sameName) {
        functions[i]->member()->variant = functions[j]->variant() + 1;
      }
    }
  }
}

void ClassDeclarer::declareMethods(ClassDecl *cls) {
  for (FunctionDecl *ctor : cls->constructors) {
    this->functionChecker->declareFunction(ctor, nullptr, false);
    ctor->sym->setReturnType(Type::voidTy());
  }
  for (FunctionDecl *fn : cls->methods) {
    this->declareClassMethod(cls, fn);
  }
  for (const ClassField &f : cls->fields) {
    if (f.method() != nullptr) {
      this->declareFieldMethod(cls, f);
    }
  }
  if (ClassLinks::constructorsOf(cls).empty()) {
    this->fillDefaultValues(cls);
  }
  this->makeFieldInit(cls);
  ClassDeclarer::numberVariants(cls->methods);
  ClassDeclarer::numberVariants(cls->constructors);
  this->checkVariants(cls);
}

void ClassDeclarer::declareClassMethod(const ClassDecl *cls, FunctionDecl *fn) {
  this->functionChecker->declareFunction(fn, nullptr, false);
  ResultAbstractReturns returns = ClassDeclarer::abstractReturn(cls, fn);
  if (returns.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), returns.error().message);
  } else if (fn->isAbstract() && !fn->returnType.has_value()) {
    fn->sym->setReturnType(nullptr);
  }
  ResultGenericIsDirect direct = ClassDeclarer::genericMethod(fn);
  if (direct.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), direct.error().message);
  }
}

void ClassDeclarer::declareFieldMethod(ClassDecl *cls, const ClassField &f) {
  FunctionDecl *fn = f.method();
  this->state.getScopeStack().openTypeScope();
  this->genericClassChecker->bindClassTypeParams(cls);
  const Type *declared =
      f.type.has_value() ? this->typeResolver->resolveType(*f.type, false) : nullptr;
  this->state.getScopeStack().closeTypeScope();
  if (declared == nullptr || !declared->isFunction()) {
    this->functionChecker->declareFunction(fn, nullptr, false);
    return;
  }
  this->declareTyped(fn, declared);
}

void ClassDeclarer::fillDefaultValues(ClassDecl *cls) {
  for (ClassField &f : cls->fields) {
    bool skipped = f.isStatic() || f.init != nullptr || f.method() != nullptr || f.optional() ||
                   f.parameter() || f.definite() || f.resolved == nullptr;
    if (skipped) {
      continue;
    }
    llvm::SmallPtrSet<const ClassDecl *, 4> visiting;
    visiting.insert(cls);
    const TypeRef *ref = f.type.has_value() ? &*f.type : nullptr;
    f.init = this->defaultValue(f.resolved, ref, f.loc, visiting);
  }
}

void ClassDeclarer::makeFieldInit(ClassDecl *cls) {
  std::vector<Stmt *> sets;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic() || f.init == nullptr || f.method() != nullptr) {
      continue;
    }
    auto *self = this->state.getAst().make<NameExpr>(f.loc, "this");
    auto *target = this->state.getAst().make<MemberExpr>(f.loc, self, f.name);
    auto *set = this->state.getAst().make<AssignExpr>(f.loc, std::nullopt, target, f.init);
    sets.push_back(this->state.getAst().make<ExprStmt>(f.loc, set));
  }
  if (sets.empty()) {
    return;
  }
  auto *init = this->state.getAst().make<FunctionDecl>(cls->loc(), "init");
  init->role = ClassMember{.cls = cls};
  init->body = this->state.getAst().make<BlockStmt>(cls->loc());
  init->body->body = std::move(sets);
  this->functionChecker->declareFunction(init, nullptr, false);
  init->sym->setReturnType(Type::voidTy());
  cls->init = init;
}

Expr *ClassDeclarer::literalValue(const Type *literal, llvm::SMLoc loc) {
  const Type *base = literal->inner();
  if (base && base->isString()) {
    return this->state.getAst().make<StringLitExpr>(loc, literal->literalText());
  }
  if (base && base->isBool()) {
    return this->state.getAst().make<BoolLitExpr>(loc, literal->literalNumber() != 0);
  }
  double number = literal->literalNumber();
  double size = number < 0 ? -number : number;
  if (base && base->isFloat()) {
    Expr *value = this->state.getAst().make<FloatLitExpr>(loc, size);
    return number < 0 ? this->state.getAst().make<UnaryExpr>(loc, UnaryOp::Neg, value) : value;
  }
  auto magnitude = static_cast<uint64_t>(size);
  Expr *value = this->state.getAst().make<IntLitExpr>(loc, magnitude);

  return number < 0 ? this->state.getAst().make<UnaryExpr>(loc, UnaryOp::Neg, value) : value;
}

Expr *ClassDeclarer::defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                                  llvm::SmallPtrSetImpl<const ClassDecl *> &visiting) {
  DefaultRun run{visiting, loc, {DefaultFor{type, ref}}, {}};
  while (!run.steps.empty()) {
    DefaultStep step = run.steps.back();
    run.steps.pop_back();
    bool fits = this->applyDefaultStep(step, run);
    if (!fits) {
      return nullptr;
    }
  }

  return run.values.back();
}

bool ClassDeclarer::applyDefaultStep(const DefaultStep &step, DefaultRun &run) {
  switch (step.kind()) {
    case DefaultStepKind::VALUE:
      return this->defaultFor(*step.as<DefaultFor>(), run);
    case DefaultStepKind::CHECK_EMPTY:
      return ClassDeclarer::checkEmpty(step.as<CheckEmpty>()->cls, run);
    case DefaultStepKind::RESOLVE_FAMILY:
      this->resolveFamily(step.as<ResolveFamily>()->cls, run);
      return true;
    case DefaultStepKind::LEAVE_EMPTY:
      run.visiting.erase(step.as<LeaveEmpty>()->cls);
      return true;
    case DefaultStepKind::MAKE_NEW:
      this->makeEmptyNew(step.as<MakeNew>()->cls, run);
      return true;
    case DefaultStepKind::FILL_OBJECT:
      ClassDeclarer::fillObject(*step.as<FillObject>(), run);
      return true;
  }
  llvm_unreachable("unknown default step");
}

bool ClassDeclarer::defaultFor(const DefaultFor &wanted, DefaultRun &run) {
  auto &ctx = this->state.getAst();
  const Type *type = wanted.type;
  llvm::SMLoc loc = run.loc;
  if (!type || type->isError() || type->isParam()) {
    return false;
  }
  if (Expr *simple = this->simpleDefault(type, wanted.ref, loc)) {
    run.values.push_back(simple);
    return true;
  }
  const ClassDecl *cls = type->classDecl();
  if (cls) {
    if (cls->name.empty()) {
      return false;
    }
    run.steps.push_back(MakeNew{cls});
    run.steps.push_back(CheckEmpty{cls});
    return true;
  }
  if (!ClassDeclarer::isPlainObject(type)) {
    return false;
  }
  auto *object = ctx.make<ObjectLitExpr>(loc);
  std::vector<const Field *> fields;
  for (const Field &field : type->fields()) {
    if (!(field.type && field.type->isOptional())) {
      fields.push_back(&field);
    }
  }
  run.steps.push_back(FillObject{object, fields});
  for (const Field *field : llvm::reverse(fields)) {
    run.steps.push_back(DefaultFor{field->type, nullptr});
  }

  return true;
}

Expr *ClassDeclarer::simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc) {
  auto &ctx = this->state.getAst();
  if (type->isString()) {
    return ctx.make<StringLitExpr>(loc, u"");
  }
  if (type->isInteger() || type->isDecimal()) {
    return ctx.make<IntLitExpr>(loc, uint64_t(0));
  }
  if (type->isFloat()) {
    return ctx.make<FloatLitExpr>(loc, 0.0);
  }
  if (type->isBool()) {
    return ctx.make<BoolLitExpr>(loc, false);
  }
  if (type->isArray() && !type->isTuple()) {
    return ctx.make<ArrayLitExpr>(loc);
  }
  bool collection =
      (type->isMap() || type->isSet()) && ref && ref->kind() == TypeRefKind::GENERIC_REF;
  if (collection) {
    const char *name = type->isMap() ? "Map" : "Set";
    return ctx.make<NewExpr>(loc, name, ref->args, std::vector<Expr *>());
  }
  if (type->isLiteral()) {
    return this->literalValue(type, loc);
  }
  auto literal = [](const Type *member) { return member->isLiteral(); };
  bool literals =
      type->isUnion() && !type->members().empty() && llvm::all_of(type->members(), literal);
  if (literals) {
    return this->literalValue(type->members().front(), loc);
  }

  return nullptr;
}

bool ClassDeclarer::isPlainObject(const Type *type) {
  auto hidden = [](const Field &f) { return f.name.starts_with("%"); };

  return type->isObject() && !type->isTuple() && !type->isAnyObject() && !type->isDate() &&
         !type->isRegExp() && !type->isMatch() && !type->isIntl() && !type->isWeakRef() &&
         !type->isRuntimeObject() && !type->isBytes() && llvm::none_of(type->fields(), hidden);
}

bool ClassDeclarer::checkEmpty(const ClassDecl *cls, DefaultRun &run) {
  bool madeByConstructor = cls->isAbstract || cls->builtin() || cls->isGeneric() ||
                           cls->typeParams.size() || !ClassLinks::constructorsOf(cls).empty();
  if (madeByConstructor) {
    return false;
  }
  bool firstVisit = run.visiting.insert(cls).second;
  if (!firstVisit) {
    return false;
  }
  run.steps.push_back(LeaveEmpty{cls});
  run.steps.push_back(ResolveFamily{cls});

  return true;
}

void ClassDeclarer::resolveFamily(const ClassDecl *c, DefaultRun &run) {
  this->resolveClass(const_cast<ClassDecl *>(c));
  if (c->parent) {
    run.steps.push_back(ResolveFamily{c->parent});
  }
  std::vector<const ClassDecl *> inner;
  for (const ClassField &f : c->fields) {
    bool skipped = f.isStatic() || f.init || f.method() || f.optional() || !f.resolved;
    const ClassDecl *held = skipped ? nullptr : f.resolved->classDecl();
    if (held) {
      inner.push_back(held);
    }
  }
  for (const ClassDecl *held : llvm::reverse(inner)) {
    run.steps.push_back(CheckEmpty{held});
  }
}

void ClassDeclarer::makeEmptyNew(const ClassDecl *cls, DefaultRun &run) {
  auto *made = this->state.getAst().make<NewExpr>(run.loc, cls->name, std::vector<TypeRef>(),
                                                  std::vector<Expr *>());
  run.values.push_back(made);
}

void ClassDeclarer::fillObject(const FillObject &fill, DefaultRun &run) {
  std::vector<Expr *> values = ChildrenFirst::takeAnswers(run.values, fill.fields.size());
  for (size_t i = 0; i < values.size(); ++i) {
    ObjectLitExpr::Property property;
    property.name = fill.fields[i]->name;
    property.loc = run.loc;
    property.value = values[i];
    fill.object->props.push_back(property);
  }
  run.values.push_back(fill.object);
}

void ClassDeclarer::checkVariantList(llvm::ArrayRef<FunctionDecl *> functions) {
  for (size_t i = 0; i < functions.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      bool sameName = functions[j]->name == functions[i]->name &&
                      functions[j]->isStatic() == functions[i]->isStatic();
      if (sameName) {
        this->checkVariantPair(functions[j], functions[i]);
      }
    }
  }
}

void ClassDeclarer::checkVariants(ClassDecl *cls) {
  this->checkVariantList(cls->methods);
  this->checkVariantList(cls->constructors);
}

int ClassDeclarer::ambiguousCount(const FuncSymbol *x, const FuncSymbol *y) {
  size_t fixedX = x->params.size() - x->hasRest;
  size_t fixedY = y->params.size() - y->hasRest;
  size_t last = std::min(fixedX, fixedY);
  for (size_t n = std::max(x->minArgs, y->minArgs); n <= last; ++n) {
    bool same = true;
    for (size_t k = 0; k < n && same; ++k) {
      same = x->params[k] == y->params[k];
    }
    if (same) {
      return int(n);
    }
  }

  return -1;
}

ClassDeclarer::ResultVariantsUnequal ClassDeclarer::variantPair(const FunctionDecl *a,
                                                                const FunctionDecl *b) {
  const FuncSymbol *x = a->sym;
  const FuncSymbol *y = b->sym;
  bool typed =
      x && y && !llvm::is_contained(x->params, nullptr) && !llvm::is_contained(y->params, nullptr);
  bool sameParams =
      typed && x->params == y->params && x->minArgs == y->minArgs && x->hasRest == y->hasRest;
  if (sameParams && x->hasReturnType() && y->hasReturnType() &&
      x->getKnownReturnType() != y->getKnownReturnType()) {
    std::string message = ErrorTextVariantPair::returnOnly(a->isConstructor(), a->name);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::ONLY_RETURN_DIFFERS,
        .message = message,
    });
  }
  if (sameParams) {
    std::string variant = ErrorTextVariant::text(b);
    std::string message = ErrorTextVariantPair::duplicate(variant);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::SAME_PARAMETERS,
        .message = message,
    });
  }
  int count = typed ? ClassDeclarer::ambiguousCount(x, y) : -1;
  if (count >= 0) {
    std::string first = ErrorTextVariant::text(a);
    std::string second = ErrorTextVariant::text(b);
    std::string message = ErrorTextVariantPair::ambiguous(first, second, count);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::SAME_FOR_COUNT,
        .message = message,
    });
  }

  return ok(b);
}

void ClassDeclarer::checkVariantPair(FunctionDecl *a, FunctionDecl *b) {
  ResultVariantsUnequal unequal = ClassDeclarer::variantPair(a, b);
  if (unequal.isErr()) {
    std::string other = ErrorTextVariantPair::other();
    this->state.getDiagnostics().error(b->loc(), unequal.error().message);
    this->state.getDiagnostics().note(a->loc(), other);
  }
}

ClassDeclarer::ResultNotParentMethod ClassDeclarer::parentMethod(const ClassDecl *cls,
                                                                 const ClassField &f) {
  bool clash = cls->parent && !ClassDeclarer::methodsNamed(cls->parent, f.name, false).empty();
  if (clash) {
    std::string message = ErrorTextParentMethod::clash(f.name, cls->parent->name);

    return err(ClassDeclarerParentMethodError{
        .code = ClassDeclarerParentMethodErrorCode::PARENT_METHOD,
        .message = message,
    });
  }

  return ok(&f);
}

ClassDeclarer::ResultNarrowingLayout ClassDeclarer::narrowedField(const ClassDecl *cls,
                                                                  const ClassField &f,
                                                                  const Field &inherited,
                                                                  const ClassDecl *owner) {
  bool otherLayout =
      inherited.type != f.resolved && !WideningRelations::sameReference(f.resolved, inherited.type);
  if (otherLayout) {
    std::string message = ErrorTextNarrowedField::layout(f.name, cls->name, f.resolved->name(),
                                                         owner->name, inherited.type->name());

    return err(ClassDeclarerNarrowedFieldError{
        .code = ClassDeclarerNarrowedFieldErrorCode::OTHER_LAYOUT,
        .message = message,
    });
  }

  return ok(&f);
}

void ClassDeclarer::completeClass(ClassDecl *cls) {
  std::vector<CompletionStep> steps = {StartCompletion{cls}};
  while (!steps.empty()) {
    CompletionStep step = steps.back();
    steps.pop_back();
    if (const StartCompletion *begin = step.as<StartCompletion>()) {
      this->startCompletion(begin->cls, steps);
    } else {
      this->finishCompletion(step.as<FinishCompletion>()->cls);
    }
  }
}

void ClassDeclarer::startCompletion(ClassDecl *cls, std::vector<CompletionStep> &steps) {
  bool incomplete = cls->type->isIncomplete() && !cls->literal();
  if (!incomplete || this->state.getClassStore().isCompleting(cls)) {
    return;
  }
  this->state.getClassStore().enterCompletion(cls);
  steps.push_back(FinishCompletion{cls});
  if (ClassDecl *generic = cls->generic()) {
    steps.push_back(StartCompletion{generic});
  }
  if (cls->parent) {
    steps.push_back(StartCompletion{cls->parent});
  }
}

void ClassDeclarer::finishCompletion(ClassDecl *cls) {
  auto *type = const_cast<Type *>(cls->type);
  bool muted = false;
  ClassDecl *generic = cls->generic();
  if (generic) {
    for (size_t i = 0; i < cls->fields.size() && i < generic->fields.size(); ++i) {
      ClassField &f = cls->fields[i];
      const Type *original = generic->fields[i].resolved;
      if (f.isStatic() || f.method() || f.resolved) {
        continue;
      }
      f.resolved =
          original ? this->genericClassChecker->instanceValueType(cls, original) : Type::error();
    }
    muted = this->state.getDiagnostics().setMuted(true);
  }
  std::vector<Field> fields;
  if (cls->parent) {
    fields = cls->parent->type->fields();
  }
  for (ClassField &f : cls->fields) {
    if (f.isStatic() || f.method()) {
      continue;
    }
    if (!f.resolved) {
      f.resolved = Type::error();
    }
    if (!f.redeclared) {
      ResultNotParentMethod own = ClassDeclarer::parentMethod(cls, f);
      if (own.isErr()) {
        this->state.getDiagnostics().error(f.loc, own.error().message);
      }
      fields.push_back({f.name.str(), f.resolved, f.readonly});
      continue;
    }
    auto sameName = [&](const Field &g) { return g.name == f.name; };
    auto it = llvm::find_if(fields, sameName);
    if (it == fields.end() || f.resolved->isError() || it->type->isError()) {
      continue;
    }
    const ClassDecl *owner = nullptr;
    ClassBases::findField(cls->parent, f.name, &owner);
    ResultNarrowingLayout layout = ClassDeclarer::narrowedField(cls, f, *it, owner);
    if (layout.isErr()) {
      this->state.getDiagnostics().error(f.loc, layout.error().message);
      continue;
    }
    if (it->type != f.resolved || (f.readonly && !it->readonly)) {
      this->state.getWideningStore().markFieldsNarrowed();
    }
    it->type = f.resolved;
    it->readonly = f.readonly;
  }
  type->defineFields(std::move(fields));
  this->state.getClassStore().leaveCompletion(cls);
  if (cls->generic()) {
    this->state.getDiagnostics().setMuted(muted);
  }
}

}
