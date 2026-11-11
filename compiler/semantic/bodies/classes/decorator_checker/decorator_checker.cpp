#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

DecoratorChecker::DecoratorChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->lateClasses = &all.lateClasses;
  this->stmtChecker = &all.stmtChecker;
  this->typeRelations = &all.typeRelations;
  this->metadataChecker = &all.metadataChecker;
  this->classValueChecker = &all.classValueChecker;
}

size_t DecoratorChecker::argumentsOf(Place place) {
  switch (place) {
  case Place::Class:
    return 1;
  case Place::Field:
    return 2;
  default:
    return 3;
  }
}

std::string DecoratorChecker::placeText(Place place) {
  switch (place) {
  case Place::Class:
    return ErrorTextDecoratorPlace::aClass();
  case Place::Field:
    return ErrorTextDecoratorPlace::aField();
  case Place::Method:
    return ErrorTextDecoratorPlace::aMethod();
  default:
    return ErrorTextDecoratorPlace::aParameter();
  }
}

std::string DecoratorChecker::argumentsText(Place place) {
  switch (place) {
  case Place::Class:
    return ErrorTextDecoratorPlace::classArgs();
  case Place::Field:
    return ErrorTextDecoratorPlace::fieldArgs();
  case Place::Method:
    return ErrorTextDecoratorPlace::methodArgs();
  default:
    return ErrorTextDecoratorPlace::paramArgs();
  }
}

std::string DecoratorChecker::decoratorName(const Expr *expr) {
  std::vector<llvm::StringRef> names;
  const Expr *link = DecoratorChecker::withoutCalls(expr);
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    names.push_back(member->name);
    link = DecoratorChecker::withoutCalls(member->object);
  }
  auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr || root->name.starts_with("%")) {
    return "";
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

const Expr *DecoratorChecker::withoutCalls(const Expr *expr) {
  const Expr *callee = expr;
  while (auto *call = dyn_cast<CallExpr>(callee)) {
    callee = call->callee;
  }

  return callee;
}

bool DecoratorChecker::isPlainReference(const Expr *expr) {
  const Expr *link = expr;
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    link = member->object;
  }

  return isa<NameExpr>(link);
}

bool DecoratorChecker::decoratedParams(const FunctionDecl *fn) {
  return llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *ret) {
  const Type *value = Type::function({Type::unknown(), args}, 1, true, ret, true);
  const Type *flag = Type::boolTy();
  const Type *type =
      Type::object({{"value", value, false},
                    {"writable", flag, false},
                    {"enumerable", flag, false},
                    {"configurable", flag, false}},
                   "MethodDescriptor<" + args->name().str() + ", " + ret->name().str() + ">");
  this->state.getAliasStore().rememberDescriptorOrigin(type, args, ret);

  return type;
}

bool DecoratorChecker::decoratorParams(Expr *callee, std::vector<const Type *> &params,
                                       size_t &required) {
  const FuncSymbol *sym = nullptr;
  if (auto *name = dyn_cast<NameExpr>(callee)) {
    ScopeEntry entry = this->scopes->lookup(name->name);
    if (entry.var()) {
      const Type *type = entry.var()->type;
      if (!type || !type->isFunction() || type->hasRest()) {
        return false;
      }
      params.assign(type->params().begin(), type->params().end());
      required = type->minArgs();
      return true;
    }
    sym = entry.func();
  } else if (auto *member = dyn_cast<MemberExpr>(callee)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? this->scopes->lookup(object->name).cls() : nullptr) {
      std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, member->name, true);
      if (methods.size() == 1) {
        sym = methods[0]->sym;
      }
    }
  }
  if (!sym) {
    return false;
  }
  if (sym->hasRest || llvm::is_contained(sym->params, nullptr)) {
    return false;
  }
  params = sym->params;
  required = sym->minArgs;

  return true;
}

DecoratorChecker::ResultNamesAreDistinct DecoratorChecker::namesDistinct(const DecoratedName &a,
                                                                         const DecoratedName &b,
                                                                         llvm::StringRef cls) {
  if (a.name == b.name) {
    std::string what = a.isStatic != b.isStatic ? ErrorTextDecoratedNames::bothKinds(a.name)
                                                : ErrorTextDecoratedNames::variants(a.name);
    std::string shared;
    for (const std::string &d : b.decorators) {
      if (!d.empty() && llvm::is_contained(a.decorators, d)) {
        shared = d;
      }
    }
    std::string records = shared.empty() ? ErrorTextDecoratedNames::design()
                                         : ErrorTextDecoratedNames::recordsOf(shared);
    std::string message = ErrorTextDecoratedNames::shared(what, cls, records);

    return err(DecoratorCheckerNamesDistinctError{
        .code = DecoratorCheckerNamesDistinctErrorCode::SAME_NAME,
        .message = message,
    });
  }

  return ok(&b);
}

DecoratorChecker::ResultFirstConstructor
DecoratorChecker::secondConstructor(const ClassDecl *cls, const FunctionDecl *first) {
  if (first) {
    std::string message = ErrorTextDecoratedNames::sameRecord(cls->name);

    return err(DecoratorCheckerSecondConstructorError{
        .code = DecoratorCheckerSecondConstructorErrorCode::TWO_DECORATED_CONSTRUCTORS,
        .message = message,
    });
  }

  return ok(cls);
}

void DecoratorChecker::checkDecoratedNames(ClassDecl *cls) {
  std::vector<DecoratedName> members;
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc, bool isStatic,
                 llvm::ArrayRef<Decorator> decorators, const FunctionDecl *fn) {
    DecoratedName m{name, loc, isStatic, {}};
    for (const Decorator &d : decorators) {
      m.decorators.push_back(DecoratorChecker::decoratorName(d.expr));
    }
    if (fn) {
      for (const Param &p : fn->params) {
        for (const Decorator &d : p.decorators) {
          m.decorators.push_back(DecoratorChecker::decoratorName(d.expr));
        }
      }
    }
    if (!m.decorators.empty()) {
      members.push_back(std::move(m));
    }
  };
  for (const ClassField &f : cls->fields) {
    add(f.name, f.loc, f.isStatic(), f.decorators, nullptr);
  }
  for (const FunctionDecl *fn : cls->methods) {
    add(fn->name, fn->loc(), fn->isStatic(), fn->decorators(), fn);
  }
  for (size_t i = 0; i < members.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      const DecoratedName &a = members[j];
      const DecoratedName &b = members[i];
      ResultNamesAreDistinct distinct = DecoratorChecker::namesDistinct(a, b, cls->name);
      if (distinct.isOk()) {
        continue;
      }
      this->state.getDiagnostics().error(b.loc, distinct.error().message);
      std::string other = ErrorTextDecoratedNames::other();
      this->state.getDiagnostics().note(a.loc, other);
      break;
    }
  }
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *ctor : cls->constructors) {
    if (!DecoratorChecker::decoratedParams(ctor)) {
      continue;
    }
    ResultFirstConstructor only = DecoratorChecker::secondConstructor(cls, first);
    if (only.isOk()) {
      first = ctor;
      continue;
    }
    this->state.getDiagnostics().error(ctor->loc(), only.error().message);
    std::string other = ErrorTextDecoratedNames::other();
    this->state.getDiagnostics().note(first->loc(), other);
    break;
  }
}

void DecoratorChecker::decorateClass(ClassDecl *cls) {
  this->checkDecoratedNames(cls);
  struct Member {
    ClassField *field;
    FunctionDecl *method;
    bool isStatic;
    const char *at;
  };
  std::vector<Member> members;
  for (ClassField &f : cls->fields) {
    if (!f.decorators.empty()) {
      members.push_back({&f, nullptr, f.isStatic(), f.loc.getPointer()});
    }
  }
  for (FunctionDecl *fn : cls->methods) {
    if (!fn->decorators().empty() || DecoratorChecker::decoratedParams(fn)) {
      members.push_back({nullptr, fn, fn->isStatic(), fn->loc().getPointer()});
    }
  }
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    std::vector<DecoratorUse> uses;
    for (const Decorator &d : m.field ? m.field->decorators : m.method->decorators()) {
      uses.push_back({&d});
    }
    if (m.method) {
      for (size_t i = 0; i < m.method->params.size(); ++i) {
        for (const Decorator &d : m.method->params[i].decorators) {
          uses.push_back({&d, int(i), m.method});
        }
      }
    }
    this->decorate(cls, m.field, m.method, uses);
  }
  std::vector<DecoratorUse> uses;
  for (const Decorator &d : cls->decorators) {
    uses.push_back({&d});
  }
  for (const FunctionDecl *ctor : cls->constructors) {
    for (size_t i = 0; i < ctor->params.size(); ++i) {
      for (const Decorator &d : ctor->params[i].decorators) {
        uses.push_back({&d, int(i), ctor});
      }
    }
  }
  this->decorate(cls, nullptr, nullptr, uses);
}

DecoratorChecker::ResultPlainMemberFound
DecoratorChecker::memberDecoratable(const ClassField *field, const FunctionDecl *method) {
  if (field && field->accessor) {
    std::string message = ErrorTextDecorate::accessor();

    return err(DecoratorCheckerMemberDecoratableError{
        .code = DecoratorCheckerMemberDecoratableErrorCode::ACCESSOR_FIELD,
        .message = message,
    });
  }
  if (method && method->isAbstract()) {
    std::string message = ErrorTextDecorate::abstract();

    return err(DecoratorCheckerMemberDecoratableError{
        .code = DecoratorCheckerMemberDecoratableErrorCode::ABSTRACT_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

DecoratorChecker::ResultDecoratorArgsFit DecoratorChecker::decoratorFits(llvm::StringRef label,
                                                                         Place place, size_t needs,
                                                                         size_t given,
                                                                         const Type *third) {
  if (needs > given) {
    std::string needed = std::to_string(needs);
    std::string gets = std::to_string(given);
    std::string where = DecoratorChecker::placeText(place);
    std::string args = DecoratorChecker::argumentsText(place);
    std::string message = ErrorTextDecorate::arguments(label, where, needed, gets, args);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  if (third && place == Place::Parameter && third->isObject() && third->field("value")) {
    std::string message = ErrorTextDecorate::descriptor(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::DESCRIPTOR_FOR_PARAMETER,
        .message = message,
    });
  }
  if (third && place == Place::Method && third->isNumeric()) {
    std::string message = ErrorTextDecorate::position(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::POSITION_FOR_METHOD,
        .message = message,
    });
  }

  return ok(needs);
}

DecoratorChecker::ResultDescriptorUsable DecoratorChecker::genericDescriptor(const ClassDecl *cls,
                                                                             bool descriptor) {
  if (descriptor && cls->isGeneric()) {
    std::string message = ErrorTextDecorate::generic(cls->name);

    return err(DecoratorCheckerGenericDescriptorError{
        .code = DecoratorCheckerGenericDescriptorErrorCode::GENERIC_CLASS_DESCRIPTOR,
        .message = message,
    });
  }

  return ok(cls);
}

DecoratorChecker::ResultMetadataArgsPair DecoratorChecker::metadataArgs(const CallExpr *metadata) {
  if (metadata->args.size() != 2 || metadata->hasSpread()) {
    std::string message = ErrorTextDecorate::metadata();

    return err(DecoratorCheckerMetadataArgsError{
        .code = DecoratorCheckerMetadataArgsErrorCode::WRONG_METADATA_ARGUMENTS,
        .message = message,
    });
  }

  return ok(metadata);
}

DecoratorChecker::ResultDecoratorReturns
DecoratorChecker::decoratorResult(llvm::StringRef label, const Type *result, Place place,
                                  bool descriptor, const ClassDecl *current) {
  bool nothing = result->isVoid() || result->isUndefined() || result->isError();
  if (!nothing && place == Place::Class && result->isConstructor()) {
    std::string message = ErrorTextDecoratorResult::runtime(label, result->name(), current->name);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (!nothing && place == Place::Class) {
    std::string message = ErrorTextDecoratorResult::forClass(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::CLASS_RESULT,
        .message = message,
    });
  }
  if (!nothing && place == Place::Method && descriptor) {
    std::string message = ErrorTextDecoratorResult::forMethod(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::METHOD_RESULT,
        .message = message,
    });
  }
  if (!nothing) {
    std::string where = DecoratorChecker::placeText(place);
    std::string message = ErrorTextDecoratorResult::other(label, result->name(), where);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::OTHER_RESULT,
        .message = message,
    });
  }

  return ok(result);
}

void DecoratorChecker::decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                                llvm::ArrayRef<DecoratorUse> uses) {
  if (uses.empty()) {
    return;
  }
  DecorateRun run{
      .cls = cls,
      .field = field,
      .method = method,
      .uses = uses,
      .own = field != nullptr    ? Place::Field
             : method != nullptr ? Place::Method
                                 : Place::Class,
      .memberName = field != nullptr    ? field->name
                    : method != nullptr ? method->name
                                        : llvm::StringRef(),
      .at = uses[0].decorator->loc,
      .earlyDescriptor = nullptr,
      .callees = {},
      .takes = {},
      .descriptor = false,
      .descVar = nullptr,
      .descType = nullptr,
      .current = cls,
  };
  ResultPlainMemberFound plain = DecoratorChecker::memberDecoratable(field, method);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(run.at, plain.error().message);
    return;
  }
  this->hoistDecorators(run);
  if (!this->decoratorsFit(run)) {
    return;
  }
  ResultDescriptorUsable usable = DecoratorChecker::genericDescriptor(cls, run.descriptor);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(run.at, usable.error().message);
    return;
  }
  if (run.descriptor && !DecoratorChecker::hasKnownSignature(method->sym)) {
    return;
  }
  if (run.descriptor) {
    this->declareDescriptor(run);
  }
  const FunctionDecl *ctor = DecoratorChecker::designConstructor(run);
  this->metadataChecker->writeDesignRecords(cls, field, method, ctor, run.at);
  for (size_t i = uses.size(); i-- > 0;) {
    this->applyDecorator(run, i);
  }
  if (run.descVar != nullptr) {
    this->storeDescriptorValue(run);
  }
}

bool DecoratorChecker::hasKnownSignature(FuncSymbol *sym) {
  const Type *ret = sym->getReturnType();

  return ret != nullptr && !ret->isError() && !llvm::is_contained(sym->params, nullptr);
}

bool DecoratorChecker::isMetadataCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;

  return member != nullptr && member->name == "metadata" &&
         this->callChecker->isGlobalName(member->object, GlobalName::REFLECT);
}

const Type *DecoratorChecker::methodDescriptor(DecorateRun &run) {
  if (run.earlyDescriptor != nullptr || run.method == nullptr) {
    return run.earlyDescriptor;
  }
  FuncSymbol *sym = run.method->sym;
  if (!DecoratorChecker::hasKnownSignature(sym)) {
    return nullptr;
  }
  const Type *args = Type::tuple(sym->params);
  run.earlyDescriptor = this->methodDescriptorType(args, sym->getReturnType());

  return run.earlyDescriptor;
}

const Type *DecoratorChecker::factoryHint(DecorateRun &run, const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *name = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  bool factoryShape = name != nullptr && name->typeArgs.empty() && run.own == Place::Method &&
                      !run.cls->isGeneric();
  if (!factoryShape) {
    return nullptr;
  }
  const FuncSymbol *factory = this->scopes->lookup(name->name).func();
  if (factory == nullptr || factory->decl == nullptr || factory->decl->typeParams.empty()) {
    return nullptr;
  }
  const Type *descriptorType = this->methodDescriptor(run);
  if (descriptorType == nullptr) {
    return nullptr;
  }
  const Type *classRef = Type::classRef(run.cls, run.cls->name);

  return Type::function({classRef, Type::string(), descriptorType}, 3, false, Type::voidTy());
}

void DecoratorChecker::hoistDecorators(DecorateRun &run) {
  for (const DecoratorUse &use : run.uses) {
    Expr *expr = use.decorator->expr;
    if (this->isMetadataCall(expr)) {
      run.callees.push_back(nullptr);
      continue;
    }
    if (DecoratorChecker::isPlainReference(expr)) {
      run.callees.push_back(expr);
      continue;
    }
    Expr *hoisted = this->hoistDecorator(run, use);
    run.callees.push_back(hoisted);
  }
}

Expr *DecoratorChecker::hoistDecorator(DecorateRun &run, const DecoratorUse &use) {
  Expr *expr = use.decorator->expr;
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  auto *decl = this->state.getAst().make<VarDeclStmt>(use.decorator->loc, true);
  VarDeclarator d;
  d.name = hidden;
  d.loc = use.decorator->loc;
  d.init = expr;
  const Type *hint = use.param < 0 ? this->factoryHint(run, expr) : nullptr;
  if (hint != nullptr) {
    d.init = this->exprChecker->checkExpr(expr, hint);
    d.sym = this->scopes->declareVar(hidden, d.loc, d.init->type, true);
    decl->decls.push_back(std::move(d));
  } else {
    decl->decls.push_back(std::move(d));
    this->stmtChecker->checkStmt(decl);
  }
  run.cls->decorations.push_back(decl);

  return this->state.getAst().make<NameExpr>(use.decorator->loc, hidden);
}

bool DecoratorChecker::decoratorsFit(DecorateRun &run) {
  bool fitting = true;
  for (size_t i = 0; i < run.uses.size(); ++i) {
    fitting &= this->decoratorFitsAt(run, i);
  }

  return fitting;
}

bool DecoratorChecker::decoratorFitsAt(DecorateRun &run, size_t i) {
  const DecoratorUse &use = run.uses[i];
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  size_t given = DecoratorChecker::argumentsOf(place);
  if (run.callees[i] == nullptr) {
    run.takes.push_back(0);
    return true;
  }
  std::vector<const Type *> params;
  size_t required = 0;
  bool known = this->decoratorParams(run.callees[i], params, required);
  size_t count = known ? params.size() : given;
  size_t needs = known ? std::min(required, count) : given;
  std::string name = DecoratorChecker::decoratorName(use.decorator->expr);
  std::string label = ErrorTextDecoratorPlace::label(name);
  const Type *third = known && params.size() >= 3 ? params[2]->nonOptional() : nullptr;
  ResultDecoratorArgsFit fits = DecoratorChecker::decoratorFits(label, place, needs, given, third);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(use.decorator->loc, fits.error().message);
  }
  run.takes.push_back(std::min(count, given));
  run.descriptor |= place == Place::Method && count >= 3;

  return fits.isOk();
}

void DecoratorChecker::declareDescriptor(DecorateRun &run) {
  FuncSymbol *sym = run.method->sym;
  if (run.earlyDescriptor != nullptr) {
    run.descType = run.earlyDescriptor;
  } else {
    const Type *args = Type::tuple(sym->params);
    run.descType = this->methodDescriptorType(args, sym->getReturnType());
  }
  VarSymbol *impl = this->declareMethodImpl(run);
  run.method->member()->impl = impl;
  Expr *init = this->descriptorInit(run, impl);
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  run.descVar = this->scopes->declareVar(hidden, run.at, run.descType, false);
  run.descVar->reassigned = true;
  auto *decl = this->state.getAst().make<VarDeclStmt>(run.at, false);
  VarDeclarator d;
  d.name = run.descVar->name;
  d.loc = run.at;
  d.init = init;
  d.sym = run.descVar;
  decl->decls.push_back(std::move(d));
  run.cls->decorations.push_back(decl);
}

VarSymbol *DecoratorChecker::declareMethodImpl(const DecorateRun &run) {
  const FunctionDecl *method = run.method;
  VarSymbol *impl = this->state.getAst().newVar();
  std::string implName = "%" + run.cls->name.str() + "." + method->name.str() + "." +
                         std::to_string(method->variant());
  impl->name = this->state.getAst().keepName(implName);
  impl->loc = method->loc();
  impl->type = run.descType->field("value")->type;
  impl->place = GlobalVar{.usedInFunction = true};
  impl->reassigned = true;
  Scopes::ResultEntryDeclared added = this->scopes->declare(impl->name, {VarEntry{.var = impl}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(impl->loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->scopes->lookup(impl->name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }

  return impl;
}

Expr *DecoratorChecker::descriptorInit(const DecorateRun &run, const VarSymbol *impl) {
  llvm::SMLoc at = run.at;
  auto *lit = this->state.getAst().make<ObjectLitExpr>(at);
  Expr *value = this->state.getAst().make<NameExpr>(at, impl->name);
  DecoratorChecker::addProperty(lit, "value", value, at);
  Expr *writable = this->state.getAst().make<BoolLitExpr>(at, true);
  DecoratorChecker::addProperty(lit, "writable", writable, at);
  Expr *enumerable = this->state.getAst().make<BoolLitExpr>(at, false);
  DecoratorChecker::addProperty(lit, "enumerable", enumerable, at);
  Expr *configurable = this->state.getAst().make<BoolLitExpr>(at, true);
  DecoratorChecker::addProperty(lit, "configurable", configurable, at);
  Expr *checked = this->exprChecker->checkExpr(lit, run.descType);

  return this->typeRelations->coerce(checked, run.descType);
}

void DecoratorChecker::addProperty(ObjectLitExpr *lit, llvm::StringRef name, Expr *value,
                                   llvm::SMLoc at) {
  ObjectLitExpr::Property p;
  p.name = name;
  p.loc = at;
  p.value = value;
  lit->props.push_back(p);
}

const FunctionDecl *DecoratorChecker::designConstructor(const DecorateRun &run) {
  if (run.own != Place::Class) {
    return nullptr;
  }
  const FunctionDecl *ctor = nullptr;
  for (const FunctionDecl *c : run.cls->constructors) {
    if (DecoratorChecker::decoratedParams(c) || run.cls->constructors.size() == 1) {
      ctor = ctor != nullptr ? nullptr : c;
    }
  }

  return ctor;
}

void DecoratorChecker::applyDecorator(DecorateRun &run, size_t i) {
  const DecoratorUse &use = run.uses[i];
  if (run.callees[i] == nullptr) {
    this->applyMetadata(run, use);
    return;
  }
  llvm::SMLoc loc = use.decorator->loc;
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  std::vector<Expr *> args = this->decoratorArgs(run, use, place);
  args.resize(std::min(args.size(), run.takes[i]));
  auto *call = this->state.getAst().make<CallExpr>(loc, run.callees[i], std::move(args));
  Expr *checked = this->exprChecker->checkExpr(call);
  Expr *applied = this->decoratorOutcome(run, use, place, checked);
  run.cls->decorations.push_back(this->state.getAst().make<ExprStmt>(loc, applied));
}

void DecoratorChecker::applyMetadata(DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  auto *metadata = cast<CallExpr>(use.decorator->expr);
  ResultMetadataArgsPair pair = DecoratorChecker::metadataArgs(metadata);
  if (pair.isErr()) {
    this->state.getDiagnostics().error(loc, pair.error().message);
    return;
  }
  auto *reflect = this->state.getAst().make<NameExpr>(loc, "Reflect");
  auto *callee = this->state.getAst().make<MemberExpr>(loc, reflect, "defineMetadata");
  Expr *target = this->classValueChecker->classValueAt(run.cls, loc);
  std::vector<Expr *> args{metadata->args[0], metadata->args[1], target};
  llvm::StringRef name = use.param >= 0 ? use.owner->name : run.memberName;
  if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor())) {
    args.push_back(
        this->state.getAst().make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
  }
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, args);
  Expr *checked = this->exprChecker->checkExpr(call);
  run.cls->decorations.push_back(this->state.getAst().make<ExprStmt>(loc, checked));
}

std::vector<Expr *> DecoratorChecker::decoratorArgs(const DecorateRun &run, const DecoratorUse &use,
                                                    Place place) {
  llvm::SMLoc loc = use.decorator->loc;
  ClassDecl *target = place == Place::Class ? run.current : run.cls;
  std::vector<Expr *> args{this->classValueChecker->classValueAt(target, loc)};
  if (place != Place::Class) {
    llvm::StringRef name = use.param >= 0 ? use.owner->name : run.memberName;
    if (use.param >= 0 && use.owner->isConstructor()) {
      args.push_back(this->state.getAst().make<UndefinedExpr>(loc));
    } else {
      args.push_back(
          this->state.getAst().make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
    }
  }
  if (place == Place::Parameter) {
    args.push_back(this->state.getAst().make<IntLitExpr>(loc, uint64_t(use.param)));
  } else if (place == Place::Method && run.descVar != nullptr) {
    args.push_back(this->state.getAst().make<NameExpr>(loc, run.descVar->name));
  }

  return args;
}

Expr *DecoratorChecker::decoratorOutcome(DecorateRun &run, const DecoratorUse &use, Place place,
                                         Expr *applied) {
  llvm::SMLoc loc = use.decorator->loc;
  const Type *result = applied->type;
  if (run.descVar != nullptr && place == Place::Method && result == run.descType) {
    auto *target = this->state.getAst().make<NameExpr>(loc, run.descVar->name);
    target->binding = NamedVar{.var = run.descVar};
    target->type = run.descType;
    auto *assign = this->state.getAst().make<AssignExpr>(loc, std::nullopt, target, applied);
    assign->type = run.descType;
    return assign;
  }
  const ClassDecl *made = result->classRef();
  if (place == Place::Class && made != nullptr && made->derivesFrom(run.current)) {
    auto *given = const_cast<ClassDecl *>(made);
    if (given != run.current) {
      this->lateClasses->replaceClass(run.current, given, loc);
      run.current = given;
    }
    return applied;
  }
  std::string name = DecoratorChecker::decoratorName(use.decorator->expr);
  std::string label = ErrorTextDecoratorPlace::label(name);
  ResultDecoratorReturns returned =
      DecoratorChecker::decoratorResult(label, result, place, run.descVar != nullptr, run.current);
  if (returned.isErr()) {
    this->state.getDiagnostics().error(loc, returned.error().message);
  }

  return applied;
}

void DecoratorChecker::storeDescriptorValue(const DecorateRun &run) {
  llvm::SMLoc at = run.at;
  auto *target = this->state.getAst().make<NameExpr>(at, run.method->impl()->name);
  auto *descriptor = this->state.getAst().make<NameExpr>(at, run.descVar->name);
  auto *value = this->state.getAst().make<MemberExpr>(at, descriptor, "value");
  auto *assign = this->state.getAst().make<AssignExpr>(at, std::nullopt, target, value);
  Expr *checked = this->exprChecker->checkExpr(assign);
  run.cls->decorations.push_back(this->state.getAst().make<ExprStmt>(at, checked));
}
}
