#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_check_helpers/class_check_helpers.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/wide_writes/wide_writes.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

namespace tz {

const Type *literalType(const Expr *value);

ClassDeclarer::ClassDeclarer(Checker &owner)
    : checker(owner) {}

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
  cls->id = ++this->checker.classCount_;
  cls->type = Type::classType(cls->name, cls);
  this->checker.classes_.push_back(cls);
  Scopes::ResultEntryDeclared added =
      this->checker.scopes().declare(cls->name, cls->loc(), {ClassEntry{.cls = cls}});
  if (added.isErr()) {
    this->checker.diags_.error(cls->loc(), added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->checker.scopes().lookup(cls->name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->checker.diags_.note(previousEntry.loc(), declaredHere);
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
    this->checker.diags_.error(f.loc, unique.error().message);
    this->checker.diags_.note(earlier->loc, previous);
  }
  for (FunctionDecl *fn : cls->methods) {
    ResultFieldNameIsFree distinct = ClassDeclarer::fieldOrMethod(cls, f, fn);
    if (distinct.isErr()) {
      std::string field = ErrorTextFieldOrMethod::field();
      this->checker.diags_.error(fn->loc(), distinct.error().message);
      this->checker.diags_.note(f.loc, field);
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

ClassDeclarer::ResultFieldTypeSource ClassDeclarer::typedField(const ClassField &f) {
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
    if (param->type) {
      return this->checker.typeResolver().resolveType(*param->type, false);
    }
    if (param->defaultValue()) {
      return literalType(param->defaultValue());
    }
    return nullptr;
  }
  if (f.type) {
    return this->checker.typeResolver().resolveType(*f.type, false);
  }

  return literalType(f.init);
}

void ClassDeclarer::arrowMethod(ClassDecl *cls, ClassField &f, const Type *type) {
  auto *function = llvm::dyn_cast_or_null<FunctionExpr>(f.init);
  if (function && function->fn->name.empty()) {
    function->fn->name = f.name;
  }
  bool method = function && function->fn->isArrow() && !f.isStatic() && !f.parameter() &&
                !f.optional() && !f.redeclared && !this->checker.assignedFields_.contains(f.name) &&
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
  if (f.isStatic() && type && !cls->typeArgs.empty() && mentionsParam(type)) {
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
  VarSymbol *var = this->checker.ctx_.newVar();
  std::string name = (cls->name + "." + f.name).str();
  var->name = this->checker.ctx_.keepName(name);
  var->loc = f.loc;
  var->type = type;
  var->isConst = f.readonly;
  var->place = GlobalVar{.usedInFunction = true};
  shared->var = var;
  auto *literal = f.readonly ? llvm::dyn_cast_or_null<IntLitExpr>(f.init) : nullptr;
  bool fitsInt = literal && literal->value <= uint64_t(std::numeric_limits<int64_t>::max());
  if (fitsInt) {
    this->checker.constVars_[var] = int64_t(literal->value);
  }
  ResultStaticGetsValue valued = ClassDeclarer::staticValue(f, type);
  if (valued.isErr()) {
    this->checker.diags_.error(f.loc, valued.error().message);
  }
}

void ClassDeclarer::resolveField(ClassDecl *cls, ClassField &f) {
  ResultFieldNotLateSet early = ClassDeclarer::setLater(f);
  if (early.isErr()) {
    this->checker.diags_.error(f.loc, early.error().message);
  }
  const Type *type = nullptr;
  ResultFieldTypeSource source = ClassDeclarer::typedField(f);
  if (source.isErr()) {
    this->checker.diags_.error(f.loc, source.error().message);
    type = Type::error();
  } else {
    type = this->declaredFieldType(cls, f);
  }
  if (type && f.optional()) {
    type = Type::optional(type);
  }
  f.resolved = type;
  this->arrowMethod(cls, f, type);

  ResultStaticTypeFixed fixed = ClassDeclarer::staticTypeParams(cls, f, type);
  if (fixed.isErr()) {
    this->checker.diags_.error(f.loc, fixed.error().message);
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
  this->checker.genericClassChecker().declareClassTypeParams(cls);
  this->checker.typeScopes_.emplace_back();
  this->checker.genericClassChecker().bindClassTypeParams(cls);
  this->checker.classBases().resolveClassBases(cls);
  for (size_t i = 0; i < cls->fields.size(); ++i) {
    this->checkMemberName(cls, i);
  }
  for (ClassField &f : cls->fields) {
    this->resolveField(cls, f);
  }
  this->checker.typeScopes_.pop_back();
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
  FuncSymbol *sym = this->checker.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    const Type *given = i < declared->params().size() ? declared->params()[i] : nullptr;
    const Type *written =
        param.type ? this->checker.typeResolver().resolveType(*param.type, false) : given;
    ResultParamTypePassed passed = ClassDeclarer::paramType(param, written, declared);
    if (passed.isErr()) {
      this->checker.diags_.error(param.loc, passed.error().message);
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
    hidden.name = this->checker.ctx_.hiddenName();
    hidden.loc = fn->loc();
    hidden.declaredType = declared->params()[i];
    fn->params.push_back(hidden);
    sym->params.push_back(declared->params()[i]);
  }
  sym->minArgs = declared->minArgs();
  sym->hasRest = declared->hasRest();
  sym->ret = declared->ret();
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
    this->checker.functionChecker().declareFunction(ctor, nullptr, false);
    ctor->sym->ret = Type::voidTy();
  }
  for (FunctionDecl *fn : cls->methods) {
    this->checker.functionChecker().declareFunction(fn, nullptr, false);
    ResultAbstractReturns returns = ClassDeclarer::abstractReturn(cls, fn);
    if (returns.isErr()) {
      this->checker.diags_.error(fn->loc(), returns.error().message);
    } else if (fn->isAbstract() && !fn->returnType) {
      fn->sym->ret = nullptr;
    }
    ResultGenericIsDirect direct = ClassDeclarer::genericMethod(fn);
    if (direct.isErr()) {
      this->checker.diags_.error(fn->loc(), direct.error().message);
    }
  }
  for (ClassField &f : cls->fields) {
    if (!f.method()) {
      continue;
    }
    FunctionDecl *fn = f.method();
    this->checker.typeScopes_.emplace_back();
    this->checker.genericClassChecker().bindClassTypeParams(cls);
    const Type *declared =
        f.type ? this->checker.typeResolver().resolveType(*f.type, false) : nullptr;
    this->checker.typeScopes_.pop_back();
    if (!declared || !declared->isFunction()) {
      this->checker.functionChecker().declareFunction(fn, nullptr, false);
      continue;
    }
    this->declareTyped(fn, declared);
  }

  if (ClassLinks::constructorsOf(cls).empty()) {
    for (ClassField &f : cls->fields) {
      bool skipped = f.isStatic() || f.init || f.method() || f.optional() || f.parameter() ||
                     f.definite() || !f.resolved;
      if (skipped) {
        continue;
      }
      llvm::SmallPtrSet<const ClassDecl *, 4> visiting;
      visiting.insert(cls);
      const TypeRef *ref = f.type ? &*f.type : nullptr;
      f.init = this->defaultValue(f.resolved, ref, f.loc, visiting);
    }
  }
  std::vector<Stmt *> sets;
  for (const ClassField &f : cls->fields) {
    if (f.isStatic() || !f.init || f.method()) {
      continue;
    }
    auto *self = this->checker.ctx_.make<NameExpr>(f.loc, "this");
    auto *target = this->checker.ctx_.make<MemberExpr>(f.loc, self, f.name);
    auto *set = this->checker.ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
    sets.push_back(this->checker.ctx_.make<ExprStmt>(f.loc, set));
  }
  if (!sets.empty()) {
    auto *init = this->checker.ctx_.make<FunctionDecl>(cls->loc(), "init");
    init->role = ClassMember{.cls = cls};
    init->body = this->checker.ctx_.make<BlockStmt>(cls->loc());
    init->body->body = std::move(sets);
    this->checker.functionChecker().declareFunction(init, nullptr, false);
    init->sym->ret = Type::voidTy();
    cls->init = init;
  }

  ClassDeclarer::numberVariants(cls->methods);
  ClassDeclarer::numberVariants(cls->constructors);
  this->checkVariants(cls);
}

Expr *ClassDeclarer::literalValue(const Type *literal, llvm::SMLoc loc) {
  const Type *base = literal->inner();
  if (base && base->isString()) {
    return this->checker.ctx_.make<StringLitExpr>(loc, literal->literalText());
  }
  if (base && base->isBool()) {
    return this->checker.ctx_.make<BoolLitExpr>(loc, literal->literalNumber() != 0);
  }
  double number = literal->literalNumber();
  double size = number < 0 ? -number : number;
  if (base && base->isFloat()) {
    Expr *value = this->checker.ctx_.make<FloatLitExpr>(loc, size);
    return number < 0 ? this->checker.ctx_.make<UnaryExpr>(loc, UnaryOp::Neg, value) : value;
  }
  auto magnitude = static_cast<uint64_t>(size);
  Expr *value = this->checker.ctx_.make<IntLitExpr>(loc, magnitude);

  return number < 0 ? this->checker.ctx_.make<UnaryExpr>(loc, UnaryOp::Neg, value) : value;
}

bool ClassDeclarer::startsEmpty(const ClassDecl *cls,
                                llvm::SmallPtrSetImpl<const ClassDecl *> &visiting) {
  bool madeByConstructor = cls->isAbstract || cls->builtin() || cls->isGeneric() ||
                           cls->typeParams.size() || !ClassLinks::constructorsOf(cls).empty();
  if (madeByConstructor) {
    return false;
  }
  bool firstVisit = visiting.insert(cls).second;
  if (!firstVisit) {
    return false;
  }
  for (const ClassDecl *c = cls; c; c = c->parent) {
    this->resolveClass(const_cast<ClassDecl *>(c));
    for (const ClassField &f : c->fields) {
      if (f.isStatic() || f.init || f.method() || f.optional() || !f.resolved) {
        continue;
      }
      const ClassDecl *inner = f.resolved->classDecl();
      if (inner && !this->startsEmpty(inner, visiting)) {
        return false;
      }
    }
  }
  visiting.erase(cls);

  return true;
}

Expr *ClassDeclarer::defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                                  llvm::SmallPtrSetImpl<const ClassDecl *> &visiting) {
  auto &ctx = this->checker.ctx_;
  if (!type || type->isError() || type->isParam()) {
    return nullptr;
  }
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
  const ClassDecl *cls = type->classDecl();
  if (cls) {
    if (cls->name.empty() || !this->startsEmpty(cls, visiting)) {
      return nullptr;
    }
    return ctx.make<NewExpr>(loc, cls->name, std::vector<TypeRef>(), std::vector<Expr *>());
  }
  auto hidden = [](const Field &f) { return f.name.starts_with("%"); };
  bool plainObject = type->isObject() && !type->isTuple() && !type->isAnyObject() &&
                     !type->isDate() && !type->isRegExp() && !type->isMatch() && !type->isIntl() &&
                     !type->isWeakRef() && !type->isRuntimeObject() && !type->isBytes() &&
                     llvm::none_of(type->fields(), hidden);
  if (!plainObject) {
    return nullptr;
  }
  auto *object = ctx.make<ObjectLitExpr>(loc);
  for (const Field &field : type->fields()) {
    if (field.type && field.type->isOptional()) {
      continue;
    }
    Expr *value = this->defaultValue(field.type, nullptr, loc, visiting);
    if (!value) {
      return nullptr;
    }
    ObjectLitExpr::Property property;
    property.name = field.name;
    property.loc = loc;
    property.value = value;
    object->props.push_back(property);
  }

  return object;
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
  if (sameParams && x->ret && y->ret && x->ret != y->ret) {
    std::string message = ErrorTextVariantPair::returnOnly(a->isConstructor(), a->name);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::ONLY_RETURN_DIFFERS,
        .message = message,
    });
  }
  if (sameParams) {
    std::string variant = variantText(b);
    std::string message = ErrorTextVariantPair::duplicate(variant);

    return err(ClassDeclarerVariantPairError{
        .code = ClassDeclarerVariantPairErrorCode::SAME_PARAMETERS,
        .message = message,
    });
  }
  int count = typed ? ClassDeclarer::ambiguousCount(x, y) : -1;
  if (count >= 0) {
    std::string first = variantText(a);
    std::string second = variantText(b);
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
    this->checker.diags_.error(b->loc(), unequal.error().message);
    this->checker.diags_.note(a->loc(), other);
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
      inherited.type != f.resolved && !WideWrites::sameReference(f.resolved, inherited.type);
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
  auto *type = const_cast<Type *>(cls->type);
  if (!type->isIncomplete() || cls->literal()) {
    return;
  }
  bool firstVisit = this->completing.insert(cls).second;
  if (!firstVisit) {
    return;
  }
  if (cls->parent) {
    this->completeClass(cls->parent);
  }
  bool muted = false;
  ClassDecl *generic = cls->generic();
  if (generic) {
    this->completeClass(generic);
    for (size_t i = 0; i < cls->fields.size() && i < generic->fields.size(); ++i) {
      ClassField &f = cls->fields[i];
      const Type *original = generic->fields[i].resolved;
      if (f.isStatic() || f.method() || f.resolved) {
        continue;
      }
      f.resolved = original ? this->checker.genericClassChecker().instanceValueType(cls, original)
                            : Type::error();
    }
    muted = this->checker.diags_.setMuted(true);
  }
  for (ClassField &f : cls->fields) {
    if (!f.parameter() || f.resolved) {
      continue;
    }
    for (FunctionDecl *ctor : cls->constructors) {
      if (f.resolved || !ctor->sym) {
        continue;
      }
      this->checker.returnInferrer().resolveParams(ctor->sym);
      for (const Param &param : ctor->params) {
        if (param.property && param.name == f.name && param.declaredType) {
          f.resolved = param.declaredType;
        }
      }
    }
    if (!f.resolved) {
      f.resolved = Type::error();
    }
  }
  auto unresolved = [](const ClassField &f) { return !f.isStatic() && !f.method() && !f.resolved; };
  bool pending = llvm::any_of(cls->fields, unresolved);
  if (pending && cls->init && !cls->init->checked) {
    Checker::BodyState saved = this->checker.returnInferrer().enterBody(cls->init);
    this->checker.flow_ = Checker::Flow();
    this->checker.functionChecker().checkFunctionBody(cls->init);
    this->checker.returnInferrer().leaveBody(saved);
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
        this->checker.diags_.error(f.loc, own.error().message);
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
      this->checker.diags_.error(f.loc, layout.error().message);
      continue;
    }
    this->checker.narrowedFields_ |= it->type != f.resolved || (f.readonly && !it->readonly);
    it->type = f.resolved;
    it->readonly = f.readonly;
  }
  type->defineFields(std::move(fields));
  this->completing.erase(cls);
  if (cls->generic()) {
    this->checker.diags_.setMuted(muted);
  }
}

}
