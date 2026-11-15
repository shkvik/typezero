#include "compiler/semantic/library_v2/intl/library_intl_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/date/date_format_method_table/date_format_method_table.h"
#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"
#include "compiler/program/library/intl/intl_function_table/intl_function_table.h"
#include "compiler/program/library/intl/intl_option_key_table/intl_option_key_table.h"
#include "compiler/program/library/intl/intl_shape_table/intl_shape_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/program/library/intl/intl_unit_table/intl_unit_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/library/string/normalization_form_table/normalization_form_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "runtime/library/date/date/date.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kFewestDigits = 1;
constexpr int64_t kMostDigits = 3;

}

LibraryIntlRegistry::LibraryIntlRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryIntlRegistry::findIntlType(const FindIntlType &task) {
  const auto *type = this->state.library.intl.intlTypeNamed(task.name);

  return this->state.walk.tasks.pushAnswer(type);
}

void LibraryIntlRegistry::segmentsArraySlot(const SegmentsArraySlot &task) {
  Expr *segments = *task.slot;
  auto *member = this->state.ast.make<MemberExpr>(segments->loc(), segments, "%segments");
  const auto *type = Type::array(this->state.library.intl.segmentDataType());
  member->type = type;
  *task.slot = this->state.library.common.builtinCall(segments->loc(), member,
                                                      Builtin::SegmentsArray, {}, type);
}

void LibraryIntlRegistry::checkIntlNew(const CheckIntlNew &task) {
  NewExpr *newExpr = task.newExpr;

  return this->startIntlNew(newExpr->loc(), newExpr->member, &newExpr->args, IntlCreation::NEW);
}

void LibraryIntlRegistry::startIntlNew(llvm::SMLoc loc, llvm::StringRef name,
                                       std::vector<Expr *> *args, IntlCreation creation) {
  auto &tasks = this->state.walk.tasks;
  const auto *service = IntlServiceTable::find(name);
  auto created = this->checkIntlCreation(service, name, args->size(), creation);
  if (created.isErr()) {
    this->state.diagnostics.error(loc, created.error().message);

    return this->failIntlNew(loc, args);
  }
  auto what = ErrorTextIntlNew::label(name);
  tasks.pushTask(FinishIntlNew{
      .loc = loc,
      .args = args,
      .kind = service->kind,
      .runtimeKind = service->runtimeKind,
  });

  return tasks.pushTask(
      PrepareIntlArgs{.args = args, .from = 0, .kind = service->kind, .what = what});
}

void LibraryIntlRegistry::finishIntlNew(const FinishIntlNew &task) {
  auto &tasks = this->state.walk.tasks;
  bool isPrepared = tasks.takeVerdict();
  if (!isPrepared) {
    return tasks.pushChecked(this->failedValue(task.loc));
  }
  auto *kind = this->kindArg(task.loc, task.runtimeKind);
  auto all = std::vector<Expr *>{kind};
  all.insert(all.end(), task.args->begin(), task.args->end());
  auto intlName = GlobalNameTable::getName(GlobalName::INTL);
  auto *callee = this->state.ast.make<NameExpr>(task.loc, intlName);
  const auto *intlType = Type::intl(task.kind);
  auto *made = this->state.library.common.builtinCall(task.loc, callee, Builtin::IntlNew,
                                                      std::move(all), intlType);

  return tasks.pushChecked(made);
}

void LibraryIntlRegistry::failIntlNew(llvm::SMLoc loc, std::vector<Expr *> *args) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(this->failedValue(loc));
  for (Expr *&arg : llvm::reverse(*args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

Expr *LibraryIntlRegistry::failedValue(llvm::SMLoc loc) {
  auto *error = this->state.ast.make<UndefinedExpr>(loc);
  error->type = Type::error();

  return error;
}

void LibraryIntlRegistry::prepareIntlArgs(const PrepareIntlArgs &task) {
  std::vector<Expr *> &args = *task.args;
  size_t from = task.from;
  auto &tasks = this->state.walk.tasks;
  Expr **options = args.size() > from + 1 ? &args[from + 1] : nullptr;
  tasks.pushTask(IntlOptionsArg{.options = options, .kind = task.kind, .what = task.what});
  if (args.size() <= from) {
    return tasks.pushVerdict(true);
  }
  tasks.pushTask(LocaleArgSlot{.slot = &args[from], .what = task.what});

  return tasks.pushTask(CheckExprSlot{.slot = &args[from], .expected = nullptr});
}

void LibraryIntlRegistry::localeArgSlot(const LocaleArgSlot &task) {
  bool isPrepared = this->prepareLocaleArg(task.slot, task.what);

  return this->state.walk.tasks.pushVerdict(isPrepared);
}

void LibraryIntlRegistry::intlOptionsArg(const IntlOptionsArg &task) {
  auto &tasks = this->state.walk.tasks;
  bool isLocaleOk = tasks.takeVerdict();
  if (task.options == nullptr) {
    return tasks.pushVerdict(isLocaleOk);
  }
  tasks.pushTask(AfterIntlOptions{
      .options = task.options,
      .kind = task.kind,
      .what = task.what,
      .isLocaleOk = isLocaleOk,
  });

  return tasks.pushTask(CheckExprSlot{.slot = task.options, .expected = nullptr});
}

void LibraryIntlRegistry::afterIntlOptions(const AfterIntlOptions &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *options = *task.options;
  const Type *type = options->type;
  if (type->isError()) {
    return tasks.pushVerdict(false);
  }
  auto passed = this->checkOptionsGiven(type, task.kind, task.what);
  if (passed.isErr()) {
    this->state.diagnostics.error(options->loc(), passed.error().message);

    return tasks.pushVerdict(false);
  }
  if (type->isUndefined()) {
    return tasks.pushVerdict(task.isLocaleOk);
  }
  const IntlService *service = task.kind == IntlKind::None
                                   ? &IntlServiceTable::getSupportedLocalesService()
                                   : IntlServiceTable::findKind(task.kind);
  auto run = IntlOptionsRun{
      .type = type,
      .literal = dyn_cast<ObjectLitExpr>(options),
      .options = options,
      .kind = task.kind,
      .what = task.what,
      .service = service,
      .ok = task.isLocaleOk,
  };
  for (const Field &field : type->fields()) {
    this->walkOptionField(run, field);
  }
  if (!run.ok) {
    return tasks.pushVerdict(false);
  }
  this->walkOptionTexts(run);
  this->walkDigitRanges(run);
  this->walkDigitPairs(run);
  this->walkRoundingIncrement(run);

  return tasks.pushVerdict(run.ok);
}

bool LibraryIntlRegistry::prepareLocaleArg(Expr **slot, llvm::StringRef what) {
  Expr *arg = *slot;
  const Type *type = arg->type;
  if (type->isError()) {
    return false;
  }
  if (type->isUndefined()) {
    return true;
  }
  auto *locale = this->state.library.declarations.findLibraryClass(LibraryClass::LOCALE);
  if (locale != nullptr && !type->isOptional() && type->classDecl() == locale) {
    auto tagField = IntlShapeTable::getLocaleTagField();
    auto *tag = this->state.ast.make<MemberExpr>(arg->loc(), arg, tagField);
    tag->type = Type::string();
    *slot = tag;

    return true;
  }
  if (!this->isStringLike(type->nonOptional())) {
    auto message = ErrorTextLanguageTag::takes(what, type->name(), type->isArray());
    this->state.diagnostics.error(arg->loc(), message);

    return false;
  }
  auto tags = this->checkLanguageTags(arg, what, LanguageTagUse::LOCALE_ARGUMENT);
  if (tags.isErr()) {
    this->state.diagnostics.error(arg->loc(), tags.error().message);

    return false;
  }
  if (type->nonOptional()->isString()) {
    return true;
  }
  const Type *text = type->isOptional() ? Type::optional(Type::string()) : Type::string();
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = slot, .to = text});

  return true;
}

void LibraryIntlRegistry::localeListSlot(const LocaleListSlot &task) {
  auto &tasks = this->state.walk.tasks;
  bool isArray = isa<ArrayLitExpr>(*task.slot);
  const Type *hint = isArray ? Type::array(Type::string()) : nullptr;
  tasks.pushTask(AfterLocaleList{.slot = task.slot, .what = task.what});

  return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = hint});
}

void LibraryIntlRegistry::afterLocaleList(const AfterLocaleList &task) {
  const Expr *checked = *task.slot;
  const Type *type = checked->type;
  if (type->isError()) {
    return;
  }
  if (!type->isArray()) {
    this->prepareLocaleArg(task.slot, task.what);

    return;
  }
  if (type != Type::array(Type::string())) {
    auto message = ErrorTextLanguageTag::list(task.what, type->name());

    return this->state.diagnostics.error(checked->loc(), message);
  }
  const auto *list = dyn_cast<ArrayLitExpr>(this->unconverted(checked));
  if (list == nullptr) {
    auto message = ErrorTextLanguageTag::listOpen(task.what);

    return this->state.diagnostics.error(checked->loc(), message);
  }

  return this->walkLocaleElements(list, task.what);
}

void LibraryIntlRegistry::checkLocaleNew(const CheckLocaleNew &task) {
  NewExpr *newExpr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  auto present = this->checkLocalePresent(newExpr->args.size());
  if (present.isErr()) {
    bool isOptions =
        present.error().code == LibraryIntlRegistryLocalePresentErrorCode::OPTIONS_GIVEN;
    llvm::SMLoc at = isOptions ? newExpr->args[1]->loc() : newExpr->loc();
    this->state.diagnostics.error(at, present.error().message);
    tasks.pushChecked(newExpr);

    return this->walkArgsLoosely(&newExpr->args);
  }
  newExpr->name = LibraryModuleNames::kIntlLocaleName;
  newExpr->member = "";
  tasks.pushTask(AfterLocaleNew{});

  return tasks.pushTask(CheckNew{.newExpr = newExpr, .expected = task.expected});
}

void LibraryIntlRegistry::afterLocaleNew() {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  tasks.pushChecked(checked);
  auto *made = dyn_cast<NewExpr>(checked);
  auto *args = made != nullptr ? this->constructedArgs(made) : nullptr;
  bool isOneTag = args != nullptr && args->size() == 1 && args->front()->type != nullptr &&
                  !args->front()->type->isError();
  if (!isOneTag) {
    return;
  }
  auto what = ErrorTextIntlLocale::label();
  auto tags = this->checkLanguageTags(args->front(), what, LanguageTagUse::LOCALE_CONSTRUCTOR);
  if (tags.isErr()) {
    return this->state.diagnostics.error(args->front()->loc(), tags.error().message);
  }
}

void LibraryIntlRegistry::walkArgsLoosely(std::vector<Expr *> *args) {
  for (Expr *&arg : llvm::reverse(*args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryIntlRegistry::checkLocaleStatic(const CheckLocaleStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasLocaleClass()) {
    auto message = ErrorTextIntlLocale::missing();
    this->state.diagnostics.error(member->loc(), message);
    call->type = Type::error();
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  member->object =
      this->state.ast.make<NameExpr>(member->object->loc(), LibraryModuleNames::kIntlLocaleName);

  return tasks.pushTask(CheckCall{.call = call, .expected = task.expected});
}

bool LibraryIntlRegistry::hasLocaleClass() const {
  auto entry = this->state.walk.scopes.findName(LibraryModuleNames::kIntlLocaleName);

  return entry.cls() != nullptr;
}

void LibraryIntlRegistry::checkIntlCall(const CheckIntlCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  bool isDateFormat = IntlTypeNameTable::find(member->name) == IntlTypeName::DATE_TIME_FORMAT;
  if (isDateFormat && this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (isDateFormat) {
    return this->startDateFormatNew(member->loc(), &call->args);
  }

  return this->checkIntlFunction(call, member);
}

void LibraryIntlRegistry::checkIntlFunction(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  auto function = IntlFunctionTable::find(member->name);
  bool isLocale = LibraryClassTable::isName(member->name, LibraryClass::LOCALE);
  bool isValues = function == IntlFunction::SUPPORTED_VALUES_OF;
  bool isCanonical = function == IntlFunction::GET_CANONICAL_LOCALES;
  bool hasSpread = this->state.library.common.hasSpread(call);
  if (!hasSpread && !isCanonical && !isValues && !isLocale) {
    return this->startIntlNew(member->loc(), member->name, &call->args, IntlCreation::CALL);
  }
  tasks.pushChecked(call);
  if (hasSpread) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (isCanonical) {
    return this->checkCanonicalLocales(call, member);
  }
  std::string message = isLocale ? ErrorTextIntlLocale::withNew() : ErrorTextIntlFunction::values();
  this->state.diagnostics.error(member->loc(), message);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryIntlRegistry::checkCanonicalLocales(CallExpr *call, const MemberExpr *member) {
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::IntlCanonicalLocales};
  call->type = Type::array(Type::string());
  if (call->args.empty()) {
    return;
  }
  auto what = ErrorTextIntlFunction::canonical();

  return this->state.walk.tasks.pushTask(LocaleListSlot{.slot = &call->args[0], .what = what});
}

void LibraryIntlRegistry::checkIntlStatic(const CheckIntlStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto *service = cast<MemberExpr>(member->object);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto what = ErrorTextIntlFunction::staticOf(service->name, member->name);
  bool isDateFormat = IntlTypeNameTable::find(service->name) == IntlTypeName::DATE_TIME_FORMAT;
  bool isService = isDateFormat || IntlServiceTable::find(service->name) != nullptr;
  bool isLocales = IntlFunctionTable::find(member->name) == IntlFunction::SUPPORTED_LOCALES_OF;
  if (!isService || !isLocales) {
    auto message = ErrorTextIntlFunction::onlyLocale(what);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterStaticLocaleList{.call = call, .member = member, .what = what});

  return tasks.pushTask(LocaleListSlot{.slot = &call->args[0], .what = what});
}

void LibraryIntlRegistry::afterStaticLocaleList(const AfterStaticLocaleList &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (call->args[0]->type->isError()) {
    return;
  }
  tasks.pushTask(FinishSupportedLocales{.call = call, .member = task.member, .what = task.what});
  Expr **options = call->args.size() == 2 ? &call->args[1] : nullptr;
  tasks.pushTask(IntlOptionsArg{.options = options, .kind = IntlKind::None, .what = task.what});

  return tasks.pushVerdict(true);
}

void LibraryIntlRegistry::finishSupportedLocales(const FinishSupportedLocales &task) {
  CallExpr *call = task.call;
  bool isPrepared = this->state.walk.tasks.takeVerdict();
  if (!isPrepared) {
    return;
  }
  auto units = ToUtf16::toUtf16(task.what);
  auto *label = this->state.ast.make<StringLitExpr>(task.member->loc(), units);
  label->type = Type::string();
  call->args.insert(call->args.begin(), label);
  call->target = BuiltinCall{.builtin = Builtin::IntlSupportedLocales};
  call->type = Type::array(Type::string());
}

void LibraryIntlRegistry::checkIntlMember(const CheckIntlMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  member->type = Type::error();
  IntlKind kind = member->object->type->intlKind();
  llvm::StringRef name = member->name;
  auto wanted = this->wantedCallback(task.expected);
  const auto *functionType = this->boundFunctionType(kind, name, wanted);
  if (functionType == nullptr) {
    std::string message = kind == IntlKind::DateTimeFormat
                              ? ErrorTextIntlMember::dateFormat(name)
                              : ErrorTextIntlMember::onlyCalled(name, member->object->type->name());
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushChecked(member);
  }
  member->type = functionType;
  auto *bound = this->state.library.common.builtinCall(member->loc(), member, Builtin::IntlBound,
                                                       {}, functionType);

  return tasks.pushChecked(bound);
}

void LibraryIntlRegistry::checkIntlMethod(const CheckIntlMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  IntlKind kind = member->object->type->intlKind();
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  const auto *found = IntlMethodTable::find(kind, member->name);
  if (found == nullptr) {
    std::string service = member->object->type->name().str();
    auto message = ErrorTextIntlMethod::noMember(member->name, service);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isUnsupported = found->method == IntlMethod::NUMBER_FORMAT_RANGE ||
                       found->method == IntlMethod::PLURAL_SELECT_RANGE;
  auto [min, max] = this->intlMethodArity(found->method);
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (!isUnsupported && arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }

  return this->checkIntlMethodForm(call, member, found->method, kind);
}

void LibraryIntlRegistry::checkIntlMethodForm(CallExpr *call, MemberExpr *member, IntlMethod method,
                                              IntlKind kind) {
  auto &tasks = this->state.walk.tasks;
  auto &intl = this->state.library.intl;
  const auto *parts = Type::array(intl.dateFormatPartType());
  switch (method) {
    case IntlMethod::RESOLVED_OPTIONS:
      return this->toBuiltinCall(call, Builtin::IntlResolvedOptions,
                                 intl.resolvedIntlOptionsType(kind));
    case IntlMethod::NUMBER_FORMAT:
      this->toBuiltinCall(call, Builtin::NfFormat, Type::string());
      return this->pushIntlNumber(&call->args[0], member->name);
    case IntlMethod::NUMBER_FORMAT_TO_PARTS:
      this->toBuiltinCall(call, Builtin::NfFormatToParts, parts);
      return this->pushIntlNumber(&call->args[0], member->name);
    case IntlMethod::NUMBER_FORMAT_RANGE:
      return this->reportUnsupported(call, member, ErrorTextIntlMethod::range(member->name));
    case IntlMethod::COLLATOR_COMPARE:
      this->toBuiltinCall(call, Builtin::CollatorCompare, Type::intTy());
      tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::string()});
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
    case IntlMethod::PLURAL_SELECT:
      this->toBuiltinCall(call, Builtin::PrSelect, Type::string());
      return this->pushIntlNumber(&call->args[0], member->name);
    case IntlMethod::PLURAL_SELECT_RANGE:
      return this->reportUnsupported(call, member, ErrorTextIntlMethod::selectSpan());
    case IntlMethod::RELATIVE_FORMAT:
      this->toBuiltinCall(call, Builtin::RtfFormat, Type::string());
      return this->pushRelativeFormat(call);
    case IntlMethod::RELATIVE_FORMAT_TO_PARTS:
      this->toBuiltinCall(call, Builtin::RtfFormatToParts,
                          Type::array(intl.shapeType(IntlShape::RELATIVE_TIME_FORMAT_PART)));
      return this->pushRelativeFormat(call);
    case IntlMethod::LIST_FORMAT:
      this->toBuiltinCall(call, Builtin::LfFormat, Type::string());
      return tasks.pushTask(
          CheckArgSlot{.slot = &call->args[0], .to = Type::array(Type::string())});
    case IntlMethod::LIST_FORMAT_TO_PARTS:
      this->toBuiltinCall(call, Builtin::LfFormatToParts, parts);
      return tasks.pushTask(
          CheckArgSlot{.slot = &call->args[0], .to = Type::array(Type::string())});
    case IntlMethod::DISPLAY_NAME_OF:
      this->toBuiltinCall(call, Builtin::DnOf, Type::optional(Type::string()));
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
    case IntlMethod::SEGMENTER_SEGMENT:
      this->toBuiltinCall(call, Builtin::SegSegment, Type::intl(IntlKind::Segments));
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
    case IntlMethod::SEGMENTS_CONTAINING:
      this->toBuiltinCall(call, Builtin::SegmentsContaining,
                          Type::optional(intl.segmentDataType()));
      if (call->args.empty()) {
        return;
      }
      return this->pushDoubleNumber(&call->args[0]);
  }
}

std::pair<unsigned, unsigned> LibraryIntlRegistry::intlMethodArity(IntlMethod method) const {
  switch (method) {
    case IntlMethod::RESOLVED_OPTIONS:
      return {0, 0};
    case IntlMethod::COLLATOR_COMPARE:
    case IntlMethod::RELATIVE_FORMAT:
    case IntlMethod::RELATIVE_FORMAT_TO_PARTS:
      return {2, 2};
    case IntlMethod::SEGMENTS_CONTAINING:
      return {0, 1};
    case IntlMethod::NUMBER_FORMAT_RANGE:
    case IntlMethod::PLURAL_SELECT_RANGE:
    case IntlMethod::NUMBER_FORMAT:
    case IntlMethod::NUMBER_FORMAT_TO_PARTS:
    case IntlMethod::PLURAL_SELECT:
    case IntlMethod::LIST_FORMAT:
    case IntlMethod::LIST_FORMAT_TO_PARTS:
    case IntlMethod::DISPLAY_NAME_OF:
    case IntlMethod::SEGMENTER_SEGMENT:
      return {1, 1};
  }

  return {1, 1};
}

void LibraryIntlRegistry::reportUnsupported(CallExpr *call, const MemberExpr *member,
                                            const std::string &message) {
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryIntlRegistry::pushIntlNumber(Expr **slot, llvm::StringRef method) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterIntlNumber{.slot = slot, .method = method});
  tasks.pushTask(ApparentSlot{.slot = slot});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

void LibraryIntlRegistry::afterIntlNumber(const AfterIntlNumber &task) {
  const Expr *arg = *task.slot;
  auto number = this->checkIntlNumberType(arg->type, task.method);
  if (number.isErr()) {
    return this->state.diagnostics.error(arg->loc(), number.error().message);
  }
}

void LibraryIntlRegistry::pushDoubleNumber(Expr **slot) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterDoubleNumber{.slot = slot});

  return tasks.pushTask(NumericArgSlot{.slot = slot, .hint = nullptr});
}

void LibraryIntlRegistry::afterDoubleNumber(const AfterDoubleNumber &task) {
  if (!(*task.slot)->type->isNumeric()) {
    return;
  }

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::doubleTy()});
}

void LibraryIntlRegistry::pushRelativeFormat(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterRelativeUnit{.call = call, .written = call->args[1]});
  tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::string()});

  return this->pushDoubleNumber(&call->args[0]);
}

void LibraryIntlRegistry::afterRelativeUnit(const AfterRelativeUnit &task) {
  CallExpr *call = task.call;
  const Expr *unitArg = call->args[1];
  auto unit = this->literalText(task.written);
  auto units = IntlUnitTable::getRelativeUnits();
  if (unit.has_value()) {
    auto written = ToUtf8::toUtf8(*unit);
    if (!llvm::is_contained(units, written)) {
      auto message = ErrorTextIntlMethod::badUnit(written);
      this->state.diagnostics.error(unitArg->loc(), message);
    }

    return;
  }
  auto texts = this->state.library.common.knownTexts(unitArg);
  auto what = ErrorTextClosedArg::unit();
  auto closed = this->checkClosedArg(unitArg, texts, units, what);
  if (closed.isErr()) {
    return this->state.diagnostics.error(unitArg->loc(), closed.error().message);
  }
}

void LibraryIntlRegistry::checkNumberLocale(const CheckNumberLocale &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(
      FinishLocaleCall{.call = call, .builtin = Builtin::NumToLocale, .type = Type::string()});

  return tasks.pushTask(PrepareIntlArgs{
      .args = &call->args,
      .from = 0,
      .kind = IntlKind::NumberFormat,
      .what = member->name.str(),
  });
}

void LibraryIntlRegistry::finishLocaleCall(const FinishLocaleCall &task) {
  bool isPrepared = this->state.walk.tasks.takeVerdict();
  if (!isPrepared) {
    return;
  }

  return this->toBuiltinCall(task.call, task.builtin, task.type);
}

void LibraryIntlRegistry::checkStringLocale(const CheckStringLocale &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  const auto *method = StringMethodTable::find(member->name);
  bool isNormalize = method != nullptr && method->method == StringMethod::NORMALIZE;
  auto arity =
      this->state.library.common.checkArity(call, isNormalize ? 0 : 1, isNormalize ? 1 : 3);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  if (isNormalize) {
    return this->checkNormalize(call);
  }
  tasks.pushTask(
      FinishLocaleCall{.call = call, .builtin = Builtin::StrLocaleCompare, .type = Type::intTy()});
  tasks.pushTask(PrepareIntlArgs{
      .args = &call->args,
      .from = 1,
      .kind = IntlKind::Collator,
      .what = member->name.str(),
  });

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryIntlRegistry::checkNormalize(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  this->toBuiltinCall(call, Builtin::StrNormalize, Type::string());
  if (call->args.empty()) {
    return;
  }
  tasks.pushTask(AfterNormalizeForm{.call = call, .written = call->args[0]});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryIntlRegistry::afterNormalizeForm(const AfterNormalizeForm &task) {
  const Expr *arg = task.call->args[0];
  auto form = this->literalText(task.written);
  if (form.has_value() && !NormalizationFormTable::isKnown(ToUtf8::toUtf8(*form))) {
    auto message = ErrorTextIntlMethod::badForm(ToUtf8::toUtf8(*form));

    return this->state.diagnostics.error(arg->loc(), message);
  }
  if (form.has_value()) {
    return;
  }
  auto forms = NormalizationFormTable::getForms();
  auto texts = this->state.library.common.knownTexts(arg);
  auto what = ErrorTextClosedArg::form();
  auto closed = this->checkClosedArg(arg, texts, forms, what);
  if (closed.isErr()) {
    return this->state.diagnostics.error(arg->loc(), closed.error().message);
  }
}

void LibraryIntlRegistry::prepareLocaleArgs(const PrepareLocaleArgs &task) {
  std::vector<Expr *> &args = *task.args;
  auto &tasks = this->state.walk.tasks;
  Expr **options = args.size() > 1 ? &args[1] : nullptr;
  tasks.pushTask(DateLocaleOptions{.options = options, .field = task.field});
  if (args.empty()) {
    return tasks.pushVerdict(true);
  }
  auto what = ErrorTextDateOptions::method(task.field);
  tasks.pushTask(LocaleArgSlot{.slot = &args[0], .what = what});

  return tasks.pushTask(CheckExprSlot{.slot = &args[0], .expected = nullptr});
}

void LibraryIntlRegistry::dateLocaleOptions(const DateLocaleOptions &task) {
  auto &tasks = this->state.walk.tasks;
  bool isLocaleOk = tasks.takeVerdict();
  if (task.options == nullptr) {
    return tasks.pushVerdict(isLocaleOk);
  }
  tasks.pushTask(AfterDateOptions{
      .options = task.options,
      .field = task.field,
      .isLocaleOk = isLocaleOk,
  });

  return tasks.pushTask(CheckExprSlot{.slot = task.options, .expected = nullptr});
}

void LibraryIntlRegistry::afterDateOptions(const AfterDateOptions &task) {
  auto &tasks = this->state.walk.tasks;
  const Expr *options = *task.options;
  const Type *type = options->type;
  if (type->isError()) {
    return tasks.pushVerdict(false);
  }
  if (type->isUndefined()) {
    return tasks.pushVerdict(task.isLocaleOk);
  }
  auto object = this->checkOptionsObject(type, task.field);
  if (object.isErr()) {
    this->state.diagnostics.error(options->loc(), object.error().message);

    return tasks.pushVerdict(false);
  }
  bool isKindFitting = this->matchOptionKinds(options, task.field);
  bool isValuesFitting = this->matchOptionValues(options);

  return tasks.pushVerdict(task.isLocaleOk && isKindFitting && isValuesFitting);
}

void LibraryIntlRegistry::checkDateFormatNew(const CheckDateFormatNew &task) {
  return this->startDateFormatNew(task.newExpr->loc(), &task.newExpr->args);
}

void LibraryIntlRegistry::startDateFormatNew(llvm::SMLoc loc, std::vector<Expr *> *args) {
  auto &tasks = this->state.walk.tasks;
  if (args->size() > 2) {
    auto message = ErrorTextDateFormat::arity(args->size());
    this->state.diagnostics.error(loc, message);

    return this->failIntlNew(loc, args);
  }
  tasks.pushTask(FinishDateFormatNew{.loc = loc, .args = args});

  return tasks.pushTask(PrepareLocaleArgs{.args = args, .field = TZ_DATE_LOCALE_FORMAT});
}

void LibraryIntlRegistry::finishDateFormatNew(const FinishDateFormatNew &task) {
  auto &tasks = this->state.walk.tasks;
  bool isPrepared = tasks.takeVerdict();
  if (!isPrepared) {
    return tasks.pushChecked(this->failedValue(task.loc));
  }
  auto intlName = GlobalNameTable::getName(GlobalName::INTL);
  auto *callee = this->state.ast.make<NameExpr>(task.loc, intlName);
  auto *made = this->state.library.common.builtinCall(task.loc, callee, Builtin::DtfNew, *task.args,
                                                      Type::dateFormat());

  return tasks.pushChecked(made);
}

void LibraryIntlRegistry::checkDateFormatMethod(const CheckDateFormatMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto method = DateFormatMethodTable::find(name);
  bool isFormat = method == DateFormatMethod::FORMAT;
  bool isFormatting = isFormat || method == DateFormatMethod::FORMAT_TO_PARTS;
  bool isResolved = method == DateFormatMethod::RESOLVED_OPTIONS;
  if (!isFormatting && !isResolved) {
    bool isRange = method == DateFormatMethod::FORMAT_RANGE ||
                   method == DateFormatMethod::FORMAT_RANGE_TO_PARTS;
    std::string message =
        isRange ? ErrorTextDateFormat::range(name) : ErrorTextDateFormat::noMember(name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, isFormatting ? 1 : 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  if (isResolved) {
    return this->toBuiltinCall(call, Builtin::DtfResolvedOptions,
                               this->state.library.intl.resolvedDateOptionsType());
  }
  if (call->args.empty()) {
    return this->finishDateFormatCall(call, isFormat);
  }
  tasks.pushTask(AfterDateFormatArg{.call = call, .member = member, .isFormat = isFormat});
  tasks.pushTask(ApparentSlot{.slot = &call->args[0]});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryIntlRegistry::afterDateFormatArg(const AfterDateFormatArg &task) {
  CallExpr *call = task.call;
  const Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (!type->isNumeric() && !type->isDate() && !type->isError()) {
    auto message = ErrorTextDateFormat::formatArg(task.member->name, type->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  this->finishDateFormatCall(call, task.isFormat);
  if (!type->isNumeric()) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &call->args[0], .to = Type::doubleTy()});
}

void LibraryIntlRegistry::finishDateFormatCall(CallExpr *call, bool isFormat) {
  const auto *parts = Type::array(this->state.library.intl.dateFormatPartType());
  Builtin builtin = isFormat ? Builtin::DtfFormat : Builtin::DtfFormatToParts;

  return this->toBuiltinCall(call, builtin, isFormat ? Type::string() : parts);
}

void LibraryIntlRegistry::toBuiltinCall(CallExpr *call, Builtin builtin, const Type *type) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;
}

Expr *LibraryIntlRegistry::kindArg(llvm::SMLoc loc, int kind) {
  auto *literal = this->state.ast.make<IntLitExpr>(loc, uint64_t(kind));
  literal->type = Type::intTy();

  return literal;
}

LibraryIntlRegistry::ResultIntlCreation
LibraryIntlRegistry::checkIntlCreation(const IntlService *service, llvm::StringRef name,
                                       size_t count, IntlCreation creation) const {
  if (service == nullptr) {
    auto message = ErrorTextIntlNew::unknown(name);

    return err(LibraryIntlRegistryIntlCreationError{
        .code = LibraryIntlRegistryIntlCreationErrorCode::UNKNOWN_SERVICE,
        .message = message,
    });
  }
  if (creation == IntlCreation::CALL && !service->callable) {
    auto message = ErrorTextIntlNew::withNew(name);

    return err(LibraryIntlRegistryIntlCreationError{
        .code = LibraryIntlRegistryIntlCreationErrorCode::NEEDS_NEW,
        .message = message,
    });
  }
  if (count > 2) {
    auto message = ErrorTextDateFormat::arity(count);

    return err(LibraryIntlRegistryIntlCreationError{
        .code = LibraryIntlRegistryIntlCreationErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }
  if (service->kind == IntlKind::DisplayNames && count < 2) {
    auto message = ErrorTextIntlNew::noType();

    return err(LibraryIntlRegistryIntlCreationError{
        .code = LibraryIntlRegistryIntlCreationErrorCode::NO_DISPLAY_TYPE,
        .message = message,
    });
  }

  return ok(service);
}

LibraryIntlRegistry::ResultLocalePresent
LibraryIntlRegistry::checkLocalePresent(size_t count) const {
  if (!this->hasLocaleClass()) {
    auto message = ErrorTextIntlLocale::missing();

    return err(LibraryIntlRegistryLocalePresentError{
        .code = LibraryIntlRegistryLocalePresentErrorCode::NO_LOCALE_CLASS,
        .message = message,
    });
  }
  if (count == 2) {
    auto message = ErrorTextIntlLocale::options();

    return err(LibraryIntlRegistryLocalePresentError{
        .code = LibraryIntlRegistryLocalePresentErrorCode::OPTIONS_GIVEN,
        .message = message,
    });
  }

  return ok(count);
}

std::optional<std::u16string> LibraryIntlRegistry::literalText(const Expr *expr) const {
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

const Expr *LibraryIntlRegistry::unconverted(const Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

bool LibraryIntlRegistry::isStringLike(const Type *type) const {
  auto isStringLiteral = std::bind_front(&LibraryIntlRegistry::isStringLiteralMember, this);

  return type->isString() || type->isStringLiteral() ||
         (type->isUnion() && llvm::all_of(type->members(), isStringLiteral));
}

bool LibraryIntlRegistry::isStringLiteralMember(const Type *member) const {
  return member->isStringLiteral();
}

LibraryIntlRegistry::ResultLanguageTags
LibraryIntlRegistry::checkLanguageTags(const Expr *arg, llvm::StringRef what, LanguageTagUse use) {
  auto tags = this->state.library.common.knownTexts(arg);
  if (!tags.has_value()) {
    bool isConstructor = use == LanguageTagUse::LOCALE_CONSTRUCTOR;
    auto message = ErrorTextLanguageTag::unknown(isConstructor, what);

    return err(LibraryIntlRegistryLanguageTagsError{
        .code = LibraryIntlRegistryLanguageTagsErrorCode::UNKNOWN_TAGS,
        .message = message,
    });
  }
  for (const std::string &tag : *tags) {
    if (!this->isLanguageTag(tag)) {
      auto message = ErrorTextLanguageTag::notTag(tag);

      return err(LibraryIntlRegistryLanguageTagsError{
          .code = LibraryIntlRegistryLanguageTagsErrorCode::NOT_A_TAG,
          .message = message,
      });
    }
  }

  return ok(*tags);
}

void LibraryIntlRegistry::walkLocaleElements(const ArrayLitExpr *list, llvm::StringRef what) {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      auto message = ErrorTextLanguageTag::spread(what);
      this->state.diagnostics.error(element.expr->loc(), message);
      return;
    }
    auto tags = this->checkLanguageTags(element.expr, what, LanguageTagUse::LOCALE_ARGUMENT);
    if (tags.isErr()) {
      this->state.diagnostics.error(element.expr->loc(), tags.error().message);
      return;
    }
  }
}

std::vector<Expr *> *LibraryIntlRegistry::constructedArgs(NewExpr *made) const {
  if (made->ctor() != nullptr) {
    return &made->ctor()->args;
  }

  return &made->args;
}

LibraryIntlRegistry::WantedCallback
LibraryIntlRegistry::wantedCallback(const Type *expected) const {
  const auto *callback = this->state.walk.functions.getExpectedCallback();
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

const Type *LibraryIntlRegistry::boundFunctionType(IntlKind kind, llvm::StringRef name,
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

LibraryIntlRegistry::ResultIntlNumber
LibraryIntlRegistry::checkIntlNumberType(const Type *type, llvm::StringRef method) const {
  if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
    auto message = ErrorTextIntlNumber::notNumber(method, type);

    return err(LibraryIntlRegistryIntlNumberError{
        .code = LibraryIntlRegistryIntlNumberErrorCode::NOT_A_NUMBER,
        .message = message,
    });
  }

  return ok(type);
}

LibraryIntlRegistry::ResultClosedText LibraryIntlRegistry::checkClosedArg(
    const Expr *arg, const std::optional<std::vector<std::string>> &texts,
    llvm::ArrayRef<llvm::StringLiteral> allowed, llvm::StringRef what) const {
  if (arg->type == nullptr || arg->type->isError()) {
    return ok(arg);
  }
  if (!texts) {
    auto message = ErrorTextClosedArg::unknown(what, allowed[0], allowed[1]);

    return err(LibraryIntlRegistryClosedArgError{
        .code = LibraryIntlRegistryClosedArgErrorCode::UNKNOWN_TEXT,
        .message = message,
    });
  }
  for (const std::string &text : *texts) {
    if (!llvm::is_contained(allowed, text)) {
      auto message = ErrorTextClosedArg::notAllowed(text, what);

      return err(LibraryIntlRegistryClosedArgError{
          .code = LibraryIntlRegistryClosedArgErrorCode::NOT_ALLOWED,
          .message = message,
      });
    }
  }

  return ok(arg);
}

std::optional<double> LibraryIntlRegistry::literalNumber(const Expr *expr) const {
  bool isNegative = false;
  const Expr *inner = expr;
  auto *negation = dyn_cast<UnaryExpr>(inner);
  while (negation != nullptr && negation->op == UnaryOp::Neg) {
    isNegative = !isNegative;
    inner = negation->operand;
    negation = dyn_cast<UnaryExpr>(inner);
  }
  auto value = this->plainNumber(inner);
  if (!value.has_value() || !isNegative) {
    return value;
  }

  return -*value;
}

std::optional<double> LibraryIntlRegistry::plainNumber(const Expr *expr) const {
  const auto *integer = dyn_cast<IntLitExpr>(expr);
  if (integer != nullptr) {
    return double(integer->value);
  }
  const auto *fraction = dyn_cast<FloatLitExpr>(expr);
  if (fraction != nullptr) {
    return fraction->value;
  }

  return std::nullopt;
}

bool LibraryIntlRegistry::isKeywordType(llvm::StringRef text) const {
  auto parts = llvm::SmallVector<llvm::StringRef, 4>();
  text.split(parts, '-');
  auto isKeyword = std::bind_front(&LibraryIntlRegistry::isKeywordPart, this);

  return llvm::all_of(parts, isKeyword);
}

bool LibraryIntlRegistry::isKeywordPart(llvm::StringRef part) const {
  return part.size() >= 3 && part.size() <= 8 && llvm::all_of(part, llvm::isAlnum);
}

std::optional<std::string> LibraryIntlRegistry::knownText(const Expr *value,
                                                          const Type *type) const {
  if (std::optional<std::u16string> text =
          value != nullptr ? this->literalText(value) : std::nullopt) {
    return ToUtf8::toUtf8(*text);
  }
  if (type != nullptr && type->isStringLiteral()) {
    return ToUtf8::toUtf8(type->literalText());
  }

  return std::nullopt;
}

std::vector<std::string> LibraryIntlRegistry::literalTexts(const Type *type, bool &open) const {
  auto texts = std::vector<std::string>();
  type = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  for (const Type *member : members) {
    if (member->isStringLiteral()) {
      texts.push_back(ToUtf8::toUtf8(member->literalText()));
    } else if (!member->isBool() && !(member->isLiteral() && member->literalBase()->isBool())) {
      open = true;
    }
  }

  return texts;
}

LibraryIntlRegistry::ResultOptionsGiven
LibraryIntlRegistry::checkOptionsGiven(const Type *type, IntlKind kind,
                                       llvm::StringRef what) const {
  if (type->isUndefined() && kind == IntlKind::DisplayNames) {
    auto message = ErrorTextIntlOptions::noType();

    return err(LibraryIntlRegistryOptionsGivenError{
        .code = LibraryIntlRegistryOptionsGivenErrorCode::NO_DISPLAY_TYPE,
        .message = message,
    });
  }
  bool isObject =
      type->isUndefined() || (type->isObject() && !type->isTuple() && !type->isClass() &&
                              !type->isRegExp() && !type->isMatch() && !type->isDate() &&
                              !type->isIntl() && !type->isCrypto() && !type->isBytes());
  if (!isObject) {
    auto message = ErrorTextIntlOptions::notObject(what, type->name());

    return err(LibraryIntlRegistryOptionsGivenError{
        .code = LibraryIntlRegistryOptionsGivenErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

Expr *LibraryIntlRegistry::valueOf(const IntlOptionsRun &run, llvm::StringRef name) const {
  if (run.literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : run.literal->props) {
    if (!property.spread() && property.name == name) {
      return property.value;
    }
  }

  return nullptr;
}

const Field *LibraryIntlRegistry::fieldOf(const IntlOptionsRun &run, IntlOptionKey key) const {
  auto name = IntlOptionKeyTable::getName(key);

  return run.type->field(name);
}

std::optional<std::string> LibraryIntlRegistry::textOf(const IntlOptionsRun &run,
                                                       IntlOptionKey key) const {
  const auto *field = this->fieldOf(run, key);
  if (field == nullptr) {
    return std::nullopt;
  }
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));

  return this->knownText(value, field->type->nonOptional());
}

std::optional<double> LibraryIntlRegistry::constantOf(const IntlOptionsRun &run,
                                                      IntlOptionKey key) const {
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));
  std::optional<double> literal = value != nullptr ? this->literalNumber(value) : std::nullopt;
  if (literal.has_value()) {
    return literal;
  }
  const auto *field = this->fieldOf(run, key);
  const Type *type = field != nullptr ? field->type->nonOptional() : nullptr;
  if (type != nullptr && type->isLiteral() && type->literalBase()->isNumeric()) {
    return type->literalNumber();
  }

  return std::nullopt;
}

const Expr *LibraryIntlRegistry::valueExpr(const IntlOptionsRun &run, IntlOptionKey key) const {
  auto *value = this->valueOf(run, IntlOptionKeyTable::getName(key));

  return value != nullptr ? this->unconverted(value) : nullptr;
}

void LibraryIntlRegistry::reportOption(IntlOptionsRun &run, IntlOptionKey key,
                                       const std::string &message) {
  auto name = IntlOptionKeyTable::getName(key);

  return this->reportOptionNamed(run, name, message);
}

void LibraryIntlRegistry::reportOptionNamed(IntlOptionsRun &run, llvm::StringRef name,
                                            const std::string &message) {
  auto *value = this->valueOf(run, name);
  llvm::SMLoc at = value != nullptr ? value->loc() : run.options->loc();
  this->state.diagnostics.error(at, message);
  run.ok = false;
}

const IntlOption *LibraryIntlRegistry::findOption(const IntlService *service,
                                                  llvm::StringRef name) const {
  auto key = IntlOptionKeyTable::find(name);
  if (!key.has_value()) {
    return nullptr;
  }
  for (const IntlOption &option : service->options) {
    if (option.key == *key) {
      return &option;
    }
  }

  return nullptr;
}

void LibraryIntlRegistry::walkOptionField(IntlOptionsRun &run, const Field &field) {
  const Type *value = field.type->nonOptional();
  const auto *found = this->findOption(run.service, field.name);
  if (found == nullptr) {
    auto message = ErrorTextIntlField::unknown(field.name, run.what, run.service);
    this->reportOptionNamed(run, field.name, message);
    return;
  }
  if (value->isUndefined()) {
    return;
  }
  if (!this->isOptionKindFitting(found->kind, value)) {
    auto message = ErrorTextIntlField::kind(field.name, found->kind, field.type->name());
    this->reportOption(run, found->key, message);
    return;
  }
  auto text = this->textOf(run, found->key);
  bool isGrouping = found->kind == IntlOptionKind::GROUPING;
  bool isBadText = text.has_value() && !found->values.empty() &&
                   !llvm::is_contained(found->values, *text) && !(isGrouping && text->empty());
  if (isBadText) {
    auto message = ErrorTextIntlField::badValue(*text, field.name, found->values);
    this->reportOption(run, found->key, message);
    return;
  }
  bool isListed = found->kind != IntlOptionKind::NUMBER && found->kind != IntlOptionKind::FLAG;
  if (!text.has_value() && !found->values.empty() && isListed) {
    this->walkOptionValues(run, field, *found, value);
  }
}

bool LibraryIntlRegistry::isOptionKindFitting(IntlOptionKind kind, const Type *value) const {
  switch (kind) {
    case IntlOptionKind::TEXT:
      return this->isStringLike(value);
    case IntlOptionKind::NUMBER:
      return this->isNumberLike(value) ||
             (value->isUnion() && this->areAllNumberLike(value->members()));
    case IntlOptionKind::FLAG:
      return this->isBooleanLike(value);
    case IntlOptionKind::GROUPING:
      break;
  }

  return this->isStringLike(value) || this->isBooleanLike(value) ||
         (value->isUnion() && this->areAllTextOrBoolean(value->members()));
}

bool LibraryIntlRegistry::isBooleanLike(const Type *type) const {
  return type->isBool() || (type->isLiteral() && type->literalBase()->isBool());
}

bool LibraryIntlRegistry::isNumberLike(const Type *type) const {
  return type->isNumeric() || (type->isLiteral() && type->literalBase()->isNumeric());
}

bool LibraryIntlRegistry::areAllNumberLike(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    if (!this->isNumberLike(member)) {
      return false;
    }
  }

  return true;
}

bool LibraryIntlRegistry::areAllTextOrBoolean(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    if (!this->isStringLike(member) && !this->isBooleanLike(member)) {
      return false;
    }
  }

  return true;
}

void LibraryIntlRegistry::walkOptionValues(IntlOptionsRun &run, const Field &field,
                                           const IntlOption &found, const Type *value) {
  bool isOpen = false;
  auto texts = this->literalTexts(value, isOpen);
  auto *given = this->valueOf(run, field.name);
  std::optional<std::vector<std::string>> known =
      given != nullptr ? this->state.library.common.knownTexts(given) : std::nullopt;
  if (known.has_value()) {
    texts = *known;
    isOpen = false;
  }
  const auto *wrong = this->wrongText(texts, found);
  if (wrong != nullptr) {
    auto message = ErrorTextIntlField::badValue(*wrong, field.name, found.values);
    this->reportOption(run, found.key, message);
  } else if (isOpen) {
    auto message = ErrorTextIntlField::open(field.name, found.values);
    this->reportOption(run, found.key, message);
  }
}

const std::string *LibraryIntlRegistry::wrongText(const std::vector<std::string> &texts,
                                                  const IntlOption &found) const {
  bool isGrouping = found.kind == IntlOptionKind::GROUPING;
  for (const std::string &text : texts) {
    if (!llvm::is_contained(found.values, text) && !(isGrouping && text.empty())) {
      return &text;
    }
  }

  return nullptr;
}

void LibraryIntlRegistry::walkOptionTexts(IntlOptionsRun &run) {
  auto code = this->textOf(run, IntlOptionKey::CURRENCY);
  if (code.has_value() && !(code->size() == 3 && llvm::all_of(*code, llvm::isAlpha))) {
    auto message = ErrorTextIntlField::currency(*code);
    this->reportOption(run, IntlOptionKey::CURRENCY, message);
  }
  auto unit = this->textOf(run, IntlOptionKey::UNIT);
  if (unit.has_value() && !IntlUnitTable::isWellFormed(*unit)) {
    auto message = ErrorTextIntlField::unit(*unit);
    this->reportOption(run, IntlOptionKey::UNIT, message);
  }
  for (IntlOptionKey key : {IntlOptionKey::NUMBERING_SYSTEM, IntlOptionKey::COLLATION}) {
    auto value = this->textOf(run, key);
    if (value.has_value() && !this->isKeywordType(*value)) {
      auto name = IntlOptionKeyTable::getName(key);
      auto message = ErrorTextIntlField::keyword(*value, name);
      this->reportOption(run, key, message);
    }
  }
  this->walkStyleOption(run);
  if (run.kind == IntlKind::DisplayNames && this->fieldOf(run, IntlOptionKey::TYPE) == nullptr) {
    auto message = ErrorTextIntlOptions::noType();
    this->reportOption(run, IntlOptionKey::TYPE, message);
  }
}

void LibraryIntlRegistry::walkStyleOption(IntlOptionsRun &run) {
  const auto *style = this->fieldOf(run, IntlOptionKey::STYLE);
  if (style == nullptr || run.kind != IntlKind::NumberFormat) {
    return;
  }
  bool isOpen = false;
  auto styles = this->literalTexts(style->type, isOpen);
  auto text = this->textOf(run, IntlOptionKey::STYLE);
  if (text.has_value()) {
    styles = {*text};
  }
  auto currencyStyle = IntlOptionKeyTable::getName(IntlOptionKey::CURRENCY);
  auto unitStyle = IntlOptionKeyTable::getName(IntlOptionKey::UNIT);
  bool hasCurrency = this->fieldOf(run, IntlOptionKey::CURRENCY) != nullptr;
  bool hasUnit = this->fieldOf(run, IntlOptionKey::UNIT) != nullptr;
  if (llvm::is_contained(styles, currencyStyle) && !hasCurrency) {
    auto message = ErrorTextIntlField::styleCur();
    this->reportOption(run, IntlOptionKey::STYLE, message);
  }
  if (llvm::is_contained(styles, unitStyle) && !hasUnit) {
    auto message = ErrorTextIntlField::styleUnit();
    this->reportOption(run, IntlOptionKey::STYLE, message);
  }
}

void LibraryIntlRegistry::walkDigitRanges(IntlOptionsRun &run) {
  for (IntlOptionKey key :
       {IntlOptionKey::MINIMUM_INTEGER_DIGITS, IntlOptionKey::MINIMUM_FRACTION_DIGITS,
        IntlOptionKey::MAXIMUM_FRACTION_DIGITS, IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS,
        IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS}) {
    auto range = IntlServiceTable::findDigitRange(key);
    this->walkDigitRange(run, *range);
  }
}

void LibraryIntlRegistry::walkDigitRange(IntlOptionsRun &run, const IntlDigitRange &range) {
  const auto *field = this->fieldOf(run, range.key);
  if (field == nullptr) {
    return;
  }
  auto name = IntlOptionKeyTable::getName(range.key);
  int min = int(range.low);
  int max = int(range.high);
  auto constant = this->constantOf(run, range.key);
  if (constant.has_value() && !(*constant >= min && *constant <= max)) {
    auto message = ErrorTextIntlField::range(name, min, max);
    this->reportOption(run, range.key, message);
  }
  if (constant.has_value()) {
    return;
  }
  auto held = this->state.types.literals.intRange(field->type->nonOptional());
  if (held.has_value() && held->first >= min && held->second <= max) {
    return;
  }
  const auto *value = this->valueExpr(run, range.key);
  if (value != nullptr && value->type->isInt() &&
      this->state.flow.proofs.canProveFits(value, min, max)) {
    return;
  }
  auto message = ErrorTextIntlField::rangeOpen(name, min, max);
  this->reportOption(run, range.key, message);
}

bool LibraryIntlRegistry::isNotAbove(const IntlOptionsRun &run, IntlOptionKey low,
                                     IntlOptionKey high) {
  auto lowConstant = this->constantOf(run, low);
  auto highConstant = this->constantOf(run, high);
  if (lowConstant.has_value() && highConstant.has_value()) {
    return std::floor(*lowConstant) <= std::floor(*highConstant);
  }
  auto lowLinear = this->optionLinear(run, low, lowConstant);
  auto highLinear = this->optionLinear(run, high, highConstant);
  if (!lowLinear.has_value() || !highLinear.has_value()) {
    return false;
  }
  if (lowLinear->term == highLinear->term) {
    return lowLinear->offset <= highLinear->offset;
  }
  int64_t gap = highLinear->offset - lowLinear->offset + 1;

  return this->state.walk.flow.getFacts().canProve(lowLinear->term, highLinear->term, gap);
}

std::optional<Linear> LibraryIntlRegistry::optionLinear(const IntlOptionsRun &run,
                                                        IntlOptionKey key,
                                                        std::optional<double> constant) {
  const auto *value = this->valueExpr(run, key);
  std::optional<Linear> linear =
      value != nullptr ? this->state.flow.measurements.linear(value) : std::nullopt;
  if (linear.has_value()) {
    return linear;
  }
  if (constant.has_value() && *constant == std::floor(*constant)) {
    return Linear{Term::zero(), int64_t(*constant)};
  }

  return std::nullopt;
}

void LibraryIntlRegistry::walkDigitPairs(IntlOptionsRun &run) {
  static const std::pair<IntlOptionKey, IntlOptionKey> pairs[] = {
      {IntlOptionKey::MINIMUM_FRACTION_DIGITS, IntlOptionKey::MAXIMUM_FRACTION_DIGITS},
      {IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS, IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS}};
  for (const auto &[min, max] : pairs) {
    bool isSkipped = run.literal == nullptr || this->fieldOf(run, min) == nullptr ||
                     this->fieldOf(run, max) == nullptr || this->isNotAbove(run, min, max);
    if (!isSkipped) {
      auto low = this->constantOf(run, min);
      auto high = this->constantOf(run, max);
      auto minName = IntlOptionKeyTable::getName(min);
      auto maxName = IntlOptionKeyTable::getName(max);
      auto message =
          ErrorTextIntlField::above(minName, maxName, low.has_value() && high.has_value());
      this->reportOption(run, max, message);
    }
  }
}

bool LibraryIntlRegistry::isIncrement(double value) const {
  return llvm::is_contained(IntlServiceTable::getRoundingIncrements(), value);
}

bool LibraryIntlRegistry::areAllIncrements(const Type *type) const {
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral() || !this->isIncrement(member->literalNumber())) {
      return false;
    }
  }

  return true;
}

void LibraryIntlRegistry::walkRoundingIncrement(IntlOptionsRun &run) {
  const auto *increment = this->fieldOf(run, IntlOptionKey::ROUNDING_INCREMENT);
  if (increment == nullptr) {
    return;
  }
  auto constant = this->constantOf(run, IntlOptionKey::ROUNDING_INCREMENT);
  bool isKnown = constant.has_value() || this->areAllIncrements(increment->type->nonOptional());
  bool isListed = !constant.has_value() || this->isIncrement(*constant);
  if (run.literal != nullptr && isKnown && constant != 1.0 && isListed) {
    this->walkIncrementDigits(run);
  }
  if (!isKnown || !isListed) {
    auto message = ErrorTextIntlField::increments(constant.has_value());
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
  }
}

void LibraryIntlRegistry::walkIncrementDigits(IntlOptionsRun &run) {
  bool isSignificant = this->fieldOf(run, IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS) != nullptr ||
                       this->fieldOf(run, IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS) != nullptr;
  if (isSignificant) {
    auto message = ErrorTextIntlField::incSig();
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
    return;
  }
  IntlOptionKey low = IntlOptionKey::MINIMUM_FRACTION_DIGITS;
  IntlOptionKey high = IntlOptionKey::MAXIMUM_FRACTION_DIGITS;
  bool isSameDigits = this->fieldOf(run, low) != nullptr && this->fieldOf(run, high) != nullptr &&
                      this->isNotAbove(run, low, high) && this->isNotAbove(run, high, low);
  if (!isSameDigits) {
    auto message = ErrorTextIntlField::incSame();
    this->reportOption(run, IntlOptionKey::ROUNDING_INCREMENT, message);
  }
}

std::optional<std::vector<std::string>> LibraryIntlRegistry::textsOf(const Type *type) const {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  auto texts = std::vector<std::string>();
  for (const Type *member : members) {
    if (!member->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(member->literalText()));
  }

  return texts;
}

std::optional<std::vector<double>> LibraryIntlRegistry::numbersOf(const Type *type) const {
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  auto numbers = std::vector<double>();
  for (const Type *member : members) {
    if (!member->isLiteral() || !member->literalBase()->isNumeric()) {
      return std::nullopt;
    }
    numbers.push_back(member->literalNumber());
  }

  return numbers;
}

bool LibraryIntlRegistry::isLanguageTag(const std::string &tag) const {
  auto parts = llvm::SmallVector<llvm::StringRef, 4>();
  llvm::StringRef(tag).split(parts, '-');
  for (llvm::StringRef part : parts) {
    if (part.empty() || part.size() > 8 || !llvm::all_of(part, llvm::isAlnum)) {
      return false;
    }
  }
  llvm::StringRef language = parts[0];
  bool isLength = (language.size() >= 2 && language.size() <= 3) ||
                  (language.size() >= 5 && language.size() <= 8);

  return isLength && llvm::all_of(language, llvm::isAlpha);
}

LibraryIntlRegistry::ResultOptionsObject LibraryIntlRegistry::checkOptionsObject(const Type *type,
                                                                                 int kind) const {
  if (!type->isObject() || type->isTuple() || type->isClass() || type->isRegExp() ||
      type->isMatch() || type->isDate() || type->isIntl() || type->isCrypto() || type->isBytes()) {
    auto message =
        ErrorTextDateOptions::notObject(ErrorTextDateOptions::method(kind), type->name());

    return err(LibraryIntlRegistryOptionsObjectError{
        .code = LibraryIntlRegistryOptionsObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

bool LibraryIntlRegistry::matchOptionKinds(const Expr *options, int kind) {
  bool isOk = true;
  auto roles = OptionRoles{.hasStyle = false, .hasComponent = false};
  for (const Field &field : options->type->fields()) {
    auto option = DateFormatOptionTable::find(field.name);
    bool isFitting = this->matchOptionKind(options, field, option, kind);
    isOk &= isFitting;
    if (isFitting) {
      isOk &= this->matchOptionRole(options, field, option.value(), kind, roles);
    }
  }
  if (roles.hasStyle && roles.hasComponent) {
    auto message = ErrorTextDateOptions::styleParts();
    this->state.diagnostics.error(options->loc(), message);
    isOk = false;
  }

  return isOk;
}

bool LibraryIntlRegistry::matchOptionRole(const Expr *options, const Field &field,
                                          DateFormatOption option, int kind, OptionRoles &roles) {
  bool isPresent = !field.type->isOptional();
  auto role = this->optionRole(option);
  if (role == DateOptionRole::Component) {
    roles.hasComponent |= isPresent;
    return true;
  }
  if (role != DateOptionRole::Style) {
    return true;
  }
  roles.hasStyle |= isPresent;

  return !isPresent || this->matchStyleMethod(options, field.name, kind);
}

bool LibraryIntlRegistry::matchOptionKind(const Expr *options, const Field &field,
                                          std::optional<DateFormatOption> option, int kind) {
  if (!option.has_value()) {
    auto message = ErrorTextDateOptions::unknown(field.name, ErrorTextDateOptions::method(kind));
    this->state.diagnostics.error(options->loc(), message);
    return false;
  }
  const Type *value = field.type->nonOptional();
  const auto *wanted = this->wantedOptionType(*option, value);
  if (value == wanted) {
    return true;
  }
  auto message = ErrorTextDateOptions::kind(field.name, wanted == Type::boolTy(),
                                            wanted->isNumeric(), field.type->name());
  this->state.diagnostics.error(options->loc(), message);

  return false;
}

const Type *LibraryIntlRegistry::wantedOptionType(DateFormatOption option,
                                                  const Type *value) const {
  if (option == DateFormatOption::HOUR12) {
    return Type::boolTy();
  }
  if (option == DateFormatOption::FRACTIONAL_SECOND_DIGITS) {
    bool isNumeric = value->isNumeric() || this->numbersOf(value).has_value();
    return isNumeric ? value : Type::intTy();
  }
  if (this->textsOf(value).has_value()) {
    return value;
  }

  return Type::string();
}

LibraryIntlRegistry::DateOptionRole LibraryIntlRegistry::optionRole(DateFormatOption option) const {
  switch (option) {
    case DateFormatOption::DATE_STYLE:
    case DateFormatOption::TIME_STYLE:
      return DateOptionRole::Style;
    case DateFormatOption::WEEKDAY:
    case DateFormatOption::ERA:
    case DateFormatOption::YEAR:
    case DateFormatOption::MONTH:
    case DateFormatOption::DAY:
    case DateFormatOption::DAY_PERIOD:
    case DateFormatOption::HOUR:
    case DateFormatOption::MINUTE:
    case DateFormatOption::SECOND:
    case DateFormatOption::TIME_ZONE_NAME:
    case DateFormatOption::FRACTIONAL_SECOND_DIGITS:
      return DateOptionRole::Component;
    case DateFormatOption::LOCALE_MATCHER:
    case DateFormatOption::CALENDAR:
    case DateFormatOption::NUMBERING_SYSTEM:
    case DateFormatOption::HOUR_CYCLE:
    case DateFormatOption::TIME_ZONE:
    case DateFormatOption::FORMAT_MATCHER:
    case DateFormatOption::HOUR12:
      return DateOptionRole::Setting;
  }

  return DateOptionRole::Setting;
}

bool LibraryIntlRegistry::matchStyleMethod(const Expr *options, llvm::StringRef name, int kind) {
  auto option = DateFormatOptionTable::find(name);
  bool isForeign = (kind == TZ_DATE_LOCALE_DATE && option == DateFormatOption::TIME_STYLE) ||
                   (kind == TZ_DATE_LOCALE_TIME && option == DateFormatOption::DATE_STYLE);
  if (!isForeign) {
    return true;
  }
  auto message = ErrorTextDateOptions::styleOnly(name, ErrorTextDateOptions::method(kind));
  this->state.diagnostics.error(options->loc(), message);

  return false;
}

bool LibraryIntlRegistry::matchOptionValues(const Expr *options) {
  const auto *literal = dyn_cast<ObjectLitExpr>(options);
  bool isExempt = this->state.flow.proofs.isExempt(options->loc());
  bool isOk = true;
  for (const Field &field : options->type->fields()) {
    const auto *given = this->givenProperty(literal, field.name);
    DateOptionPlace place{
        .given = given,
        .at = given != nullptr ? given->loc() : options->loc(),
        .isExempt = isExempt,
    };
    auto option = DateFormatOptionTable::find(field.name);
    bool hasDigits = option == DateFormatOption::FRACTIONAL_SECOND_DIGITS;
    bool isFitting =
        hasDigits ? this->matchDigits(field, place) : this->matchOptionText(field, place);
    isOk &= isFitting;
  }

  return isOk;
}

const Expr *LibraryIntlRegistry::givenProperty(const ObjectLitExpr *literal,
                                               llvm::StringRef name) const {
  if (literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && property.name == name && property.value != nullptr) {
      return property.value;
    }
  }

  return nullptr;
}

bool LibraryIntlRegistry::matchDigits(const Field &field, const DateOptionPlace &place) {
  if (place.given != nullptr) {
    auto digits = ErrorTextDateOptions::digitsName();
    auto between =
        this->state.flow.proofs.requireBetween(place.given, kFewestDigits, kMostDigits, digits);
    if (between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT) {
      this->state.diagnostics.error(place.given->loc(), between.error().message);
    }
    return between.isOk();
  }
  auto digits = this->numbersOf(field.type->nonOptional());
  auto isOutside = std::bind_front(&LibraryIntlRegistry::isOutsideDigits, this);
  bool isAnyOutside = !digits.has_value() || llvm::any_of(*digits, isOutside);
  if (place.isExempt || !isAnyOutside) {
    return true;
  }
  auto message = ErrorTextDateOptions::digits();
  this->state.diagnostics.error(place.at, message);

  return false;
}

bool LibraryIntlRegistry::isOutsideDigits(double digits) const {
  return digits < kFewestDigits || digits > kMostDigits;
}

bool LibraryIntlRegistry::matchOptionText(const Field &field, const DateOptionPlace &place) {
  auto values = DateFormatOptionTable::getValues(field.name);
  if (values.empty() || place.isExempt) {
    return true;
  }
  auto texts = this->optionTexts(field, place);
  if (!texts.has_value()) {
    std::string written =
        place.given != nullptr ? BytesCheckHelpers::valueText(place.given) : std::string();
    auto message = ErrorTextDateOptions::unknownVal(field.name, written, values[0], values[1]);
    this->state.diagnostics.error(place.at, message);
    return false;
  }
  for (const std::string &text : *texts) {
    if (!llvm::is_contained(values, text)) {
      auto message = ErrorTextDateOptions::badValue(text, field.name, values);
      this->state.diagnostics.error(place.at, message);
      return false;
    }
  }

  return true;
}

std::optional<std::vector<std::string>>
LibraryIntlRegistry::optionTexts(const Field &field, const DateOptionPlace &place) {
  if (place.given != nullptr) {
    auto known = this->state.library.common.knownTexts(place.given);
    if (known.has_value()) {
      return known;
    }
  }

  return this->textsOf(field.type->nonOptional());
}

}
