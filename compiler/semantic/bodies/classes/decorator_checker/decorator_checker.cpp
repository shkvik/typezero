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
  this->classDeclarer = &all.classDeclarer;
  this->metadataChecker = &all.metadataChecker;
  this->classValueChecker = &all.classValueChecker;
}

size_t DecoratorChecker::argumentsOf(Place place) const {
  switch (place) {
  case Place::Class:
    return 1;
  case Place::Field:
    return 2;
  default:
    return 3;
  }
}

std::string DecoratorChecker::placeText(Place place) const {
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

std::string DecoratorChecker::argumentsText(Place place) const {
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

std::string DecoratorChecker::decoratorName(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->withoutCalls(expr);
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    names.push_back(member->name);
    link = this->withoutCalls(member->object);
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

const Expr *DecoratorChecker::withoutCalls(const Expr *expr) const {
  const Expr *callee = expr;
  while (auto *call = dyn_cast<CallExpr>(callee)) {
    callee = call->callee;
  }

  return callee;
}

bool DecoratorChecker::isPlainReference(const Expr *expr) const {
  const Expr *link = expr;
  while (auto *member = dyn_cast<MemberExpr>(link)) {
    link = member->object;
  }

  return isa<NameExpr>(link);
}

bool DecoratorChecker::decoratedParams(const FunctionDecl *functionDecl) const {
  return llvm::any_of(functionDecl->params,
                      [](const Param &param) { return !param.decorators.empty(); });
}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *returnType) {
  const auto *value = Type::function({Type::unknown(), args}, 1, true, returnType, true);
  const auto *flag = Type::boolTy();
  const auto *type = Type::object({{"value", value, false},
                                   {"writable", flag, false},
                                   {"enumerable", flag, false},
                                   {"configurable", flag, false}},
                                  "MethodDescriptor<" + args->name().str() + ", " +
                                      returnType->name().str() + ">");
  this->state.getAliasStore().rememberDescriptorOrigin(type, args, returnType);

  return type;
}

bool DecoratorChecker::decoratorParams(Expr *callee, std::vector<const Type *> &params,
                                       size_t &required) {
  const FuncSymbol *function = nullptr;
  auto *name = dyn_cast<NameExpr>(callee);
  if (name != nullptr) {
    auto entry = this->scopes->lookup(name->name);
    if (entry.var() != nullptr) {
      const Type *type = entry.var()->type;
      if (type == nullptr || !type->isFunction() || type->hasRest()) {
        return false;
      }
      params.assign(type->params().begin(), type->params().end());
      required = type->minArgs();
      return true;
    }
    function = entry.func();
  } else if (auto *member = dyn_cast<MemberExpr>(callee)) {
    auto *object = dyn_cast<NameExpr>(member->object);
    ClassDecl *classDecl = object != nullptr ? this->scopes->lookup(object->name).cls() : nullptr;
    if (classDecl != nullptr) {
      auto methods = this->classDeclarer->staticMethodsNamed(classDecl, member->name);
      if (methods.size() == 1) {
        function = methods[0]->sym;
      }
    }
  }
  if (function == nullptr) {
    return false;
  }
  if (function->hasRest || llvm::is_contained(function->params, nullptr)) {
    return false;
  }
  params = function->params;
  required = function->minArgs;

  return true;
}

DecoratorChecker::ResultNamesAreDistinct
DecoratorChecker::namesDistinct(const DecoratedName &a, const DecoratedName &b,
                                llvm::StringRef className) const {
  if (a.name == b.name) {
    std::string what = a.isStatic != b.isStatic ? ErrorTextDecoratedNames::bothKinds(a.name)
                                                : ErrorTextDecoratedNames::variants(a.name);
    auto shared = std::string();
    for (const std::string &d : b.decorators) {
      if (!d.empty() && llvm::is_contained(a.decorators, d)) {
        shared = d;
      }
    }
    std::string records = shared.empty() ? ErrorTextDecoratedNames::design()
                                         : ErrorTextDecoratedNames::recordsOf(shared);
    auto message = ErrorTextDecoratedNames::shared(what, className, records);

    return err(DecoratorCheckerNamesDistinctError{
        .code = DecoratorCheckerNamesDistinctErrorCode::SAME_NAME,
        .message = message,
    });
  }

  return ok(&b);
}

DecoratorChecker::ResultFirstConstructor
DecoratorChecker::secondConstructor(const ClassDecl *classDecl, const FunctionDecl *first) const {
  if (first != nullptr) {
    auto message = ErrorTextDecoratedNames::sameRecord(classDecl->name);

    return err(DecoratorCheckerSecondConstructorError{
        .code = DecoratorCheckerSecondConstructorErrorCode::TWO_DECORATED_CONSTRUCTORS,
        .message = message,
    });
  }

  return ok(classDecl);
}

void DecoratorChecker::walkDecoratedNames(ClassDecl *classDecl) {
  auto members = std::vector<DecoratedName>();
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc, bool isStatic,
                 llvm::ArrayRef<Decorator> decorators, const FunctionDecl *functionDecl) {
    DecoratedName m{name, loc, isStatic, {}};
    for (const Decorator &decorator : decorators) {
      m.decorators.push_back(this->decoratorName(decorator.expr));
    }
    if (functionDecl != nullptr) {
      for (const Param &param : functionDecl->params) {
        for (const Decorator &decorator : param.decorators) {
          m.decorators.push_back(this->decoratorName(decorator.expr));
        }
      }
    }
    if (!m.decorators.empty()) {
      members.push_back(std::move(m));
    }
  };
  for (const ClassField &field : classDecl->fields) {
    add(field.name, field.loc, field.isStatic(), field.decorators, nullptr);
  }
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    add(functionDecl->name, functionDecl->loc(), functionDecl->isStatic(),
        functionDecl->decorators(), functionDecl);
  }
  for (size_t index = 0; index < members.size(); ++index) {
    for (size_t j = 0; j < index; ++j) {
      const DecoratedName &a = members[j];
      const DecoratedName &b = members[index];
      auto distinct = this->namesDistinct(a, b, classDecl->name);
      if (distinct.isOk()) {
        continue;
      }
      this->state.getDiagnostics().error(b.loc, distinct.error().message);
      auto other = ErrorTextDecoratedNames::other();
      this->state.getDiagnostics().note(a.loc, other);
      break;
    }
  }
  const FunctionDecl *first = nullptr;
  for (const FunctionDecl *constructor : classDecl->constructors) {
    if (!this->decoratedParams(constructor)) {
      continue;
    }
    auto only = this->secondConstructor(classDecl, first);
    if (only.isOk()) {
      first = constructor;
      continue;
    }
    this->state.getDiagnostics().error(constructor->loc(), only.error().message);
    auto other = ErrorTextDecoratedNames::other();
    this->state.getDiagnostics().note(first->loc(), other);
    break;
  }
}

void DecoratorChecker::decorateClass(ClassDecl *classDecl) {
  this->walkDecoratedNames(classDecl);
  struct Member {
    ClassField *field;
    FunctionDecl *method;
    bool isStatic;
    const char *at;
  };
  auto members = std::vector<Member>();
  for (ClassField &f : classDecl->fields) {
    if (!f.decorators.empty()) {
      members.push_back({&f, nullptr, f.isStatic(), f.loc.getPointer()});
    }
  }
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->decorators().empty() || this->decoratedParams(functionDecl)) {
      members.push_back(
          {nullptr, functionDecl, functionDecl->isStatic(), functionDecl->loc().getPointer()});
    }
  }
  llvm::stable_sort(members, [](const Member &a, const Member &b) {
    return std::make_pair(a.isStatic, a.at) < std::make_pair(b.isStatic, b.at);
  });
  for (Member &m : members) {
    auto uses = std::vector<DecoratorUse>();
    for (const Decorator &decorator :
         m.field != nullptr ? m.field->decorators : m.method->decorators()) {
      uses.push_back({&decorator});
    }
    if (m.method != nullptr) {
      for (size_t index = 0; index < m.method->params.size(); ++index) {
        for (const Decorator &decorator : m.method->params[index].decorators) {
          uses.push_back({&decorator, int(index), m.method});
        }
      }
    }
    this->decorate(classDecl, m.field, m.method, uses);
  }
  auto uses = std::vector<DecoratorUse>();
  for (const Decorator &decorator : classDecl->decorators) {
    uses.push_back({&decorator});
  }
  for (const FunctionDecl *constructor : classDecl->constructors) {
    for (size_t index = 0; index < constructor->params.size(); ++index) {
      for (const Decorator &decorator : constructor->params[index].decorators) {
        uses.push_back({&decorator, int(index), constructor});
      }
    }
  }
  this->decorate(classDecl, nullptr, nullptr, uses);
}

DecoratorChecker::ResultPlainMemberFound
DecoratorChecker::memberDecoratable(const ClassField *field, const FunctionDecl *method) const {
  if (field != nullptr && field->accessor) {
    auto message = ErrorTextDecorate::accessor();

    return err(DecoratorCheckerMemberDecoratableError{
        .code = DecoratorCheckerMemberDecoratableErrorCode::ACCESSOR_FIELD,
        .message = message,
    });
  }
  if (method != nullptr && method->isAbstract()) {
    auto message = ErrorTextDecorate::abstract();

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
                                                                         const Type *third) const {
  if (needs > given) {
    std::string needed = std::to_string(needs);
    std::string gets = std::to_string(given);
    auto where = this->placeText(place);
    auto args = this->argumentsText(place);
    auto message = ErrorTextDecorate::arguments(label, where, needed, gets, args);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  if (third != nullptr && place == Place::Parameter && third->isObject() &&
      third->field("value") != nullptr) {
    auto message = ErrorTextDecorate::descriptor(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::DESCRIPTOR_FOR_PARAMETER,
        .message = message,
    });
  }
  if (third != nullptr && place == Place::Method && third->isNumeric()) {
    auto message = ErrorTextDecorate::position(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::POSITION_FOR_METHOD,
        .message = message,
    });
  }

  return ok(needs);
}

DecoratorChecker::ResultDescriptorUsable
DecoratorChecker::genericDescriptor(const ClassDecl *classDecl, bool descriptor) const {
  if (descriptor && classDecl->isGeneric()) {
    auto message = ErrorTextDecorate::generic(classDecl->name);

    return err(DecoratorCheckerGenericDescriptorError{
        .code = DecoratorCheckerGenericDescriptorErrorCode::GENERIC_CLASS_DESCRIPTOR,
        .message = message,
    });
  }

  return ok(classDecl);
}

DecoratorChecker::ResultMetadataArgsPair
DecoratorChecker::metadataArgs(const CallExpr *metadata) const {
  if (metadata->args.size() != 2 || metadata->hasSpread()) {
    auto message = ErrorTextDecorate::metadata();

    return err(DecoratorCheckerMetadataArgsError{
        .code = DecoratorCheckerMetadataArgsErrorCode::WRONG_METADATA_ARGUMENTS,
        .message = message,
    });
  }

  return ok(metadata);
}

DecoratorChecker::ResultDecoratorReturns
DecoratorChecker::decoratorResult(llvm::StringRef label, const Type *result, Place place,
                                  bool descriptor, const ClassDecl *current) const {
  bool isNothing = result->isVoid() || result->isUndefined() || result->isError();
  if (!isNothing && place == Place::Class && result->isConstructor()) {
    auto message = ErrorTextDecoratorResult::runtime(label, result->name(), current->name);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (!isNothing && place == Place::Class) {
    auto message = ErrorTextDecoratorResult::forClass(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::CLASS_RESULT,
        .message = message,
    });
  }
  if (!isNothing && place == Place::Method && descriptor) {
    auto message = ErrorTextDecoratorResult::forMethod(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::METHOD_RESULT,
        .message = message,
    });
  }
  if (!isNothing) {
    auto where = this->placeText(place);
    auto message = ErrorTextDecoratorResult::other(label, result->name(), where);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::OTHER_RESULT,
        .message = message,
    });
  }

  return ok(result);
}

void DecoratorChecker::decorate(ClassDecl *classDecl, ClassField *field, FunctionDecl *method,
                                llvm::ArrayRef<DecoratorUse> uses) {
  if (uses.empty()) {
    return;
  }
  DecorateRun run{
      .cls = classDecl,
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
      .current = classDecl,
  };
  auto plain = this->memberDecoratable(field, method);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(run.at, plain.error().message);
    return;
  }
  this->hoistDecorators(run);
  if (!this->decoratorsFit(run)) {
    return;
  }
  auto usable = this->genericDescriptor(classDecl, run.descriptor);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(run.at, usable.error().message);
    return;
  }
  if (run.descriptor && !this->hasKnownSignature(method->sym)) {
    return;
  }
  if (run.descriptor) {
    this->declareDescriptor(run);
  }
  const auto *constructor = this->designConstructor(run);
  this->metadataChecker->writeDesignRecords(classDecl, field, method, constructor, run.at);
  for (size_t index = uses.size(); index-- > 0;) {
    this->applyDecorator(run, index);
  }
  if (run.descVar != nullptr) {
    this->storeDescriptorValue(run);
  }
}

bool DecoratorChecker::hasKnownSignature(FuncSymbol *function) const {
  const auto *returnType = function->getReturnType();

  return returnType != nullptr && !returnType->isError() &&
         !llvm::is_contained(function->params, nullptr);
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
  FuncSymbol *function = run.method->sym;
  if (!this->hasKnownSignature(function)) {
    return nullptr;
  }
  const auto *args = Type::tuple(function->params);
  run.earlyDescriptor = this->methodDescriptorType(args, function->getReturnType());

  return run.earlyDescriptor;
}

const Type *DecoratorChecker::factoryHint(DecorateRun &run, const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *name = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  bool isFactoryShape = name != nullptr && name->typeArgs.empty() && run.own == Place::Method &&
                        !run.cls->isGeneric();
  if (!isFactoryShape) {
    return nullptr;
  }
  const FuncSymbol *factory = this->scopes->lookup(name->name).func();
  if (factory == nullptr || factory->decl == nullptr || factory->decl->typeParams.empty()) {
    return nullptr;
  }
  const auto *descriptorType = this->methodDescriptor(run);
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
    if (this->isPlainReference(expr)) {
      run.callees.push_back(expr);
      continue;
    }
    auto *hoisted = this->hoistDecorator(run, use);
    run.callees.push_back(hoisted);
  }
}

Expr *DecoratorChecker::hoistDecorator(DecorateRun &run, const DecoratorUse &use) {
  Expr *expr = use.decorator->expr;
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  auto *varDecl = this->state.getAst().make<VarDeclStmt>(use.decorator->loc, true);
  VarDeclarator declarator;
  declarator.name = hidden;
  declarator.loc = use.decorator->loc;
  declarator.init = expr;
  const Type *hint = use.param < 0 ? this->factoryHint(run, expr) : nullptr;
  if (hint != nullptr) {
    declarator.init = this->exprChecker->checkExpr(expr, hint);
    declarator.sym = this->scopes->declareConst(hidden, declarator.loc, declarator.init->type);
    varDecl->decls.push_back(std::move(declarator));
  } else {
    varDecl->decls.push_back(std::move(declarator));
    this->stmtChecker->walkStmt(varDecl);
  }
  run.cls->decorations.push_back(varDecl);

  return this->state.getAst().make<NameExpr>(use.decorator->loc, hidden);
}

bool DecoratorChecker::decoratorsFit(DecorateRun &run) {
  bool isFitting = true;
  for (size_t index = 0; index < run.uses.size(); ++index) {
    isFitting &= this->decoratorFitsAt(run, index);
  }

  return isFitting;
}

bool DecoratorChecker::decoratorFitsAt(DecorateRun &run, size_t index) {
  const DecoratorUse &use = run.uses[index];
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  size_t given = this->argumentsOf(place);
  if (run.callees[index] == nullptr) {
    run.takes.push_back(0);
    return true;
  }
  auto params = std::vector<const Type *>();
  size_t required = 0;
  bool isKnown = this->decoratorParams(run.callees[index], params, required);
  size_t count = isKnown ? params.size() : given;
  size_t needs = isKnown ? std::min(required, count) : given;
  auto name = this->decoratorName(use.decorator->expr);
  auto label = ErrorTextDecoratorPlace::label(name);
  const Type *third = isKnown && params.size() >= 3 ? params[2]->nonOptional() : nullptr;
  auto fits = this->decoratorFits(label, place, needs, given, third);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(use.decorator->loc, fits.error().message);
  }
  run.takes.push_back(std::min(count, given));
  run.descriptor |= place == Place::Method && count >= 3;

  return fits.isOk();
}

void DecoratorChecker::declareDescriptor(DecorateRun &run) {
  FuncSymbol *function = run.method->sym;
  if (run.earlyDescriptor != nullptr) {
    run.descType = run.earlyDescriptor;
  } else {
    const auto *args = Type::tuple(function->params);
    run.descType = this->methodDescriptorType(args, function->getReturnType());
  }
  auto *implementation = this->declareMethodImpl(run);
  run.method->member()->impl = implementation;
  auto *init = this->descriptorInit(run, implementation);
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  run.descVar = this->scopes->declareLet(hidden, run.at, run.descType);
  run.descVar->reassigned = true;
  auto *varDecl = this->state.getAst().make<VarDeclStmt>(run.at, false);
  VarDeclarator declarator;
  declarator.name = run.descVar->name;
  declarator.loc = run.at;
  declarator.init = init;
  declarator.sym = run.descVar;
  varDecl->decls.push_back(std::move(declarator));
  run.cls->decorations.push_back(varDecl);
}

VarSymbol *DecoratorChecker::declareMethodImpl(const DecorateRun &run) {
  const FunctionDecl *method = run.method;
  auto *implementation = this->state.getAst().newVar();
  std::string implName = "%" + run.cls->name.str() + "." + method->name.str() + "." +
                         std::to_string(method->variant());
  implementation->name = this->state.getAst().keepName(implName);
  implementation->loc = method->loc();
  implementation->type = run.descType->field("value")->type;
  implementation->place = GlobalVar{.usedInFunction = true};
  implementation->reassigned = true;
  auto added = this->scopes->declare(implementation->name, {VarEntry{.var = implementation}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(implementation->loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->scopes->lookup(implementation->name);
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }

  return implementation;
}

Expr *DecoratorChecker::descriptorInit(const DecorateRun &run, const VarSymbol *implementation) {
  llvm::SMLoc at = run.at;
  auto *literal = this->state.getAst().make<ObjectLitExpr>(at);
  Expr *value = this->state.getAst().make<NameExpr>(at, implementation->name);
  this->addProperty(literal, "value", value, at);
  Expr *writable = this->state.getAst().make<BoolLitExpr>(at, true);
  this->addProperty(literal, "writable", writable, at);
  Expr *enumerable = this->state.getAst().make<BoolLitExpr>(at, false);
  this->addProperty(literal, "enumerable", enumerable, at);
  Expr *configurable = this->state.getAst().make<BoolLitExpr>(at, true);
  this->addProperty(literal, "configurable", configurable, at);
  auto *checked = this->exprChecker->checkExpr(literal, run.descType);

  return this->typeRelations->coerce(checked, run.descType);
}

void DecoratorChecker::addProperty(ObjectLitExpr *literal, llvm::StringRef name, Expr *value,
                                   llvm::SMLoc at) const {
  ObjectLitExpr::Property property;
  property.name = name;
  property.loc = at;
  property.value = value;
  literal->props.push_back(property);
}

const FunctionDecl *DecoratorChecker::designConstructor(const DecorateRun &run) const {
  if (run.own != Place::Class) {
    return nullptr;
  }
  const FunctionDecl *constructor = nullptr;
  for (const FunctionDecl *c : run.cls->constructors) {
    if (this->decoratedParams(c) || run.cls->constructors.size() == 1) {
      constructor = constructor != nullptr ? nullptr : c;
    }
  }

  return constructor;
}

void DecoratorChecker::applyDecorator(DecorateRun &run, size_t index) {
  const DecoratorUse &use = run.uses[index];
  if (run.callees[index] == nullptr) {
    this->applyMetadata(run, use);
    return;
  }
  llvm::SMLoc loc = use.decorator->loc;
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  auto args = this->decoratorArgs(run, use, place);
  args.resize(std::min(args.size(), run.takes[index]));
  auto *call = this->state.getAst().make<CallExpr>(loc, run.callees[index], std::move(args));
  auto *checked = this->exprChecker->checkExpr(call);
  auto *applied = this->decoratorOutcome(run, use, place, checked);
  run.cls->decorations.push_back(this->state.getAst().make<ExprStmt>(loc, applied));
}

void DecoratorChecker::applyMetadata(DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  Place place = use.param >= 0 ? Place::Parameter : run.own;
  auto *metadata = cast<CallExpr>(use.decorator->expr);
  auto pair = this->metadataArgs(metadata);
  if (pair.isErr()) {
    this->state.getDiagnostics().error(loc, pair.error().message);
    return;
  }
  auto *reflect = this->state.getAst().make<NameExpr>(loc, "Reflect");
  auto *callee = this->state.getAst().make<MemberExpr>(loc, reflect, "defineMetadata");
  auto *target = this->classValueChecker->classValueAt(run.cls, loc);
  std::vector<Expr *> args{metadata->args[0], metadata->args[1], target};
  llvm::StringRef name = use.param >= 0 ? use.owner->name : run.memberName;
  if (place != Place::Class && !(use.param >= 0 && use.owner->isConstructor())) {
    args.push_back(
        this->state.getAst().make<StringLitExpr>(loc, std::u16string(name.begin(), name.end())));
  }
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, args);
  auto *checked = this->exprChecker->checkExpr(call);
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
  auto name = this->decoratorName(use.decorator->expr);
  auto label = ErrorTextDecoratorPlace::label(name);
  auto returned = this->decoratorResult(label, result, place, run.descVar != nullptr, run.current);
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
  auto *checked = this->exprChecker->checkExpr(assign);
  run.cls->decorations.push_back(this->state.getAst().make<ExprStmt>(at, checked));
}

}
