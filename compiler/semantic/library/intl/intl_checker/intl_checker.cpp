#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/date/date_format_method_table/date_format_method_table.h"
#include "compiler/program/library/intl/intl_function_table/intl_function_table.h"
#include "compiler/program/library/intl/intl_method_table/intl_method_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/new_checker/new_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker.h"
#include "compiler/semantic/library/date/date_format_checker/date_format_checker_errors.h"
#include "compiler/semantic/library/intl/intl_options_checker/intl_options_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

IntlChecker::IntlChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->newChecker = &all.newChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
  this->topLevelChecker = &all.topLevelChecker;
  this->dateFormatChecker = &all.dateFormatChecker;
  this->intlOptionsChecker = &all.intlOptionsChecker;
}

Expr *IntlChecker::kindArg(AstContext &context, llvm::SMLoc loc, int kind) const {
  auto *literal = context.make<IntLitExpr>(loc, uint64_t(kind));
  literal->type = Type::intTy();

  return literal;
}

std::optional<std::u16string> IntlChecker::literalText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

const Expr *IntlChecker::unconverted(const Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

bool IntlChecker::isStringLike(const Type *type) const {
  auto isStringLiteral = std::bind_front(&IntlChecker::isStringLiteralMember, this);

  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() && llvm::all_of(type->members(), isStringLiteral));
}

bool IntlChecker::isStringLiteralMember(const Type *member) const {
  return member->isStringLiteral();
}

const Type *IntlChecker::numberOptionType(IntlOptionKey key) const {
  bool isIncrement = key == IntlOptionKey::ROUNDING_INCREMENT;
  auto increments = IntlServiceTable::getRoundingIncrements();
  auto values = isIncrement ? std::vector<int64_t>(increments.begin(), increments.end())
                            : this->digitRange(key);
  auto members = std::vector<const Type *>();
  for (int64_t value : values) {
    const auto *literal = Type::numberLiteral(double(value), Type::intTy());
    members.push_back(literal);
  }
  const auto *type = Type::unionOf(members);
  if (!isIncrement) {
    auto rangeName = std::to_string(values.front()) + ".." + std::to_string(values.back());
    Type::nameUnion(type, rangeName);
  }

  return type;
}

std::vector<int64_t> IntlChecker::digitRange(IntlOptionKey key) const {
  auto range = IntlServiceTable::findDigitRange(key);
  auto values = std::vector<int64_t>();
  for (int64_t value = range->low; value <= range->high; ++value) {
    values.push_back(value);
  }

  return values;
}

const Type *IntlChecker::segmentDataType() const {
  return this->shapeType(IntlShape::SEGMENT_DATA);
}

const Type *IntlChecker::shapeType(IntlShape shape) const {
  const IntlShapeInfo &info = IntlShapeTable::find(shape);
  auto fields = std::vector<Field>();
  for (const IntlShapeField &field : info.fields) {
    const Type *type = this->shapeValueType(field.type);
    fields.push_back({field.name.str(), type, false});
  }

  return Type::object(std::move(fields), info.typeName);
}

const Type *IntlChecker::shapeValueType(IntlValueType type) const {
  switch (type) {
    case IntlValueType::TEXT:
      return Type::string();
    case IntlValueType::MAYBE_TEXT:
      return Type::optional(Type::string());
    case IntlValueType::FLAG:
      return Type::boolTy();
    case IntlValueType::MAYBE_FLAG:
      return Type::optional(Type::boolTy());
    case IntlValueType::INT:
      return Type::intTy();
    case IntlValueType::MAYBE_INT:
      return Type::optional(Type::intTy());
    case IntlValueType::TEXT_OR_FLAG:
      return Type::unionOf({Type::string(), Type::boolTy()});
    case IntlValueType::TEXT_LIST:
      return Type::array(Type::string());
  }

  return Type::error();
}

const Type *IntlChecker::resolvedIntlOptionsType(IntlKind kind) const {
  return this->shapeType(IntlShapeTable::resolvedOf(kind));
}

const Type *IntlChecker::intlTypeNamed(llvm::StringRef name) const {
  const auto *service = IntlServiceTable::find(name);
  if (service != nullptr) {
    return Type::intl(service->kind);
  }
  auto typeName = IntlTypeNameTable::find(name);
  if (!typeName.has_value()) {
    return this->serviceTypeNamed(name);
  }
  switch (*typeName) {
    case IntlTypeName::DATE_TIME_FORMAT:
      return Type::dateFormat();
    case IntlTypeName::SEGMENTS:
      return Type::intl(IntlKind::Segments);
    case IntlTypeName::SEGMENT_DATA:
      return this->segmentDataType();
    case IntlTypeName::DATE_TIME_FORMAT_PART:
    case IntlTypeName::NUMBER_FORMAT_PART:
      return this->shapeType(IntlShape::DATE_TIME_FORMAT_PART);
    case IntlTypeName::RELATIVE_TIME_FORMAT_PART:
      return this->shapeType(IntlShape::RELATIVE_TIME_FORMAT_PART);
    case IntlTypeName::RESOLVED_DATE_TIME_FORMAT_OPTIONS:
      return this->shapeType(IntlShape::RESOLVED_DATE_TIME_FORMAT);
    case IntlTypeName::DATE_TIME_FORMAT_OPTIONS:
      return this->dateFormatChecker->dateOptionsType();
    case IntlTypeName::LDML_PLURAL_RULE:
    case IntlTypeName::RELATIVE_TIME_FORMAT_UNIT:
    case IntlTypeName::UNICODE_BCP47_LOCALE_IDENTIFIER:
    case IntlTypeName::BCP47_LANGUAGE_TAG:
      return Type::string();
    case IntlTypeName::LOCALE:
      break;
  }

  return this->serviceTypeNamed(name);
}

const Type *IntlChecker::serviceTypeNamed(llvm::StringRef name) const {
  const auto *resolved = IntlServiceTable::findByResolvedTypeName(name);
  if (resolved != nullptr) {
    return this->resolvedIntlOptionsType(resolved->kind);
  }
  const auto *options = IntlServiceTable::findByOptionsTypeName(name);
  if (options != nullptr) {
    return this->serviceOptionsType(*options);
  }

  return nullptr;
}

const Type *IntlChecker::serviceOptionsType(const IntlService &service) const {
  auto fields = std::vector<Field>();
  for (const IntlOption &option : service.options) {
    const auto *type = this->optionType(option);
    auto name = IntlOptionKeyTable::getName(option.key);
    fields.push_back({.name = name.str(), .type = Type::optional(type)});
  }
  auto typeName = IntlServiceTable::getOptionsTypeName(service.name);

  return Type::object(std::move(fields), typeName);
}

const Type *IntlChecker::optionType(const IntlOption &option) const {
  if (option.kind == IntlOptionKind::NUMBER) {
    return this->numberOptionType(option.key);
  }
  bool isTextual = option.kind == IntlOptionKind::TEXT || option.kind == IntlOptionKind::GROUPING;
  if (isTextual && !option.values.empty()) {
    return this->listedOptionType(option);
  }
  if (option.kind == IntlOptionKind::TEXT) {
    return Type::string();
  }
  if (option.kind == IntlOptionKind::FLAG) {
    return Type::boolTy();
  }

  return Type::unionOf({Type::string(), Type::boolTy()});
}

const Type *IntlChecker::listedOptionType(const IntlOption &option) const {
  auto members = std::vector<const Type *>();
  for (llvm::StringLiteral value : option.values) {
    auto text = ToUtf16::toUtf16(value);
    members.push_back(Type::stringLiteral(text));
  }
  if (option.kind == IntlOptionKind::GROUPING) {
    members.push_back(Type::boolTy());
  }

  return Type::unionOf(members);
}

Expr *IntlChecker::checkIntlNew(llvm::SMLoc loc, llvm::StringRef name, std::vector<Expr *> &args,
                                IntlCreation creation) {
  const auto *service = IntlServiceTable::find(name);
  if (service == nullptr) {
    auto message = ErrorTextIntlNew::unknown(name);
    this->state.getDiagnostics().error(loc, message);
    return this->failIntlNew(loc, args);
  }
  if (creation == IntlCreation::CALL && !service->callable) {
    auto message = ErrorTextIntlNew::withNew(name);
    this->state.getDiagnostics().error(loc, message);
    return this->failIntlNew(loc, args);
  }
  if (args.size() > 2) {
    auto message = ErrorTextDateFormat::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    return this->failIntlNew(loc, args);
  }
  if (service->kind == IntlKind::DisplayNames && args.size() < 2) {
    auto message = ErrorTextIntlNew::noType();
    this->state.getDiagnostics().error(loc, message);
    return this->failIntlNew(loc, args);
  }
  auto what = ErrorTextIntlNew::label(name);
  if (!this->intlOptionsChecker->prepareIntlArgs(args, 0, service->kind, what)) {
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  }
  auto *kind = this->kindArg(this->state.getAst(), loc, service->runtimeKind);
  std::vector<Expr *> all{kind};
  all.insert(all.end(), args.begin(), args.end());
  auto intlName = GlobalNameTable::getName(GlobalName::INTL);
  auto *callee = this->state.getAst().make<NameExpr>(loc, intlName);
  const auto *intlType = Type::intl(service->kind);

  return this->processChecker->builtinCall(loc, callee, Builtin::IntlNew, std::move(all), intlType);
}

Expr *IntlChecker::failIntlNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  this->walkArgsLoosely(args);
  auto *error = this->state.getAst().make<UndefinedExpr>(loc);
  error->type = Type::error();

  return error;
}

IntlChecker::ResultLanguageTags
IntlChecker::checkLanguageTags(const Expr *arg, llvm::StringRef what, LanguageTagUse use) {
  auto tags = this->literalTypes->knownTexts(arg);
  if (!tags.has_value()) {
    bool isConstructor = use == LanguageTagUse::LOCALE_CONSTRUCTOR;
    auto message = ErrorTextLanguageTag::unknown(isConstructor, what);

    return err(IntlCheckerLanguageTagsError{
        .code = IntlCheckerLanguageTagsErrorCode::UNKNOWN_TAGS,
        .message = message,
    });
  }
  for (const std::string &tag : *tags) {
    if (!this->dateFormatChecker->isLanguageTag(tag)) {
      auto message = ErrorTextLanguageTag::notTag(tag);

      return err(IntlCheckerLanguageTagsError{
          .code = IntlCheckerLanguageTagsErrorCode::NOT_A_TAG,
          .message = message,
      });
    }
  }

  return ok(*tags);
}

bool IntlChecker::prepareLocaleArg(Expr *&arg, llvm::StringRef what) {
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return true;
  }
  auto *locale = this->state.getLibraryDeclarations().findLibraryClass(LibraryClass::LOCALE);
  if (locale != nullptr && !type->isOptional() && this->classDeclarer->classOf(type) == locale) {
    auto tagField = IntlShapeTable::getLocaleTagField();
    auto *tag = this->state.getAst().make<MemberExpr>(arg->loc(), arg, tagField);
    tag->type = Type::string();
    arg = tag;
    return true;
  }
  if (!this->isStringLike(type->nonOptional())) {
    auto message = ErrorTextLanguageTag::takes(what, type->name(), type->isArray());
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  auto tags = this->checkLanguageTags(arg, what, LanguageTagUse::LOCALE_ARGUMENT);
  if (tags.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), tags.error().message);
    return false;
  }
  if (!type->nonOptional()->isString()) {
    const Type *text = type->isOptional() ? Type::optional(Type::string()) : Type::string();
    arg = this->typeRelations->convert(arg, text);
  }

  return true;
}

Expr *IntlChecker::checkLocaleList(Expr *arg, llvm::StringRef what) {
  bool isArray = isa<ArrayLitExpr>(arg);
  const Type *hint = isArray ? Type::array(Type::string()) : nullptr;
  auto *checked = this->exprChecker->checkExpr(arg, hint);
  const Type *type = checked->type;
  if (type->isError()) {
    return checked;
  }
  if (!type->isArray()) {
    this->prepareLocaleArg(checked, what);
    return checked;
  }
  if (type != Type::array(Type::string())) {
    auto message = ErrorTextLanguageTag::list(what, type->name());
    this->state.getDiagnostics().error(checked->loc(), message);
    return checked;
  }
  const auto *list = dyn_cast<ArrayLitExpr>(this->unconverted(checked));
  if (list == nullptr) {
    auto message = ErrorTextLanguageTag::listOpen(what);
    this->state.getDiagnostics().error(checked->loc(), message);
    return checked;
  }
  this->walkLocaleElements(list, what);

  return checked;
}

void IntlChecker::walkLocaleElements(const ArrayLitExpr *list, llvm::StringRef what) {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      auto message = ErrorTextLanguageTag::spread(what);
      this->state.getDiagnostics().error(element.expr->loc(), message);
      return;
    }
    auto tags = this->checkLanguageTags(element.expr, what, LanguageTagUse::LOCALE_ARGUMENT);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(element.expr->loc(), tags.error().message);
      return;
    }
  }
}

Expr *IntlChecker::checkLocaleNew(NewExpr *expr, const Type *expected) {
  if (this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls() == nullptr) {
    auto message = ErrorTextIntlLocale::missing();
    this->state.getDiagnostics().error(expr->loc(), message);
    this->walkArgsLoosely(expr->args);
    return expr;
  }
  if (expr->args.size() == 2) {
    auto message = ErrorTextIntlLocale::options();
    this->state.getDiagnostics().error(expr->args[1]->loc(), message);
    this->walkArgsLoosely(expr->args);
    return expr;
  }
  expr->name = LibraryModuleNames::kIntlLocaleName;
  expr->member = "";
  auto *checked = this->newChecker->checkNew(expr, expected);
  auto *made = dyn_cast<NewExpr>(checked);
  auto *args = made != nullptr ? this->constructedArgs(made) : nullptr;
  bool isOneTag = args != nullptr && args->size() == 1 && args->front()->type != nullptr &&
                  !args->front()->type->isError();
  if (isOneTag) {
    auto what = ErrorTextIntlLocale::label();
    auto tags = this->checkLanguageTags(args->front(), what, LanguageTagUse::LOCALE_CONSTRUCTOR);
    if (tags.isErr()) {
      this->state.getDiagnostics().error(args->front()->loc(), tags.error().message);
    }
  }

  return checked;
}

std::vector<Expr *> *IntlChecker::constructedArgs(NewExpr *made) const {
  if (made->ctor() != nullptr) {
    return &made->ctor()->args;
  }

  return &made->args;
}

void IntlChecker::walkArgsLoosely(std::vector<Expr *> &args) {
  for (Expr *&arg : args) {
    arg = this->exprChecker->checkExpr(arg);
  }
}

Expr *IntlChecker::checkLocaleStatic(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (this->topLevelChecker->lookupValue(LibraryModuleNames::kIntlLocaleName).cls() == nullptr) {
    auto message = ErrorTextIntlLocale::missing();
    this->state.getDiagnostics().error(member->loc(), message);
    call->type = Type::error();
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  member->object = this->state.getAst().make<NameExpr>(member->object->loc(),
                                                       LibraryModuleNames::kIntlLocaleName);

  return this->callChecker->checkCall(call, expected);
}

Expr *IntlChecker::checkIntlFunction(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto function = IntlFunctionTable::find(member->name);
  if (function == IntlFunction::GET_CANONICAL_LOCALES) {
    return this->checkCanonicalLocales(call, member);
  }
  if (function == IntlFunction::SUPPORTED_VALUES_OF) {
    auto message = ErrorTextIntlFunction::values();
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  if (LibraryClassTable::isName(member->name, LibraryClass::LOCALE)) {
    auto message = ErrorTextIntlLocale::withNew();
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }

  return this->checkIntlNew(member->loc(), member->name, call->args, IntlCreation::CALL);
}

Expr *IntlChecker::checkCanonicalLocales(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (!call->args.empty()) {
    auto what = ErrorTextIntlFunction::canonical();
    call->args[0] = this->checkLocaleList(call->args[0], what);
  }
  call->target = BuiltinCall{.builtin = Builtin::IntlCanonicalLocales};
  call->type = Type::array(Type::string());

  return call;
}

Expr *IntlChecker::checkIntlStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto *service = cast<MemberExpr>(member->object);
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto what = ErrorTextIntlFunction::staticOf(service->name, member->name);
  bool isDateFormat = IntlTypeNameTable::find(service->name) == IntlTypeName::DATE_TIME_FORMAT;
  bool isService = isDateFormat || IntlServiceTable::find(service->name) != nullptr;
  bool isLocales = IntlFunctionTable::find(member->name) == IntlFunction::SUPPORTED_LOCALES_OF;
  if (!isService || !isLocales) {
    auto message = ErrorTextIntlFunction::onlyLocale(what);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->checkLocaleList(call->args[0], what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (call->args.size() == 2 && !this->prepareStaticOptions(call, member, what)) {
    return call;
  }
  auto units = ToUtf16::toUtf16(what);
  auto *label = this->state.getAst().make<StringLitExpr>(member->loc(), units);
  label->type = Type::string();
  call->args.insert(call->args.begin(), label);
  call->target = BuiltinCall{.builtin = Builtin::IntlSupportedLocales};
  call->type = Type::array(Type::string());

  return call;
}

bool IntlChecker::prepareStaticOptions(CallExpr *call, MemberExpr *member, llvm::StringRef what) {
  auto *none = this->state.getAst().make<UndefinedExpr>(member->loc());
  std::vector<Expr *> pair{none, call->args[1]};
  if (!this->intlOptionsChecker->prepareIntlArgs(pair, 0, IntlKind::None, what)) {
    return false;
  }
  call->args[1] = pair[1];

  return true;
}

Expr *IntlChecker::checkIntlMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  IntlKind kind = member->object->type->intlKind();
  llvm::StringRef name = member->name;
  auto wanted = this->wantedCallback(expected);
  const auto *functionType = this->boundFunctionType(kind, name, wanted);
  if (functionType == nullptr) {
    std::string message = kind == IntlKind::DateTimeFormat
                              ? ErrorTextIntlMember::dateFormat(name)
                              : ErrorTextIntlMember::onlyCalled(name, member->object->type->name());
    this->state.getDiagnostics().error(member->loc(), message);
    return member;
  }
  member->type = functionType;

  return this->processChecker->builtinCall(member->loc(), member, Builtin::IntlBound, {},
                                           functionType);
}

IntlChecker::WantedCallback IntlChecker::wantedCallback(const Type *expected) const {
  const auto *callback = this->state.getFunctionStack().getExpectedCallback();
  if (callback != nullptr && !callback->params.empty()) {
    return WantedCallback{.param = callback->params[0], .ret = callback->ret};
  }
  if (expected != nullptr && expected->nonOptional()->isFunction()) {
    const Type *functionType = expected->nonOptional();
    const Type *param = functionType->params().empty() ? nullptr : functionType->params()[0];
    return WantedCallback{.param = param, .ret = functionType->ret()};
  }

  return WantedCallback{.param = nullptr, .ret = nullptr};
}

const Type *IntlChecker::boundFunctionType(IntlKind kind, llvm::StringRef name,
                                           const WantedCallback &wanted) const {
  const auto *method = IntlMethodTable::find(kind, name);
  bool isNumberFormat = method != nullptr && method->method == IntlMethod::NUMBER_FORMAT;
  bool isCompare = method != nullptr && method->method == IntlMethod::COLLATOR_COMPARE;
  bool isDateFormat = kind == IntlKind::DateTimeFormat &&
                      DateFormatMethodTable::find(name) == DateFormatMethod::FORMAT;
  if (isNumberFormat) {
    bool isNumber =
        wanted.param != nullptr && (wanted.param->isNumeric() || wanted.param->isDecimal());
    const Type *param = isNumber ? wanted.param : Type::doubleTy();
    return Type::function({param}, 1, false, Type::string());
  }
  if (isDateFormat) {
    bool isNumber = wanted.param != nullptr && wanted.param->isNumeric();
    const Type *param = isNumber ? wanted.param : Type::date();
    return Type::function({param}, 1, false, Type::string());
  }
  if (isCompare) {
    bool isFloat = wanted.ret != nullptr && wanted.ret->isFloat();
    const Type *returnType = isFloat ? Type::doubleTy() : Type::intTy();
    return Type::function({Type::string(), Type::string()}, 2, false, returnType);
  }

  return nullptr;
}

Expr *IntlChecker::segmentsArray(Expr *segments) {
  auto *member = this->state.getAst().make<MemberExpr>(segments->loc(), segments, "%segments");
  const auto *type = Type::array(this->segmentDataType());
  member->type = type;

  return this->processChecker->builtinCall(segments->loc(), member, Builtin::SegmentsArray, {},
                                           type);
}

}
