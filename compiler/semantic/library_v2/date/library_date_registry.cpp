#include "compiler/semantic/library_v2/date/library_date_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/date/date_legacy_method_table/date_legacy_method_table.h"
#include "compiler/program/library/date/date_static_table/date_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "runtime/library/date/date/date.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr size_t kMostDateArgs = 7;

}

LibraryDateRegistry::LibraryDateRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryDateRegistry::dateToTime() {
  auto &tasks = this->state.walk.tasks;
  Expr *date = tasks.takeChecked();
  auto *time = this->dateTime(date);

  return tasks.pushChecked(time);
}

void LibraryDateRegistry::dateToString() {
  auto &tasks = this->state.walk.tasks;
  Expr *date = tasks.takeChecked();
  auto *text = this->dateText(date);

  return tasks.pushChecked(text);
}

Expr *LibraryDateRegistry::dateTime(Expr *date) {
  auto getTime = DateMethodTable::getName(Builtin::DateGetTime, 0);
  auto *callee = this->state.ast.make<MemberExpr>(date->loc(), date, getTime);

  return this->state.library.common.builtinCall(date->loc(), callee, Builtin::DateGetTime, {},
                                                Type::intTy());
}

Expr *LibraryDateRegistry::dateText(Expr *date) {
  auto toString = DateMethodTable::getName(Builtin::DateToLocalString, TZ_DATE_TEXT_FULL);
  auto *callee = this->state.ast.make<MemberExpr>(date->loc(), date, toString);
  auto *mode = this->state.ast.make<IntLitExpr>(date->loc(), TZ_DATE_TEXT_FULL);
  mode->type = Type::intTy();

  return this->state.library.common.builtinCall(date->loc(), callee, Builtin::DateToLocalString,
                                                {mode}, Type::string());
}

void LibraryDateRegistry::timeArgSlot(const TimeArgSlot &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishTimeArg{.slot = task.slot});
  tasks.pushTask(ApparentSlot{.slot = task.slot});
  if ((*task.slot)->type != nullptr) {
    return;
  }

  return tasks.pushTask(CheckExprSlot{.slot = task.slot, .expected = Type::intTy()});
}

void LibraryDateRegistry::finishTimeArg(const FinishTimeArg &task) {
  const Type *type = (*task.slot)->type;
  if (type->isInt()) {
    return;
  }
  if (type->isInteger()) {
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::intTy()});
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = task.slot, .to = Type::intTy(), .what = ""});
}

void LibraryDateRegistry::pushDateFields(std::vector<Expr *> *args, size_t first,
                                         llvm::StringRef setter) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterDateFields{.args = args, .first = first, .setter = setter});
  for (Expr *&arg : llvm::reverse(*args)) {
    tasks.pushTask(TimeArgSlot{.slot = &arg});
  }
}

void LibraryDateRegistry::afterDateFields(const AfterDateFields &task) {
  llvm::ArrayRef<Expr *> args = *task.args;
  auto fields = DateFieldValues{.first = task.first, .setter = task.setter, .constant = {}};
  for (size_t index = 0; index < args.size(); ++index) {
    if (args[index]->type->isInteger()) {
      fields.constant[task.first + index] = this->state.code.constants.intConstant(args[index]);
    }
  }
  this->walkYearField(args, fields);
  auto month = this->validMonth(args, fields);
  this->walkDayField(args, fields, month);
  this->walkTimeFields(args, fields);
}

void LibraryDateRegistry::checkDateNew(const CheckDateNew &task) {
  return this->startDateNew(task.newExpr->loc(), &task.newExpr->args);
}

void LibraryDateRegistry::startDateNew(llvm::SMLoc loc, std::vector<Expr *> *args) {
  auto &tasks = this->state.walk.tasks;
  if (args->size() > kMostDateArgs) {
    auto message = ErrorTextDateNew::arity(args->size());
    this->state.diagnostics.error(loc, message);
    tasks.pushChecked(this->failedDate(loc));
    for (Expr *&arg : llvm::reverse(*args)) {
      tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
    }

    return;
  }
  if (args->empty()) {
    auto *made = this->dateCall(loc, Builtin::DateNew, {});

    return tasks.pushChecked(made);
  }
  if (args->size() > 1) {
    tasks.pushTask(FinishDateFields{.loc = loc, .args = args});

    return this->pushDateFields(args, 0, "");
  }
  Expr *given = (*args)[0];
  auto literal = this->checkDateLiteral(given);
  if (literal.isErr()) {
    this->state.diagnostics.error(given->loc(), literal.error().message);
  }
  tasks.pushTask(AfterDateValue{.loc = loc, .args = args, .given = given});
  tasks.pushTask(ApparentSlot{.slot = &(*args)[0]});

  return tasks.pushTask(CheckExprSlot{.slot = &(*args)[0], .expected = nullptr});
}

void LibraryDateRegistry::finishDateFields(const FinishDateFields &task) {
  auto *made = this->dateCall(task.loc, Builtin::DateFromFields, *task.args);

  return this->state.walk.tasks.pushChecked(made);
}

void LibraryDateRegistry::afterDateValue(const AfterDateValue &task) {
  llvm::SMLoc loc = task.loc;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = (*task.args)[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return tasks.pushChecked(this->failedDate(loc));
  }
  if (type->isDate()) {
    auto *copy = this->dateCall(loc, Builtin::DateCopy, {arg});

    return tasks.pushChecked(copy);
  }
  if (type->isString() && !this->literalText(task.given).has_value()) {
    auto message = ErrorTextDateNew::unknown();
    this->state.diagnostics.error(arg->loc(), message);

    return tasks.pushChecked(this->failedDate(loc));
  }
  if (type->isString()) {
    auto *parsed = this->dateCall(loc, Builtin::DateFromString, {arg});

    return tasks.pushChecked(parsed);
  }
  if (type->isNumeric()) {
    tasks.pushTask(FinishDateTime{.loc = loc, .args = task.args});

    return tasks.pushTask(TimeArgSlot{.slot = &(*task.args)[0]});
  }
  auto message = ErrorTextDateValue::takes(type);
  this->state.diagnostics.error(arg->loc(), message);

  return tasks.pushChecked(this->failedDate(loc));
}

void LibraryDateRegistry::finishDateTime(const FinishDateTime &task) {
  auto *made = this->dateCall(task.loc, Builtin::DateNew, {(*task.args)[0]});

  return this->state.walk.tasks.pushChecked(made);
}

Expr *LibraryDateRegistry::dateCall(llvm::SMLoc loc, Builtin builtin, std::vector<Expr *> args) {
  auto dateName = GlobalNameTable::getName(GlobalName::DATE);
  auto *callee = this->state.ast.make<NameExpr>(loc, dateName);

  return this->state.library.common.builtinCall(loc, callee, builtin, std::move(args),
                                                Type::date());
}

Expr *LibraryDateRegistry::failedDate(llvm::SMLoc loc) {
  auto *error = this->state.ast.make<UndefinedExpr>(loc);
  error->type = Type::error();

  return error;
}

void LibraryDateRegistry::checkDateFunction(const CheckDateFunction &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (!call->args.empty()) {
    auto message = ErrorTextDateNew::call();
    this->state.diagnostics.error(task.name->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  call->target = BuiltinCall{.builtin = Builtin::DateText};
  call->type = Type::string();
}

void LibraryDateRegistry::checkDateStatic(const CheckDateStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto dateStatic = DateStaticTable::find(member->name);
  bool isUtc = dateStatic == DateStatic::UTC;
  bool isParse = dateStatic == DateStatic::PARSE;
  if (!isUtc && !isParse) {
    this->reportDateStaticMember(member);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, isUtc ? 7 : 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  if (isUtc) {
    call->target = BuiltinCall{.builtin = Builtin::DateUTC};
    call->type = Type::intTy();

    return this->pushDateFields(&call->args, 0, "");
  }
  auto literal = this->checkDateLiteral(call->args[0]);
  if (literal.isErr()) {
    this->state.diagnostics.error(call->args[0]->loc(), literal.error().message);
  }
  tasks.pushTask(FinishDateParse{.call = call});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::DATE_ERROR});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryDateRegistry::finishDateParse(const FinishDateParse &task) {
  const Type *dateError = this->state.walk.tasks.takeAnswer();
  task.call->target = BuiltinCall{.builtin = Builtin::DateParse};
  task.call->type = Type::result(Type::intTy(), dateError);
}

void LibraryDateRegistry::checkDateStaticMember(const CheckDateStaticMember &task) {
  this->reportDateStaticMember(task.member);

  return this->state.walk.tasks.pushChecked(task.member);
}

void LibraryDateRegistry::reportDateStaticMember(MemberExpr *member) {
  member->type = Type::error();
  llvm::StringRef name = member->name;
  bool isStatic = DateStaticTable::find(name).has_value();
  std::string message =
      isStatic ? ErrorTextDateStatic::onlyCalled(name) : ErrorTextDateStatic::noMember(name);
  this->state.diagnostics.error(member->loc(), message);
}

void LibraryDateRegistry::checkDateMethod(const CheckDateMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  const auto *method = DateMethodTable::find(member->name);
  if (method == nullptr) {
    auto message = this->unknownMethodMessage(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, method->min, method->max);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = method->builtin};

  return this->checkDateMethodForm(call, member, *method);
}

void LibraryDateRegistry::checkDateMethodForm(CallExpr *call, MemberExpr *member,
                                              const DateMethodInfo &method) {
  auto &tasks = this->state.walk.tasks;
  switch (method.form) {
    case DateMethodForm::GET_TIME:
      call->type = Type::intTy();
      return;
    case DateMethodForm::GET_FIELD:
      call->args.push_back(this->fieldLiteral(member, method.field));
      call->type = Type::intTy();
      return;
    case DateMethodForm::SET_TIME:
      call->type = Type::intTy();
      return tasks.pushTask(TimeArgSlot{.slot = &call->args[0]});
    case DateMethodForm::SET_FIELDS:
      tasks.pushTask(DateSetterFields{.call = call, .member = member, .field = method.field});
      for (Expr *&arg : llvm::reverse(call->args)) {
        tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = Type::intTy()});
      }
      return;
    case DateMethodForm::TO_JSON:
      call->type = Type::string();
      if (call->args.empty()) {
        return;
      }
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
    case DateMethodForm::TO_TEXT:
      call->type = Type::string();
      return;
    case DateMethodForm::TO_FIELD_TEXT:
      call->args.push_back(this->fieldLiteral(member, method.field));
      call->type = Type::string();
      return;
    case DateMethodForm::TO_LOCALE:
      tasks.pushTask(AfterLocaleArgs{.call = call, .member = member, .field = method.field});
      return tasks.pushTask(PrepareLocaleArgs{.args = &call->args, .field = method.field});
  }
}

void LibraryDateRegistry::dateSetterFields(const DateSetterFields &task) {
  int field = task.field;
  size_t first = size_t(field >= TZ_DATE_HOURS ? field - 1 : field);
  this->state.walk.tasks.pushTask(
      FinishDateSetter{.call = task.call, .member = task.member, .field = field});

  return this->pushDateFields(&task.call->args, first, task.member->name);
}

void LibraryDateRegistry::finishDateSetter(const FinishDateSetter &task) {
  CallExpr *call = task.call;
  call->args.insert(call->args.begin(), this->fieldLiteral(task.member, task.field));
  call->type = Type::intTy();
}

void LibraryDateRegistry::afterLocaleArgs(const AfterLocaleArgs &task) {
  CallExpr *call = task.call;
  bool isPrepared = this->state.walk.tasks.takeVerdict();
  if (!isPrepared) {
    return;
  }
  call->args.insert(call->args.begin(), this->fieldLiteral(task.member, task.field));
  call->type = Type::string();
}

void LibraryDateRegistry::checkDateMember(const CheckDateMember &task) {
  MemberExpr *member = task.member;
  member->type = Type::error();
  llvm::StringRef name = member->name;
  bool isMethod = DateMethodTable::find(name) != nullptr;
  bool isGetYear = DateLegacyMethodTable::find(name) == DateLegacyMethod::GET_YEAR;
  std::string message = isMethod    ? ErrorTextDateMethod::onlyCalled(name)
                        : isGetYear ? ErrorTextDateMethod::getYearVal()
                                    : ErrorTextDateMethod::noMember(name);
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushChecked(member);
}

void LibraryDateRegistry::checkDateOperands(const CheckDateOperands &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  this->state.walk.tasks.pushChecked(binary);
  bool isCompare = binary->op == BinaryOp::Lt || binary->op == BinaryOp::Le ||
                   binary->op == BinaryOp::Gt || binary->op == BinaryOp::Ge;
  bool isTimeSpan = isCompare || binary->op == BinaryOp::Sub;
  if (leftType->isDate() && rightType->isDate() && isTimeSpan) {
    binary->lhs = this->dateTime(binary->lhs);
    binary->rhs = this->dateTime(binary->rhs);
    binary->type = isCompare ? Type::boolTy() : Type::intTy();

    return;
  }
  auto message = this->dateOperatorText(binary, isCompare);
  this->state.diagnostics.error(binary->loc(), message);
}

std::string LibraryDateRegistry::dateOperatorText(const BinaryExpr *binary, bool isCompare) const {
  const char *op = Operators::opSpelling(binary->op);
  if (binary->op == BinaryOp::Add) {
    return ErrorTextDateOperator::plus();
  }
  if (binary->op == BinaryOp::Sub) {
    return ErrorTextDateOperator::minus();
  }
  if (isCompare) {
    return ErrorTextDateOperator::compare(op);
  }
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isUndefined = leftType->isOptional() || rightType->isOptional() || leftType->isUndefined() ||
                     rightType->isUndefined();

  return ErrorTextOperands::types(op, leftType->name(), rightType->name(), isUndefined);
}

std::optional<std::u16string> LibraryDateRegistry::literalText(const Expr *expr) const {
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

bool LibraryDateRegistry::isLeap(int64_t year) const {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int LibraryDateRegistry::daysInMonth(int64_t year, int64_t month) const {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  return month == 1 && this->isLeap(year) ? 29 : days[month];
}

std::pair<int64_t, int64_t> LibraryDateRegistry::carriedDay(int64_t year, int64_t month,
                                                            int64_t day) const {
  int64_t currentYear = year;
  int64_t currentMonth = month;
  int64_t currentDay = day;
  while (currentDay > this->daysInMonth(currentYear, currentMonth)) {
    currentDay -= this->daysInMonth(currentYear, currentMonth);
    if (++currentMonth == 12) {
      currentMonth = 0;
      ++currentYear;
    }
  }

  return {currentMonth, currentDay};
}

std::string LibraryDateRegistry::missingDay(std::optional<int64_t> year, int64_t month,
                                            int64_t day) const {
  std::pair<int64_t, int64_t> carried = year.has_value() ? this->carriedDay(*year, month, day)
                                                         : std::pair<int64_t, int64_t>(month, day);

  return ErrorTextDateDay::missing(year, month, day, carried.first, carried.second);
}

LibraryDateRegistry::ResultLiteralDate
LibraryDateRegistry::checkLiteralDate(const std::u16string &text) const {
  std::vector<uint16_t> units(text.begin(), text.end());
  TzDateFields fields;
  auto shown = ToUtf8::toUtf8(text);
  if (!tz_date_parse_fields(units.data(), units.size(), &fields)) {
    auto message = ErrorTextDateLiteral::unreadable(shown);

    return err(LibraryDateRegistryLiteralDateError{
        .code = LibraryDateRegistryLiteralDateErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (fields.date_only) {
    auto message = ErrorTextDateLiteral::dateOnly(shown);

    return err(LibraryDateRegistryLiteralDateError{
        .code = LibraryDateRegistryLiteralDateErrorCode::DATE_ONLY,
        .message = message,
    });
  }
  int64_t month = int64_t(fields.month);
  int64_t dayOfMonth = int64_t(fields.day);
  if (dayOfMonth > this->daysInMonth(int64_t(fields.year), month)) {
    auto missing = this->missingDay(int64_t(fields.year), month, dayOfMonth);
    auto message = ErrorTextDateLiteral::noDay(shown, missing);

    return err(LibraryDateRegistryLiteralDateError{
        .code = LibraryDateRegistryLiteralDateErrorCode::NO_SUCH_DAY,
        .message = message,
    });
  }
  double day = tz_date_make_day(fields.year, fields.month, fields.day);
  double time = tz_date_make_time(fields.hour, fields.minute, fields.second, fields.ms);
  double value = tz_date_make_date(day, time);
  double limit = TZ_DATE_MAX_TIME;
  if (std::isnan(fields.offset)) {
    limit = TZ_DATE_MAX_LOCAL_TIME;
  } else {
    value -= fields.offset * 1000;
  }
  if (!(value >= -limit && value <= limit)) {
    auto message = ErrorTextDateLiteral::outside(shown);

    return err(LibraryDateRegistryLiteralDateError{
        .code = LibraryDateRegistryLiteralDateErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(value);
}

LibraryDateRegistry::ResultDateLiteralChecked
LibraryDateRegistry::checkDateLiteral(const Expr *arg) {
  auto text = this->literalText(arg);
  if (!text) {
    return ok();
  }
  auto date = this->checkLiteralDate(*text);
  if (date.isErr()) {
    return err(date.error());
  }

  return ok();
}

void LibraryDateRegistry::reportField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                                      size_t field, const std::string &message) {
  this->state.diagnostics.error(args[field - fields.first]->loc(), message);
}

void LibraryDateRegistry::walkYearField(llvm::ArrayRef<Expr *> args,
                                        const DateFieldValues &fields) {
  std::optional<int64_t> year = fields.constant[0];
  bool isShortYear = fields.setter.empty() && year.has_value() && *year >= 0 && *year <= 99;
  if (isShortYear) {
    auto message = ErrorTextDateFields::shortYear(*year);
    this->reportField(args, fields, 0, message);
  }
}

std::optional<int64_t> LibraryDateRegistry::validMonth(llvm::ArrayRef<Expr *> args,
                                                       const DateFieldValues &fields) {
  std::optional<int64_t> month = fields.constant[1];
  if (!month.has_value() || (*month >= 0 && *month <= 11)) {
    return month;
  }
  std::string message =
      *month == 12 ? ErrorTextDateFields::month12() : ErrorTextDateFields::month(*month);
  this->reportField(args, fields, 1, message);

  return std::nullopt;
}

void LibraryDateRegistry::walkDayField(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                                       std::optional<int64_t> month) {
  std::optional<int64_t> day = fields.constant[2];
  if (!day.has_value()) {
    return;
  }
  if (*day < 0) {
    auto message = ErrorTextDateFields::dayBelow(*day);
    this->reportField(args, fields, 2, message);
    return;
  }
  if (month.has_value()) {
    this->walkDayInMonth(args, fields, *day, *month);
    return;
  }
  if (*day > 31) {
    auto message = ErrorTextDateFields::dayAbove(*day);
    this->reportField(args, fields, 2, message);
  }
}

void LibraryDateRegistry::walkDayInMonth(llvm::ArrayRef<Expr *> args, const DateFieldValues &fields,
                                         int64_t day, int64_t month) {
  std::optional<int64_t> year = fields.constant[0];
  bool isShortYear = fields.setter.empty() && year.has_value() && *year >= 0 && *year <= 99;
  int64_t fullYear = isShortYear ? 1900 + *year : year.value_or(0);
  std::optional<int64_t> known = year.has_value() ? std::optional<int64_t>(fullYear) : std::nullopt;
  int last = this->lastDayOf(known, month);
  if (day <= last) {
    return;
  }
  auto missing = this->missingDay(known, month, day);
  auto message = this->dayOverflowText(fields.setter, missing, month, known, last);
  this->reportField(args, fields, 2, message);
}

int LibraryDateRegistry::lastDayOf(std::optional<int64_t> year, int64_t month) const {
  if (year.has_value()) {
    return this->daysInMonth(*year, month);
  }
  if (month == 1) {
    return 29;
  }

  return this->daysInMonth(2001, month);
}

std::string LibraryDateRegistry::dayOverflowText(llvm::StringRef setter, const std::string &missing,
                                                 int64_t month, std::optional<int64_t> year,
                                                 int last) const {
  if (setter.empty()) {
    auto nextYear = this->nextMonthYear(year, month);
    int64_t next = month == 11 ? 0 : month + 1;
    return ErrorTextDateFields::lastDay(missing, month, nextYear, next);
  }
  if (month == 1) {
    return ErrorTextDateFields::february(missing, setter, year);
  }

  return ErrorTextDateFields::monthDays(missing, month, last);
}

std::optional<int64_t> LibraryDateRegistry::nextMonthYear(std::optional<int64_t> year,
                                                          int64_t month) const {
  if (!year.has_value()) {
    return std::nullopt;
  }

  return month == 11 ? *year + 1 : *year;
}

void LibraryDateRegistry::walkTimeFields(llvm::ArrayRef<Expr *> args,
                                         const DateFieldValues &fields) {
  static const int64_t maxima[] = {23, 59, 59, 999};
  size_t end = fields.first + args.size();
  for (size_t index = std::max<size_t>(fields.first, 3); index < end; ++index) {
    std::optional<int64_t> value = fields.constant[index];
    int64_t max = maxima[index - 3];
    if (value.has_value() && (*value < 0 || *value > max)) {
      auto what = ErrorTextDateFields::timeField(index - 3);
      auto message = ErrorTextDateFields::range(*value, what, max);
      this->reportField(args, fields, index, message);
    }
  }
}

std::string LibraryDateRegistry::unknownMethodMessage(llvm::StringRef name) const {
  auto legacy = DateLegacyMethodTable::find(name);
  if (legacy == DateLegacyMethod::GET_YEAR) {
    return ErrorTextDateMethod::getYear();
  }
  if (legacy == DateLegacyMethod::SET_YEAR) {
    return ErrorTextDateMethod::setYear();
  }
  if (legacy == DateLegacyMethod::TO_GMT_STRING) {
    return ErrorTextDateMethod::gmt();
  }

  return ErrorTextDateMethod::noMember(name);
}

Expr *LibraryDateRegistry::fieldLiteral(const MemberExpr *member, int value) {
  auto *literal = this->state.ast.make<IntLitExpr>(member->loc(), uint64_t(value));
  literal->type = Type::intTy();

  return literal;
}

}
