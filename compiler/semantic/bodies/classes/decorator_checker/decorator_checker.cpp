#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
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

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";

}

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
    case Place::CLASS:
      return 1;
    case Place::FIELD:
      return 2;
    case Place::METHOD:
    case Place::PARAMETER:
      return 3;
  }

  return 3;
}

std::string DecoratorChecker::placeText(Place place) const {
  switch (place) {
    case Place::CLASS:
      return ErrorTextDecoratorPlace::aClass();
    case Place::FIELD:
      return ErrorTextDecoratorPlace::aField();
    case Place::METHOD:
      return ErrorTextDecoratorPlace::aMethod();
    case Place::PARAMETER:
      return ErrorTextDecoratorPlace::aParameter();
  }

  return ErrorTextDecoratorPlace::aParameter();
}

std::string DecoratorChecker::argumentsText(Place place) const {
  switch (place) {
    case Place::CLASS:
      return ErrorTextDecoratorPlace::classArgs();
    case Place::FIELD:
      return ErrorTextDecoratorPlace::fieldArgs();
    case Place::METHOD:
      return ErrorTextDecoratorPlace::methodArgs();
    case Place::PARAMETER:
      return ErrorTextDecoratorPlace::paramArgs();
  }

  return ErrorTextDecoratorPlace::paramArgs();
}

DecoratorChecker::Place DecoratorChecker::usePlace(const DecorateRun &run,
                                                   const DecoratorUse &use) const {
  return this->isParamUse(use) ? Place::PARAMETER : run.ownPlace;
}

bool DecoratorChecker::isParamUse(const DecoratorUse &use) const {
  return use.param >= 0;
}

bool DecoratorChecker::isConstructorParamUse(const DecoratorUse &use) const {
  return this->isParamUse(use) && use.owner->isConstructor();
}

llvm::StringRef DecoratorChecker::useOwnerName(const DecorateRun &run,
                                               const DecoratorUse &use) const {
  return this->isParamUse(use) ? use.owner->name : run.memberName;
}

std::string DecoratorChecker::decoratorName(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->withoutCalls(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    names.push_back(member->name);
    link = this->withoutCalls(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr || root->name.starts_with(kHiddenPrefix)) {
    return "";
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

std::string DecoratorChecker::decoratorLabel(const Expr *expr) const {
  auto name = this->decoratorName(expr);

  return ErrorTextDecoratorPlace::label(name);
}

const Expr *DecoratorChecker::withoutCalls(const Expr *expr) const {
  const Expr *callee = expr;
  const auto *call = dyn_cast<CallExpr>(callee);
  while (call != nullptr) {
    callee = call->callee;
    call = dyn_cast<CallExpr>(callee);
  }

  return callee;
}

bool DecoratorChecker::isPlainReference(const Expr *expr) const {
  const Expr *link = expr;
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    link = member->object;
    member = dyn_cast<MemberExpr>(link);
  }

  return isa<NameExpr>(link);
}

bool DecoratorChecker::hasDecoratedParams(const FunctionDecl *functionDecl) const {
  auto isDecorated = std::bind_front(&DecoratorChecker::isDecoratedParam, this);

  return llvm::any_of(functionDecl->params, isDecorated);
}

bool DecoratorChecker::isDecoratedParam(const Param &param) const {
  return !param.decorators.empty();
}

const Type *DecoratorChecker::methodDescriptorType(const Type *args, const Type *returnType) {
  const auto *value = Type::function({Type::unknown(), args}, 1, true, returnType, true);
  const auto *flag = Type::boolTy();
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  auto writableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_WRITABLE);
  auto enumerableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_ENUMERABLE);
  auto configurableName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_CONFIGURABLE);
  auto fields = std::vector<Field>{
      {.name = valueName.str(), .type = value},
      {.name = writableName.str(), .type = flag},
      {.name = enumerableName.str(), .type = flag},
      {.name = configurableName.str(), .type = flag},
  };
  auto descriptorName = TypeNameTable::getName(TypeName::METHOD_DESCRIPTOR);
  std::string typeName =
      descriptorName.str() + "<" + args->name().str() + ", " + returnType->name().str() + ">";
  const auto *type = Type::object(fields, typeName);
  this->state.getAliasStore().rememberDescriptorOrigin(type, args, returnType);

  return type;
}

std::optional<DecoratorChecker::KnownParams> DecoratorChecker::knownParams(Expr *callee) {
  const auto *name = dyn_cast<NameExpr>(callee);
  if (name != nullptr) {
    return this->namedParams(name);
  }
  const auto *member = dyn_cast<MemberExpr>(callee);
  if (member != nullptr) {
    const auto *function = this->staticMethodSymbol(member);
    return this->functionParams(function);
  }

  return std::nullopt;
}

std::optional<DecoratorChecker::KnownParams> DecoratorChecker::namedParams(const NameExpr *name) {
  auto entry = this->scopes->lookup(name->name);
  if (entry.var() != nullptr) {
    return this->typeParams(entry.var()->type);
  }

  return this->functionParams(entry.func());
}

std::optional<DecoratorChecker::KnownParams> DecoratorChecker::typeParams(const Type *type) const {
  if (type == nullptr || !type->isFunction() || type->hasRest()) {
    return std::nullopt;
  }
  auto params = type->params();

  return KnownParams{.params = params.vec(), .required = type->minArgs()};
}

std::optional<DecoratorChecker::KnownParams>
DecoratorChecker::functionParams(const FuncSymbol *function) const {
  if (function == nullptr || function->hasRest || llvm::is_contained(function->params, nullptr)) {
    return std::nullopt;
  }

  return KnownParams{.params = function->params, .required = function->minArgs};
}

const FuncSymbol *DecoratorChecker::staticMethodSymbol(const MemberExpr *member) {
  const auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return nullptr;
  }
  ClassDecl *classDecl = this->scopes->lookup(object->name).cls();
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = this->classDeclarer->staticMethodsNamed(classDecl, member->name);

  return methods.size() == 1 ? methods[0]->sym : nullptr;
}

const Type *DecoratorChecker::thirdParam(const std::optional<KnownParams> &known) const {
  if (!known.has_value() || known->params.size() < 3) {
    return nullptr;
  }

  return known->params[2]->nonOptional();
}

DecoratorChecker::ResultNamesAreDistinct
DecoratorChecker::checkNamesDistinct(const DecoratedName &earlier, const DecoratedName &later,
                                     llvm::StringRef className) const {
  if (earlier.name == later.name) {
    std::string what = earlier.isStatic != later.isStatic
                           ? ErrorTextDecoratedNames::bothKinds(earlier.name)
                           : ErrorTextDecoratedNames::variants(earlier.name);
    auto shared = this->sharedDecorator(earlier, later);
    std::string records = shared.empty() ? ErrorTextDecoratedNames::design()
                                         : ErrorTextDecoratedNames::recordsOf(shared);
    auto message = ErrorTextDecoratedNames::shared(what, className, records);

    return err(DecoratorCheckerNamesDistinctError{
        .code = DecoratorCheckerNamesDistinctErrorCode::SAME_NAME,
        .message = message,
    });
  }

  return ok(&later);
}

std::string DecoratorChecker::sharedDecorator(const DecoratedName &earlier,
                                              const DecoratedName &later) const {
  for (const std::string &decorator : llvm::reverse(later.decorators)) {
    if (!decorator.empty() && llvm::is_contained(earlier.decorators, decorator)) {
      return decorator;
    }
  }

  return "";
}

DecoratorChecker::ResultFirstConstructor DecoratorChecker::checkOneDecoratedConstructor(
    const ClassDecl *classDecl, llvm::ArrayRef<const FunctionDecl *> decorated) const {
  if (decorated.size() > 1) {
    auto message = ErrorTextDecoratedNames::sameRecord(classDecl->name);

    return err(DecoratorCheckerSecondConstructorError{
        .code = DecoratorCheckerSecondConstructorErrorCode::TWO_DECORATED_CONSTRUCTORS,
        .message = message,
    });
  }

  return ok(classDecl);
}

void DecoratorChecker::walkDecoratedNames(ClassDecl *classDecl) {
  auto members = this->decoratedNames(classDecl);
  for (size_t index = 0; index < members.size(); ++index) {
    this->walkNameClash(members, index, classDecl->name);
  }
  this->walkDecoratedConstructors(classDecl);
}

std::vector<DecoratorChecker::DecoratedName>
DecoratorChecker::decoratedNames(const ClassDecl *classDecl) const {
  auto members = std::vector<DecoratedName>();
  for (const ClassField &field : classDecl->fields) {
    auto decorators = this->decoratorNames(field.decorators);
    if (!decorators.empty()) {
      members.push_back({
          .name = field.name,
          .loc = field.loc,
          .isStatic = field.isStatic(),
          .decorators = decorators,
      });
    }
  }
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    auto decorators = this->methodDecoratorNames(functionDecl);
    if (!decorators.empty()) {
      members.push_back({
          .name = functionDecl->name,
          .loc = functionDecl->loc(),
          .isStatic = functionDecl->isStatic(),
          .decorators = decorators,
      });
    }
  }

  return members;
}

std::vector<std::string>
DecoratorChecker::decoratorNames(llvm::ArrayRef<Decorator> decorators) const {
  auto names = std::vector<std::string>();
  this->addDecoratorNames(names, decorators);

  return names;
}

std::vector<std::string>
DecoratorChecker::methodDecoratorNames(const FunctionDecl *functionDecl) const {
  auto names = this->decoratorNames(functionDecl->decorators());
  for (const Param &param : functionDecl->params) {
    this->addDecoratorNames(names, param.decorators);
  }

  return names;
}

void DecoratorChecker::addDecoratorNames(std::vector<std::string> &names,
                                         llvm::ArrayRef<Decorator> decorators) const {
  for (const Decorator &decorator : decorators) {
    auto name = this->decoratorName(decorator.expr);
    names.push_back(name);
  }
}

void DecoratorChecker::walkNameClash(llvm::ArrayRef<DecoratedName> members, size_t index,
                                     llvm::StringRef className) {
  const DecoratedName &later = members[index];
  for (const DecoratedName &earlier : members.take_front(index)) {
    auto distinct = this->checkNamesDistinct(earlier, later, className);
    if (distinct.isErr()) {
      this->state.getDiagnostics().error(later.loc, distinct.error().message);
      auto other = ErrorTextDecoratedNames::other();
      this->state.getDiagnostics().note(earlier.loc, other);
      return;
    }
  }
}

void DecoratorChecker::walkDecoratedConstructors(const ClassDecl *classDecl) {
  auto decorated = this->decoratedConstructors(classDecl);
  auto only = this->checkOneDecoratedConstructor(classDecl, decorated);
  if (only.isErr()) {
    this->state.getDiagnostics().error(decorated[1]->loc(), only.error().message);
    auto other = ErrorTextDecoratedNames::other();
    this->state.getDiagnostics().note(decorated[0]->loc(), other);
  }
}

std::vector<const FunctionDecl *>
DecoratorChecker::decoratedConstructors(const ClassDecl *classDecl) const {
  auto decorated = std::vector<const FunctionDecl *>();
  for (const FunctionDecl *constructor : classDecl->constructors) {
    if (this->hasDecoratedParams(constructor)) {
      decorated.push_back(constructor);
    }
  }

  return decorated;
}

void DecoratorChecker::decorateClass(ClassDecl *classDecl) {
  this->walkDecoratedNames(classDecl);
  auto members = this->decoratedMembers(classDecl);
  for (const DecoratedMember &member : members) {
    auto uses = this->memberUses(member);
    this->decorate(classDecl, member.field, member.method, uses);
  }
  auto uses = this->classUses(classDecl);
  this->decorate(classDecl, nullptr, nullptr, uses);
}

std::vector<DecoratorChecker::DecoratedMember>
DecoratorChecker::decoratedMembers(ClassDecl *classDecl) const {
  auto members = std::vector<DecoratedMember>();
  for (ClassField &field : classDecl->fields) {
    if (!field.decorators.empty()) {
      members.push_back({
          .field = &field,
          .method = nullptr,
          .isStatic = field.isStatic(),
          .at = field.loc.getPointer(),
      });
    }
  }
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->decorators().empty() || this->hasDecoratedParams(functionDecl)) {
      members.push_back({
          .field = nullptr,
          .method = functionDecl,
          .isStatic = functionDecl->isStatic(),
          .at = functionDecl->loc().getPointer(),
      });
    }
  }
  auto isBefore = std::bind_front(&DecoratorChecker::isMemberBefore, this);
  llvm::stable_sort(members, isBefore);

  return members;
}

bool DecoratorChecker::isMemberBefore(const DecoratedMember &first,
                                      const DecoratedMember &second) const {
  return std::make_pair(first.isStatic, first.at) < std::make_pair(second.isStatic, second.at);
}

std::vector<DecoratorUse> DecoratorChecker::memberUses(const DecoratedMember &member) const {
  if (member.field != nullptr) {
    return this->ownUses(member.field->decorators);
  }
  auto uses = this->ownUses(member.method->decorators());
  this->addParamUses(uses, member.method);

  return uses;
}

std::vector<DecoratorUse> DecoratorChecker::classUses(const ClassDecl *classDecl) const {
  auto uses = this->ownUses(classDecl->decorators);
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->addParamUses(uses, constructor);
  }

  return uses;
}

std::vector<DecoratorUse> DecoratorChecker::ownUses(llvm::ArrayRef<Decorator> decorators) const {
  auto uses = std::vector<DecoratorUse>();
  for (const Decorator &decorator : decorators) {
    uses.push_back({.decorator = &decorator});
  }

  return uses;
}

void DecoratorChecker::addParamUses(std::vector<DecoratorUse> &uses,
                                    const FunctionDecl *functionDecl) const {
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    this->addParamUse(uses, functionDecl, index);
  }
}

void DecoratorChecker::addParamUse(std::vector<DecoratorUse> &uses,
                                   const FunctionDecl *functionDecl, size_t index) const {
  for (const Decorator &decorator : functionDecl->params[index].decorators) {
    uses.push_back({.decorator = &decorator, .param = int(index), .owner = functionDecl});
  }
}

DecoratorChecker::ResultPlainMemberFound
DecoratorChecker::checkMemberDecoratable(const ClassField *field,
                                         const FunctionDecl *method) const {
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

DecoratorChecker::ResultDecoratorArgsFit
DecoratorChecker::checkDecoratorFits(llvm::StringRef label, Place place, size_t needs, size_t given,
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
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  if (third != nullptr && place == Place::PARAMETER && third->isObject() &&
      third->field(valueName) != nullptr) {
    auto message = ErrorTextDecorate::descriptor(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::DESCRIPTOR_FOR_PARAMETER,
        .message = message,
    });
  }
  if (third != nullptr && place == Place::METHOD && third->isNumeric()) {
    auto message = ErrorTextDecorate::position(label);

    return err(DecoratorCheckerDecoratorFitsError{
        .code = DecoratorCheckerDecoratorFitsErrorCode::POSITION_FOR_METHOD,
        .message = message,
    });
  }

  return ok(needs);
}

DecoratorChecker::ResultDescriptorUsable
DecoratorChecker::checkGenericDescriptor(const ClassDecl *classDecl, bool hasDescriptor) const {
  if (hasDescriptor && classDecl->isGeneric()) {
    auto message = ErrorTextDecorate::generic(classDecl->name);

    return err(DecoratorCheckerGenericDescriptorError{
        .code = DecoratorCheckerGenericDescriptorErrorCode::GENERIC_CLASS_DESCRIPTOR,
        .message = message,
    });
  }

  return ok(classDecl);
}

DecoratorChecker::ResultMetadataArgsPair
DecoratorChecker::checkMetadataArgs(const CallExpr *metadata) const {
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
DecoratorChecker::checkDecoratorResult(llvm::StringRef label, const Type *result, Place place,
                                       bool hasDescriptor, const ClassDecl *current) const {
  bool isNothing = result->isVoid() || result->isUndefined() || result->isError();
  if (!isNothing && place == Place::CLASS && result->isConstructor()) {
    auto message = ErrorTextDecoratorResult::runtime(label, result->name(), current->name);

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (!isNothing && place == Place::CLASS) {
    auto message = ErrorTextDecoratorResult::forClass(label, result->name());

    return err(DecoratorCheckerDecoratorResultError{
        .code = DecoratorCheckerDecoratorResultErrorCode::CLASS_RESULT,
        .message = message,
    });
  }
  if (!isNothing && place == Place::METHOD && hasDescriptor) {
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
  auto ownPlace = this->placeOfMember(field, method);
  auto memberName = this->nameOfMember(field, method);
  DecorateRun run{
      .classDecl = classDecl,
      .field = field,
      .method = method,
      .uses = uses,
      .ownPlace = ownPlace,
      .memberName = memberName,
      .at = uses[0].decorator->loc,
      .earlyDescriptor = nullptr,
      .callees = {},
      .takes = {},
      .hasDescriptor = false,
      .descriptorVar = nullptr,
      .descriptorType = nullptr,
      .current = classDecl,
  };
  auto plain = this->checkMemberDecoratable(field, method);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(run.at, plain.error().message);
    return;
  }
  this->hoistDecorators(run);
  if (!this->matchDecorators(run)) {
    return;
  }
  auto usable = this->checkGenericDescriptor(classDecl, run.hasDescriptor);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(run.at, usable.error().message);
    return;
  }
  if (run.hasDescriptor && !this->hasKnownSignature(method->sym)) {
    return;
  }
  if (run.hasDescriptor) {
    this->declareDescriptor(run);
  }
  const auto *constructor = this->designConstructor(run);
  this->metadataChecker->writeDesignRecords(classDecl, field, method, constructor, run.at);
  for (size_t index = uses.size(); index-- > 0;) {
    this->applyDecorator(run, index);
  }
  if (run.descriptorVar != nullptr) {
    this->storeDescriptorValue(run);
  }
}

DecoratorChecker::Place DecoratorChecker::placeOfMember(const ClassField *field,
                                                        const FunctionDecl *method) const {
  if (field != nullptr) {
    return Place::FIELD;
  }
  if (method != nullptr) {
    return Place::METHOD;
  }

  return Place::CLASS;
}

llvm::StringRef DecoratorChecker::nameOfMember(const ClassField *field,
                                               const FunctionDecl *method) const {
  if (field != nullptr) {
    return field->name;
  }
  if (method != nullptr) {
    return method->name;
  }

  return llvm::StringRef();
}

bool DecoratorChecker::hasKnownSignature(FuncSymbol *function) const {
  const auto *returnType = function->getReturnType();

  return returnType != nullptr && !returnType->isError() &&
         !llvm::is_contained(function->params, nullptr);
}

bool DecoratorChecker::isMetadataCall(const Expr *expr) const {
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr) {
    return false;
  }
  auto function = ReflectFunctionTable::find(member->name);

  return function == ReflectFunction::METADATA &&
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
  const auto *call = dyn_cast<CallExpr>(expr);
  const auto *name = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  bool isFactoryShape = name != nullptr && name->typeArgs.empty() &&
                        run.ownPlace == Place::METHOD && !run.classDecl->isGeneric();
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
  const Type *classRef = Type::classRef(run.classDecl, run.classDecl->name);

  return Type::function({classRef, Type::string(), descriptorType}, 3, false, Type::voidTy());
}

void DecoratorChecker::hoistDecorators(DecorateRun &run) {
  for (const DecoratorUse &use : run.uses) {
    auto *callee = this->decoratorCallee(run, use);
    run.callees.push_back(callee);
  }
}

Expr *DecoratorChecker::decoratorCallee(DecorateRun &run, const DecoratorUse &use) {
  Expr *expr = use.decorator->expr;
  if (this->isMetadataCall(expr)) {
    return nullptr;
  }
  if (this->isPlainReference(expr)) {
    return expr;
  }

  return this->hoistDecorator(run, use);
}

Expr *DecoratorChecker::hoistDecorator(DecorateRun &run, const DecoratorUse &use) {
  Expr *expr = use.decorator->expr;
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  auto *varDecl = this->state.getAst().make<VarDeclStmt>(use.decorator->loc, true);
  VarDeclarator declarator{.name = hidden, .loc = use.decorator->loc, .init = expr};
  const auto *hint = this->isParamUse(use) ? nullptr : this->factoryHint(run, expr);
  if (hint != nullptr) {
    declarator.init = this->exprChecker->checkExpr(expr, hint);
    declarator.sym = this->scopes->declareConst(hidden, declarator.loc, declarator.init->type);
    varDecl->decls.push_back(std::move(declarator));
  } else {
    varDecl->decls.push_back(std::move(declarator));
    this->stmtChecker->walkStmt(varDecl);
  }
  run.classDecl->decorations.push_back(varDecl);

  return this->state.getAst().make<NameExpr>(use.decorator->loc, hidden);
}

bool DecoratorChecker::matchDecorators(DecorateRun &run) {
  bool isFitting = true;
  for (size_t index = 0; index < run.uses.size(); ++index) {
    isFitting &= this->matchDecorator(run, index);
  }

  return isFitting;
}

bool DecoratorChecker::matchDecorator(DecorateRun &run, size_t index) {
  if (run.callees[index] == nullptr) {
    run.takes.push_back(0);
    return true;
  }
  const DecoratorUse &use = run.uses[index];
  Place place = this->usePlace(run, use);
  size_t given = this->argumentsOf(place);
  auto known = this->knownParams(run.callees[index]);
  size_t count = known.has_value() ? known->params.size() : given;
  size_t needs = known.has_value() ? std::min(known->required, count) : given;
  auto label = this->decoratorLabel(use.decorator->expr);
  const auto *third = this->thirdParam(known);
  auto fits = this->checkDecoratorFits(label, place, needs, given, third);
  if (fits.isErr()) {
    this->state.getDiagnostics().error(use.decorator->loc, fits.error().message);
  }
  run.takes.push_back(std::min(count, given));
  run.hasDescriptor |= place == Place::METHOD && count >= 3;

  return fits.isOk();
}

void DecoratorChecker::declareDescriptor(DecorateRun &run) {
  run.descriptorType = this->methodDescriptor(run);
  auto *implementation = this->declareMethodImpl(run);
  run.method->member()->impl = implementation;
  auto *init = this->descriptorInit(run, implementation);
  llvm::StringRef hidden = this->state.getAst().hiddenName();
  run.descriptorVar = this->scopes->declareLet(hidden, run.at, run.descriptorType);
  run.descriptorVar->reassigned = true;
  auto *varDecl = this->state.getAst().make<VarDeclStmt>(run.at, false);
  VarDeclarator declarator{
      .name = run.descriptorVar->name,
      .loc = run.at,
      .init = init,
      .sym = run.descriptorVar,
  };
  varDecl->decls.push_back(std::move(declarator));
  run.classDecl->decorations.push_back(varDecl);
}

VarSymbol *DecoratorChecker::declareMethodImpl(const DecorateRun &run) {
  const FunctionDecl *method = run.method;
  auto *implementation = this->state.getAst().newVar();
  std::string implName = kHiddenPrefix.str() + run.classDecl->name.str() + "." +
                         method->name.str() + "." + std::to_string(method->variant());
  implementation->name = this->state.getAst().keepName(implName);
  implementation->loc = method->loc();
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  implementation->type = run.descriptorType->field(valueName)->type;
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
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_VALUE, value, at);
  Expr *writable = this->state.getAst().make<BoolLitExpr>(at, true);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_WRITABLE, writable, at);
  Expr *enumerable = this->state.getAst().make<BoolLitExpr>(at, false);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_ENUMERABLE, enumerable, at);
  Expr *configurable = this->state.getAst().make<BoolLitExpr>(at, true);
  this->addProperty(literal, BuiltinProperty::DESCRIPTOR_CONFIGURABLE, configurable, at);
  auto *checked = this->exprChecker->checkExpr(literal, run.descriptorType);

  return this->typeRelations->coerce(checked, run.descriptorType);
}

void DecoratorChecker::addProperty(ObjectLitExpr *literal, BuiltinProperty property, Expr *value,
                                   llvm::SMLoc at) const {
  auto name = BuiltinPropertyTable::getName(property);
  literal->props.push_back({.name = name, .loc = at, .value = value});
}

const FunctionDecl *DecoratorChecker::designConstructor(const DecorateRun &run) const {
  if (run.ownPlace != Place::CLASS) {
    return nullptr;
  }
  const auto &constructors = run.classDecl->constructors;
  if (constructors.size() == 1) {
    return constructors[0];
  }
  auto decorated = this->decoratedConstructors(run.classDecl);

  return decorated.size() == 1 ? decorated[0] : nullptr;
}

void DecoratorChecker::applyDecorator(DecorateRun &run, size_t index) {
  const DecoratorUse &use = run.uses[index];
  if (run.callees[index] == nullptr) {
    this->applyMetadata(run, use);
    return;
  }
  llvm::SMLoc loc = use.decorator->loc;
  Place place = this->usePlace(run, use);
  auto args = this->decoratorArgs(run, use, place);
  args.resize(std::min(args.size(), run.takes[index]));
  auto *call = this->state.getAst().make<CallExpr>(loc, run.callees[index], std::move(args));
  auto *checked = this->exprChecker->checkExpr(call);
  auto *applied = this->decoratorOutcome(run, use, place, checked);
  auto *statement = this->state.getAst().make<ExprStmt>(loc, applied);
  run.classDecl->decorations.push_back(statement);
}

void DecoratorChecker::applyMetadata(DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  Place place = this->usePlace(run, use);
  auto *metadata = cast<CallExpr>(use.decorator->expr);
  auto pair = this->checkMetadataArgs(metadata);
  if (pair.isErr()) {
    this->state.getDiagnostics().error(loc, pair.error().message);
    return;
  }
  auto reflectName = GlobalNameTable::getName(GlobalName::REFLECT);
  auto *reflect = this->state.getAst().make<NameExpr>(loc, reflectName);
  auto defineName = ReflectFunctionTable::getName(ReflectFunction::DEFINE_METADATA);
  auto *callee = this->state.getAst().make<MemberExpr>(loc, reflect, defineName);
  auto *target = this->classValueChecker->classValueAt(run.classDecl, loc);
  std::vector<Expr *> args{metadata->args[0], metadata->args[1], target};
  if (place != Place::CLASS && !this->isConstructorParamUse(use)) {
    auto *key = this->memberKey(run, use);
    args.push_back(key);
  }
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, args);
  auto *checked = this->exprChecker->checkExpr(call);
  auto *statement = this->state.getAst().make<ExprStmt>(loc, checked);
  run.classDecl->decorations.push_back(statement);
}

std::vector<Expr *> DecoratorChecker::decoratorArgs(const DecorateRun &run, const DecoratorUse &use,
                                                    Place place) {
  llvm::SMLoc loc = use.decorator->loc;
  ClassDecl *target = place == Place::CLASS ? run.current : run.classDecl;
  auto *classValue = this->classValueChecker->classValueAt(target, loc);
  std::vector<Expr *> args{classValue};
  if (place != Place::CLASS) {
    auto *key = this->memberKey(run, use);
    args.push_back(key);
  }
  if (place == Place::PARAMETER) {
    auto *position = this->state.getAst().make<IntLitExpr>(loc, uint64_t(use.param));
    args.push_back(position);
  } else if (place == Place::METHOD && run.descriptorVar != nullptr) {
    auto *descriptor = this->state.getAst().make<NameExpr>(loc, run.descriptorVar->name);
    args.push_back(descriptor);
  }

  return args;
}

Expr *DecoratorChecker::memberKey(const DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  if (this->isConstructorParamUse(use)) {
    return this->state.getAst().make<UndefinedExpr>(loc);
  }
  auto name = this->useOwnerName(run, use);
  auto text = std::u16string(name.begin(), name.end());

  return this->state.getAst().make<StringLitExpr>(loc, text);
}

Expr *DecoratorChecker::decoratorOutcome(DecorateRun &run, const DecoratorUse &use, Place place,
                                         Expr *applied) {
  llvm::SMLoc loc = use.decorator->loc;
  const Type *result = applied->type;
  if (run.descriptorVar != nullptr && place == Place::METHOD && result == run.descriptorType) {
    return this->assignDescriptor(run, use, applied);
  }
  const ClassDecl *made = result->classRef();
  if (place == Place::CLASS && made != nullptr && made->derivesFrom(run.current)) {
    auto *given = const_cast<ClassDecl *>(made);
    if (given != run.current) {
      this->lateClasses->replaceClass(run.current, given, loc);
      run.current = given;
    }
    return applied;
  }
  auto label = this->decoratorLabel(use.decorator->expr);
  bool hasDescriptor = run.descriptorVar != nullptr;
  auto returned = this->checkDecoratorResult(label, result, place, hasDescriptor, run.current);
  if (returned.isErr()) {
    this->state.getDiagnostics().error(loc, returned.error().message);
  }

  return applied;
}

Expr *DecoratorChecker::assignDescriptor(const DecorateRun &run, const DecoratorUse &use,
                                         Expr *applied) {
  llvm::SMLoc loc = use.decorator->loc;
  auto *target = this->state.getAst().make<NameExpr>(loc, run.descriptorVar->name);
  target->binding = NamedVar{.var = run.descriptorVar};
  target->type = run.descriptorType;
  auto *assign = this->state.getAst().make<AssignExpr>(loc, std::nullopt, target, applied);
  assign->type = run.descriptorType;

  return assign;
}

void DecoratorChecker::storeDescriptorValue(const DecorateRun &run) {
  llvm::SMLoc at = run.at;
  auto *target = this->state.getAst().make<NameExpr>(at, run.method->impl()->name);
  auto *descriptor = this->state.getAst().make<NameExpr>(at, run.descriptorVar->name);
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  auto *value = this->state.getAst().make<MemberExpr>(at, descriptor, valueName);
  auto *assign = this->state.getAst().make<AssignExpr>(at, std::nullopt, target, value);
  auto *checked = this->exprChecker->checkExpr(assign);
  auto *statement = this->state.getAst().make<ExprStmt>(at, checked);
  run.classDecl->decorations.push_back(statement);
}

}
