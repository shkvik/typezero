#include "compiler/semantic/library_v2/regex/library_regex_registry.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/regex/regex_flag_table/regex_flag_table.h"
#include "compiler/program/library/regex/regex_static_table/regex_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kRegexSpecials = "^$\\.*+?()[]{}|/";
constexpr llvm::StringLiteral kStringPlaceholder = "s";

}

LibraryRegexRegistry::LibraryRegexRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryRegexRegistry::matchItems() {
  auto &tasks = this->state.walk.tasks;
  Expr *match = tasks.takeChecked();
  auto *items = this->itemsOfMatch(match);

  return tasks.pushChecked(items);
}

Expr *LibraryRegexRegistry::itemsOfMatch(Expr *match) {
  auto optionals = std::vector<Expr *>();
  Expr *link = match;
  while (link->type->isOptional()) {
    this->state.walk.functions.enterOptionalChain();
    optionals.push_back(link);
    link = this->matchLink(link);
  }
  auto *items = this->itemsMember(link);
  for (Expr *optional : llvm::reverse(optionals)) {
    items = this->optionalItems(optional, items);
  }

  return items;
}

Expr *LibraryRegexRegistry::matchLink(Expr *object) {
  auto *link = this->state.ast.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = object->type->inner();
  link->shortCircuit = true;
  this->state.walk.functions.markShortCircuit();

  return link;
}

void LibraryRegexRegistry::checkMatchMember(const CheckMatchMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  if (BuiltinPropertyTable::isName(name, BuiltinProperty::LENGTH)) {
    member->object = this->itemsOfMatch(member->object);
    member->type = Type::intTy();

    return tasks.pushChecked(member);
  }
  auto known = MatchMemberTable::find(name);
  if (known == MatchMember::INDEX || known == MatchMember::INPUT) {
    return tasks.pushTask(CheckField{.member = member});
  }
  member->type = Type::error();
  auto groupsName = MatchMemberTable::getName(MatchMember::GROUPS);
  auto indicesName = MatchMemberTable::getName(MatchMember::INDICES);
  bool hasGroups = known == MatchMember::GROUPS && !type->field(groupsName)->type->isOptional();
  bool hasIndices = known == MatchMember::INDICES && type->field(indicesName) != nullptr;
  if (hasGroups || hasIndices) {
    return tasks.pushTask(CheckField{.member = member});
  }
  auto message = this->matchMemberText(member);
  this->state.diagnostics.error(member->loc(), message);

  return tasks.pushChecked(member);
}

void LibraryRegexRegistry::checkMatchIndex(const CheckMatchIndex &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterMatchPosition{.index = task.index});

  return tasks.pushTask(CheckPosition{.slot = &task.index->index});
}

void LibraryRegexRegistry::afterMatchPosition(const AfterMatchPosition &task) {
  IndexExpr *index = task.index;
  auto &tasks = this->state.walk.tasks;
  const Type *type = index->object->type;
  index->type = Type::error();
  tasks.pushChecked(index);
  if (index->index->type->isError()) {
    return;
  }
  index->object = this->itemsOfMatch(index->object);
  const auto *item = Type::optional(Type::string());
  auto position = this->state.code.constants.intConstant(index->index);
  int groups = type->matchGroups();
  if (position.has_value() && *position == 0) {
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;

    return tasks.pushTask(Convert{.to = Type::string()});
  }
  if (position.has_value() && groups >= 0) {
    auto group = this->checkGroupIndex(*position, groups);
    if (group.isErr()) {
      return this->state.diagnostics.error(index->index->loc(), group.error().message);
    }
    index->form = ElementIndex{.mayBeOutOfRange = false};
    index->type = item;
    if (type->captureMandatory(unsigned(*position))) {
      return tasks.pushTask(Convert{.to = Type::string()});
    }

    return;
  }
  index->form = ElementIndex{.mayBeOutOfRange = true};
  index->type = item;
}

void LibraryRegexRegistry::checkRegexLiteral(const CheckRegexLiteral &task) {
  this->compileRegexLit(task.literal);

  return this->state.walk.tasks.pushChecked(task.literal);
}

void LibraryRegexRegistry::noteRegexVariable(const NoteRegexVariable &task) {
  const auto *literal = llvm::dyn_cast_or_null<RegexLitExpr>(task.value);
  if (literal == nullptr || literal->program.empty()) {
    return;
  }
  if (!task.isConst && !this->isStableRegexName(task.variable->name, task.block, task.blockIndex)) {
    return;
  }
  task.variable->holds = RegexLiteral{.regex = literal};
}

void LibraryRegexRegistry::regexToString() {
  auto &tasks = this->state.walk.tasks;
  Expr *regex = tasks.takeChecked();
  auto *text = this->stringOfRegex(regex);

  return tasks.pushChecked(text);
}

Expr *LibraryRegexRegistry::stringOfRegex(Expr *regex) {
  auto stringName = GlobalNameTable::getName(GlobalName::STRING);
  auto *callee = this->state.ast.make<NameExpr>(regex->loc(), stringName);

  return this->state.library.common.builtinCall(regex->loc(), callee, Builtin::RegExpToString,
                                                {regex}, Type::string());
}

void LibraryRegexRegistry::checkRegexMethod(const CheckRegexMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const auto *regex = this->knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto method = RegexMemberTable::find(member->name);
  if (method == RegexMember::TEST || method == RegexMember::EXEC) {
    return this->checkRegexRun(call, member, regex, method.value());
  }
  if (method == RegexMember::TO_STRING) {
    return this->checkRegexToString(call, member);
  }
  std::string message = method == RegexMember::COMPILE
                            ? ErrorTextRegexMethod::compile()
                            : ErrorTextRegexMethod::noMember(member->name);
  this->state.diagnostics.error(member->loc(), message);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryRegexRegistry::checkRegexRun(CallExpr *call, MemberExpr *member,
                                         const RegexLitExpr *regex, RegexMember method) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(
      AfterRegexRunArg{.call = call, .member = member, .regex = regex, .method = method});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryRegexRegistry::afterRegexRunArg(const AfterRegexRunArg &task) {
  CallExpr *call = task.call;
  const RegexLitExpr *regex = task.regex;
  uint32_t flags = regex != nullptr ? tz_re_flags(regex->program.data()) : 0;
  bool isTest = task.method == RegexMember::TEST;
  if (isTest && (flags & TZ_RE_GLOBAL) != 0) {
    auto message = ErrorTextRegexMethod::testG();

    return this->state.diagnostics.error(task.member->loc(), message);
  }
  if (isTest) {
    call->target = BuiltinCall{.builtin = Builtin::RegExpTest};
    call->type = Type::boolTy();

    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::RegExpExec};
  const auto *match = this->matchType(regex);
  call->type = Type::optional(match);
}

void LibraryRegexRegistry::checkRegexToString(CallExpr *call, const MemberExpr *member) {
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  auto *text = this->stringOfRegex(member->object);
  call->args.push_back(text);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();
}

void LibraryRegexRegistry::checkRegexMember(const CheckRegexMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef name = member->name;
  auto known = RegexMemberTable::find(name);
  if (known == RegexMember::LAST_INDEX) {
    return tasks.pushTask(CheckField{.member = member});
  }
  auto *value = this->regexValue(member, known);
  if (value != nullptr) {
    return tasks.pushChecked(value);
  }
  bool isMethod = known == RegexMember::TEST || known == RegexMember::EXEC;
  auto message = ErrorTextRegexMethod::notCalled(name, isMethod);
  this->state.diagnostics.error(member->loc(), message);
  member->type = Type::error();

  return tasks.pushChecked(member);
}

Expr *LibraryRegexRegistry::regexValue(MemberExpr *member, std::optional<RegexMember> known) {
  auto &common = this->state.library.common;
  llvm::SMLoc loc = member->loc();
  if (known == RegexMember::SOURCE) {
    return common.builtinCall(loc, member, Builtin::RegExpSource, {}, Type::string());
  }
  if (known == RegexMember::FLAGS) {
    return common.builtinCall(loc, member, Builtin::RegExpFlags, {}, Type::string());
  }
  auto flag = this->flagBit(member->name);
  if (!flag.has_value()) {
    return nullptr;
  }
  auto *bit = this->state.ast.make<IntLitExpr>(loc, flag.value());
  bit->type = Type::intTy();

  return common.builtinCall(loc, member, Builtin::RegExpFlag, {bit}, Type::boolTy());
}

void LibraryRegexRegistry::checkRegExpConstructor(const CheckRegExpConstructor &task) {
  return this->startRegExpNew(task.newExpr->loc(), &task.newExpr->args);
}

void LibraryRegexRegistry::checkRegExpCall(const CheckRegExpCall &task) {
  return this->startRegExpNew(task.name->loc(), &task.call->args);
}

void LibraryRegexRegistry::startRegExpNew(llvm::SMLoc loc, std::vector<Expr *> *args) {
  auto &tasks = this->state.walk.tasks;
  if (args->empty() || args->size() > 2) {
    auto message = ErrorTextRegExpNew::arity(args->size());
    this->state.diagnostics.error(loc, message);

    return this->failRegExpNew(loc, args);
  }
  Expr *patternArg = (*args)[0];
  std::optional<std::u16string> pattern = this->constantText(patternArg);
  std::optional<std::u16string> flags =
      args->size() > 1 ? this->constantText((*args)[1]) : std::u16string();
  if (pattern.has_value() && flags.has_value()) {
    auto *literal = this->state.ast.make<RegexLitExpr>(loc, *pattern, *flags);
    this->compileRegexLit(literal);

    return tasks.pushChecked(literal);
  }
  if (!this->isRegExpEscape(patternArg) || !flags.has_value()) {
    auto message = ErrorTextRegExpNew::unknown(args->size() > 1);
    this->state.diagnostics.error(loc, message);

    return this->failRegExpNew(loc, args);
  }
  auto parsed = this->checkParsedFlags(*flags);
  if (parsed.isErr()) {
    this->state.diagnostics.error((*args)[1]->loc(), parsed.error().message);

    return this->failRegExpNew(loc, args);
  }
  tasks.pushTask(FinishRegExpNew{.loc = loc, .args = args});
  if (args->size() > 1) {
    tasks.pushTask(CheckArgSlot{.slot = &(*args)[1], .to = Type::string()});
  }
  tasks.pushTask(CoerceSlot{.slot = &(*args)[0], .to = Type::string(), .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = &(*args)[0], .expected = nullptr});
}

void LibraryRegexRegistry::finishRegExpNew(const FinishRegExpNew &task) {
  auto regExpName = GlobalNameTable::getName(GlobalName::REG_EXP);
  auto *callee = this->state.ast.make<NameExpr>(task.loc, regExpName);
  auto *made = this->state.library.common.builtinCall(task.loc, callee, Builtin::RegExpNew,
                                                      *task.args, Type::regExp());

  return this->state.walk.tasks.pushChecked(made);
}

void LibraryRegexRegistry::failRegExpNew(llvm::SMLoc loc, std::vector<Expr *> *args) {
  auto &tasks = this->state.walk.tasks;
  auto *error = this->state.ast.make<UndefinedExpr>(loc);
  error->type = Type::error();
  tasks.pushChecked(error);
  for (Expr *&arg : llvm::reverse(*args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryRegexRegistry::checkRegExpStatic(const CheckRegExpStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto function = RegexStaticTable::find(member->name);
  bool isKnown = function == RegexStatic::PARSE || function == RegexStatic::ESCAPE;
  if (!isKnown) {
    this->reportRegExpStaticMember(member);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  bool isParse = function == RegexStatic::PARSE;
  auto arity = this->state.library.common.checkArity(call, 1, isParse ? 2 : 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  if (!isParse) {
    call->target = BuiltinCall{.builtin = Builtin::RegExpEscape};
    call->type = Type::string();

    return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
  }
  tasks.pushTask(FinishRegExpParse{.call = call});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::REGEX_ERROR});
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(CheckArgSlot{.slot = &arg, .to = Type::string()});
  }
}

void LibraryRegexRegistry::finishRegExpParse(const FinishRegExpParse &task) {
  const Type *regexError = this->state.walk.tasks.takeAnswer();
  task.call->target = BuiltinCall{.builtin = Builtin::RegExpParse};
  task.call->type = Type::result(Type::regExp(), regexError);
}

void LibraryRegexRegistry::checkRegExpStaticMember(const CheckRegExpStaticMember &task) {
  this->reportRegExpStaticMember(task.member);

  return this->state.walk.tasks.pushChecked(task.member);
}

void LibraryRegexRegistry::reportRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  auto message = this->staticMemberText(member->name);
  this->state.diagnostics.error(member->loc(), message);
}

void LibraryRegexRegistry::checkStringRegexMethod(const CheckStringRegexMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const auto *found = StringMethodTable::find(member->name);
  bool isTakingPattern = found != nullptr && (found->form.is<TextOrPatternStringMethod>() ||
                                              found->form.is<PatternStringMethod>());
  bool isBareSplit = found != nullptr && found->method == StringMethod::SPLIT && call->args.empty();
  if (!isTakingPattern || isBareSplit) {
    return tasks.pushChecked(nullptr);
  }
  StringMethod method = found->method;
  call->type = Type::error();
  tasks.pushChecked(call);
  bool isReplace = method == StringMethod::REPLACE || method == StringMethod::REPLACE_ALL;
  unsigned min = this->patternMinArgs(method);
  unsigned max = method == StringMethod::SPLIT || isReplace ? 2 : 1;
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterRegexPattern{.call = call, .member = member, .method = method});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryRegexRegistry::afterRegexPattern(const AfterRegexPattern &task) {
  CallExpr *call = task.call;
  const Expr *pattern = call->args[0];
  const Type *type = pattern->type;
  if (type->isError()) {
    return this->checkRestLoosely(call);
  }
  if (type->isRegExp()) {
    return this->checkRegexPattern(task);
  }
  if (type->isString()) {
    return this->checkTextPattern(task);
  }
  bool isTextAllowed =
      task.method == StringMethod::SPLIT || task.method == StringMethod::REPLACE_ALL;
  auto message = ErrorTextStringRegex::notRegex(task.member->name, isTextAllowed, type->name());
  this->state.diagnostics.error(pattern->loc(), message);

  return this->checkRestLoosely(call);
}

void LibraryRegexRegistry::checkRestLoosely(CallExpr *call) {
  for (size_t index = call->args.size(); index > 1; --index) {
    this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &call->args[index - 1], .expected = nullptr});
  }
}

void LibraryRegexRegistry::checkRegexPattern(const AfterRegexPattern &pattern) {
  CallExpr *call = pattern.call;
  const auto *regex = this->knownRegex(call->args[0]);
  call->regex = regex;
  StringMethod method = pattern.method;
  if (method == StringMethod::MATCH) {
    return this->checkRegexMatch(pattern, regex);
  }
  bool isNeedingGlobal = method == StringMethod::MATCH_ALL || method == StringMethod::REPLACE_ALL;
  if (isNeedingGlobal && this->isMissingGlobalFlag(regex)) {
    return this->reportNeedsGlobal(pattern, regex);
  }
  if (method == StringMethod::MATCH_ALL) {
    const auto *matches = Type::iterator(this->matchType(regex));

    return this->toBuiltinCall(call, Builtin::StrMatchAll, matches);
  }
  if (method == StringMethod::SEARCH) {
    return this->toBuiltinCall(call, Builtin::StrSearch, Type::intTy());
  }
  if (method == StringMethod::SPLIT) {
    return this->checkRegexSplit(call, regex);
  }
  if (this->isFunctionReplacement(call->args[1])) {
    return this->checkFunctionReplacement(call, pattern.member, regex);
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterTextReplacement{.call = call, .regex = regex});

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::string()});
}

void LibraryRegexRegistry::checkRegexMatch(const AfterRegexPattern &pattern,
                                           const RegexLitExpr *regex) {
  CallExpr *call = pattern.call;
  if (regex == nullptr) {
    auto message = ErrorTextStringRegex::matchFlag();

    return this->state.diagnostics.error(pattern.member->loc(), message);
  }
  uint32_t flags = tz_re_flags(regex->program.data());
  bool isGlobal = (flags & TZ_RE_GLOBAL) != 0;
  if (isGlobal) {
    const auto *matches = Type::optional(Type::array(Type::string()));

    return this->toBuiltinCall(call, Builtin::StrMatchGlobal, matches);
  }
  const auto *match = Type::optional(this->matchType(regex));

  return this->toBuiltinCall(call, Builtin::StrMatch, match);
}

void LibraryRegexRegistry::reportNeedsGlobal(const AfterRegexPattern &pattern,
                                             const RegexLitExpr *regex) {
  auto source = ToUtf8::toUtf8(regex->pattern);
  auto flagsText = ToUtf8::toUtf8(regex->flags);
  auto message = ErrorTextStringRegex::needsG(pattern.member->name, source, flagsText);
  this->state.diagnostics.error(pattern.call->args[0]->loc(), message);

  return this->checkRestLoosely(pattern.call);
}

void LibraryRegexRegistry::checkRegexSplit(CallExpr *call, const RegexLitExpr *regex) {
  bool isUndefinedGroups = this->hasOptionalGroups(regex);
  const Type *part = isUndefinedGroups ? Type::optional(Type::string()) : Type::string();
  this->toBuiltinCall(call, Builtin::StrSplitRegex, Type::array(part));
  if (call->args.size() == 2) {
    return this->state.walk.tasks.pushTask(CheckPosition{.slot = &call->args[1]});
  }
}

void LibraryRegexRegistry::checkTextPattern(const AfterRegexPattern &pattern) {
  CallExpr *call = pattern.call;
  auto &tasks = this->state.walk.tasks;
  if (pattern.method == StringMethod::SPLIT) {
    this->toBuiltinCall(call, Builtin::StrSplit, Type::array(Type::string()));
    if (call->args.size() == 2) {
      return tasks.pushTask(CheckPosition{.slot = &call->args[1]});
    }

    return;
  }
  const Expr *patternExpr = call->args[0];
  auto text = this->argumentText(patternExpr);
  auto *literal = dyn_cast<StringLitExpr>(patternExpr);
  std::string escaped = literal != nullptr ? this->escapedLiteral(literal->value) : std::string();
  if (pattern.method == StringMethod::REPLACE_ALL) {
    return this->checkTextReplaceAll(call, text, escaped);
  }
  std::string regexText = literal != nullptr ? escaped : ErrorTextStringRegex::escapedRe(text);
  auto message = this->textPatternMessage(pattern, text, regexText, literal != nullptr);
  this->state.diagnostics.error(pattern.member->loc(), message);

  return this->checkRestLoosely(call);
}

std::string LibraryRegexRegistry::textPatternMessage(const AfterRegexPattern &pattern,
                                                     const std::string &text,
                                                     const std::string &regexText,
                                                     bool isWritten) const {
  if (pattern.method == StringMethod::REPLACE) {
    return ErrorTextStringRegex::replaceStr(text, regexText);
  }
  if (pattern.method == StringMethod::SEARCH) {
    return ErrorTextStringRegex::searchStr(text, regexText, isWritten);
  }

  return ErrorTextStringRegex::includeStr(pattern.member->name, text, regexText, isWritten);
}

void LibraryRegexRegistry::checkTextReplaceAll(CallExpr *call, const std::string &text,
                                               const std::string &escaped) {
  const Expr *replacement = call->args[1];
  if (isa<FunctionExpr>(replacement)) {
    auto message = ErrorTextStringRegex::functionRe(escaped, text);
    this->state.diagnostics.error(replacement->loc(), message);

    return this->checkRestLoosely(call);
  }
  this->toBuiltinCall(call, Builtin::StrReplaceAll, Type::string());

  return this->state.walk.tasks.pushTask(
      CheckArgSlot{.slot = &call->args[1], .to = Type::string()});
}

void LibraryRegexRegistry::afterTextReplacement(const AfterTextReplacement &task) {
  CallExpr *call = task.call;
  const RegexLitExpr *regex = task.regex;
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegex};
  call->type = Type::string();
  auto *text = dyn_cast<StringLitExpr>(call->args[1]);
  if (regex == nullptr || text == nullptr) {
    return;
  }
  auto groups = this->checkReplacementGroups(regex, text->value);
  if (groups.isErr()) {
    return this->state.diagnostics.error(text->loc(), groups.error().message);
  }
}

void LibraryRegexRegistry::checkFunctionReplacement(CallExpr *call, MemberExpr *member,
                                                    const RegexLitExpr *regex) {
  auto &tasks = this->state.walk.tasks;
  const auto *match = this->matchType(regex);
  auto signature = this->replacerSignature(regex, match);
  auto *closure = dyn_cast<FunctionExpr>(call->args[1]);
  bool isExtraParams = closure != nullptr && closure->fn->params.size() > signature.params.size();
  if (isExtraParams) {
    return this->checkExtraReplacerParams(call, member, regex, match, std::move(signature));
  }
  auto paramCount = unsigned(signature.params.size());
  tasks.pushTask(AfterFunctionReplacement{.call = call});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1],
      .expected = std::move(signature),
      .primary = paramCount,
      .method = member->name,
  });
}

void LibraryRegexRegistry::afterFunctionReplacement(const AfterFunctionReplacement &task) {
  CallExpr *call = task.call;
  if (!call->args[1]->type->isFunction()) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::StrReplaceRegexFn};
  call->type = Type::string();

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void LibraryRegexRegistry::checkExtraReplacerParams(CallExpr *call, MemberExpr *member,
                                                    const RegexLitExpr *regex, const Type *match,
                                                    Signature signature) {
  auto *closure = llvm::cast<FunctionExpr>(call->args[1]);
  if (regex == nullptr) {
    auto message = ErrorTextStringRegex::onlyMatch();
    this->state.diagnostics.error(closure->fn->params[1].loc, message);
  } else {
    bool hasGroupsObject = signature.params.back()->isObject();
    auto message = ErrorTextStringRegex::passes(member->name, signature.params.size(),
                                                match->matchGroups(), hasGroupsObject);
    this->state.diagnostics.error(closure->fn->params[signature.params.size()].loc, message);
  }
  while (signature.params.size() < closure->fn->params.size()) {
    signature.params.push_back(Type::string());
  }
  auto paramCount = unsigned(signature.params.size());

  return this->state.walk.tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1],
      .expected = std::move(signature),
      .primary = paramCount,
      .method = member->name,
  });
}

Signature LibraryRegexRegistry::replacerSignature(const RegexLitExpr *regex,
                                                  const Type *match) const {
  auto params = std::vector<const Type *>{Type::string()};
  auto signature = Signature{.params = params, .ret = Type::string(), .pad = false};
  if (regex == nullptr) {
    return signature;
  }
  for (int group = 1; group <= match->matchGroups(); ++group) {
    bool isMandatory = match->captureMandatory(unsigned(group));
    signature.params.push_back(isMandatory ? Type::string() : Type::optional(Type::string()));
  }
  signature.params.push_back(Type::intTy());
  signature.params.push_back(Type::string());
  auto groupsName = MatchMemberTable::getName(MatchMember::GROUPS);
  const Type *named = match->field(groupsName)->type;
  if (!named->isOptional()) {
    signature.params.push_back(named);
  }

  return signature;
}

std::string LibraryRegexRegistry::matchMemberText(const MemberExpr *member) const {
  const Type *type = member->object->type;
  auto known = MatchMemberTable::find(member->name);
  if (known == MatchMember::GROUPS) {
    return ErrorTextMatch::groups(type->matchGroups() >= 0);
  }
  if (known == MatchMember::INDICES) {
    return ErrorTextMatch::indices(type->matchGroups() >= 0);
  }

  return ErrorTextMatch::noMember(member->name, type->name());
}

std::vector<const Stmt *> LibraryRegexRegistry::followingStatements(llvm::ArrayRef<Stmt *> stmts,
                                                                    size_t index) const {
  auto following = std::vector<const Stmt *>();
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (isa<FunctionDecl>(stmts[earlier])) {
      following.push_back(stmts[earlier]);
    }
  }
  following.insert(following.end(), stmts.begin() + index + 1, stmts.end());

  return following;
}

void LibraryRegexRegistry::toBuiltinCall(CallExpr *call, Builtin builtin,
                                         const Type *result) const {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;
}

Expr *LibraryRegexRegistry::itemsMember(Expr *match) {
  auto *items = this->state.ast.make<MemberExpr>(match->loc(), match, "%items");
  items->type = this->itemsType();

  return items;
}

Expr *LibraryRegexRegistry::optionalItems(Expr *match, Expr *items) {
  auto *chain = this->state.ast.make<OptionalChainExpr>(match->loc(), items);
  chain->mayShortCircuit = this->state.walk.functions.isShortCircuiting();
  chain->type = Type::optional(this->itemsType());
  this->state.walk.functions.leaveOptionalChain();

  return chain;
}

const Type *LibraryRegexRegistry::itemsType() const {
  return Type::array(Type::optional(Type::string()));
}

LibraryRegexRegistry::ResultGroupIndex LibraryRegexRegistry::checkGroupIndex(int64_t index,
                                                                             int groups) const {
  if (index < 0 || index > groups) {
    auto message = ErrorTextMatch::noGroup(groups, index);

    return err(LibraryRegexRegistryGroupIndexError{
        .code = LibraryRegexRegistryGroupIndexErrorCode::NO_SUCH_GROUP,
        .message = message,
    });
  }

  return ok(index);
}

std::u16string LibraryRegexRegistry::units(const uint32_t *program, uint32_t start,
                                           uint32_t length) const {
  auto text = std::u16string();
  for (uint32_t index = 0; index < length; ++index) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, index));
  }

  return text;
}

std::optional<uint32_t> LibraryRegexRegistry::flagBit(llvm::StringRef name) const {
  return RegexFlagTable::findBit(name);
}

LibraryRegexRegistry::ResultParsedFlags
LibraryRegexRegistry::checkParsedFlags(const std::u16string &flags) const {
  std::vector<uint16_t> flagUnits(flags.begin(), flags.end());
  uint32_t parsed = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &parsed)) {
    auto written = ToUtf8::toUtf8(flags);
    auto message = ErrorTextRegexFlags::invalid(written);

    return err(LibraryRegexRegistryParsedFlagsError{
        .code = LibraryRegexRegistryParsedFlagsErrorCode::INVALID_FLAGS,
        .message = message,
    });
  }

  return ok(parsed);
}

const RegexLitExpr *LibraryRegexRegistry::knownRegex(const Expr *expr) const {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  const auto *literal = dyn_cast<RegexLitExpr>(bare);
  if (literal != nullptr) {
    return literal->program.empty() ? nullptr : literal;
  }
  const auto *name = dyn_cast<NameExpr>(bare);
  if (name != nullptr && name->var() != nullptr) {
    return name->var()->regex();
  }

  return nullptr;
}

bool LibraryRegexRegistry::isStableRegexName(llvm::StringRef name, llvm::ArrayRef<Stmt *> block,
                                             size_t index) const {
  if (block.empty()) {
    return false;
  }
  auto following = this->followingStatements(block, index);

  return !this->isAssigningName(following, name);
}

bool LibraryRegexRegistry::isAssigningName(llvm::ArrayRef<const Stmt *> stmts,
                                           llvm::StringRef name) const {
  auto nodes = PlacedNodes::collect(stmts, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    const auto *assign = dyn_cast<AssignExpr>(placed.expr);
    const auto *target = assign != nullptr ? dyn_cast<NameExpr>(assign->target) : nullptr;
    if (target != nullptr && target->name == name) {
      return true;
    }
  }

  return false;
}

const Type *LibraryRegexRegistry::matchType(const RegexLitExpr *regex) {
  if (regex == nullptr) {
    return Type::match(-1, {}, nullptr, false);
  }
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  std::vector<bool> mandatory(captures);
  for (uint32_t group = 0; group < captures; ++group) {
    mandatory[group] = tz_re_mandatory(program, group);
  }
  auto fields = std::vector<Field>();
  for (uint32_t group = 1; group < captures; ++group) {
    this->addNamedGroup(fields, program, group);
  }
  const Type *named = fields.empty() ? nullptr : Type::object(std::move(fields));
  bool hasIndices = (tz_re_flags(program) & TZ_RE_HAS_INDICES) != 0;

  return Type::match(int(captures) - 1, std::move(mandatory), named, hasIndices);
}

void LibraryRegexRegistry::addNamedGroup(std::vector<Field> &fields, const uint32_t *program,
                                         uint32_t group) const {
  uint32_t start = 0;
  uint32_t length = tz_re_name(program, group, &start);
  if (length == 0) {
    return;
  }
  auto units = this->units(program, start, length);
  auto name = ToUtf8::toUtf8(units);
  if (this->hasFieldNamed(fields, name)) {
    return;
  }
  bool isMandatory = tz_re_name_mandatory(program, group);
  const Type *type = isMandatory ? Type::string() : Type::optional(Type::string());
  fields.push_back({.name = name, .type = type});
}

bool LibraryRegexRegistry::hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

std::optional<std::u16string> LibraryRegexRegistry::constantText(const Expr *expr) const {
  const auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  const auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

bool LibraryRegexRegistry::isRegExpEscape(const Expr *expr) const {
  const auto *escaped = dyn_cast<CallExpr>(expr);
  const auto *escape = escaped != nullptr ? dyn_cast<MemberExpr>(escaped->callee) : nullptr;
  if (escape == nullptr) {
    return false;
  }
  auto function = RegexStaticTable::find(escape->name);

  return function == RegexStatic::ESCAPE &&
         this->state.isGlobalName(escape->object, GlobalName::REG_EXP);
}

std::string LibraryRegexRegistry::staticMemberText(llvm::StringRef name) const {
  if (RegexStaticTable::find(name) == RegexStatic::ESCAPE) {
    return ErrorTextRegExpStatic::escape();
  }
  bool isLegacy = RegexFlagTable::isLegacyStatic(name);
  if (isLegacy) {
    return ErrorTextRegExpStatic::legacy(name);
  }

  return ErrorTextRegExpStatic::noMember(name);
}

std::string LibraryRegexRegistry::escapedLiteral(const std::u16string &text) const {
  auto escaped = std::u16string();
  for (char16_t unit : text) {
    if (unit < 128 && std::strchr(kRegexSpecials, static_cast<int>(unit)) != nullptr) {
      escaped += u'\\';
    }
    escaped += unit;
  }

  return "/" + ToUtf8::toUtf8(escaped) + "/";
}

std::string LibraryRegexRegistry::argumentText(const Expr *arg) const {
  const auto *literal = dyn_cast<StringLitExpr>(arg);
  if (literal != nullptr) {
    auto text = ToUtf8::toUtf8(literal->value);
    auto isPlain = std::bind_front(&LibraryRegexRegistry::isPlainQuotedChar, this);
    if (llvm::all_of(text, isPlain)) {
      return "\"" + text + "\"";
    }
  }
  const auto *name = dyn_cast<NameExpr>(arg);
  if (name != nullptr) {
    return name->name.str();
  }

  return kStringPlaceholder.str();
}

bool LibraryRegexRegistry::isPlainQuotedChar(char character) const {
  return character >= 0x20 && character != '"' && character != '\\';
}

unsigned LibraryRegexRegistry::patternMinArgs(StringMethod method) const {
  if (method == StringMethod::REPLACE || method == StringMethod::REPLACE_ALL) {
    return 2;
  }
  if (method == StringMethod::SPLIT) {
    return 0;
  }

  return 1;
}

bool LibraryRegexRegistry::isMissingGlobalFlag(const RegexLitExpr *regex) const {
  if (regex == nullptr) {
    return false;
  }
  uint32_t flags = tz_re_flags(regex->program.data());

  return (flags & TZ_RE_GLOBAL) == 0;
}

bool LibraryRegexRegistry::hasOptionalGroups(const RegexLitExpr *regex) const {
  if (regex == nullptr) {
    return true;
  }
  const uint32_t *program = regex->program.data();
  uint32_t captures = tz_re_captures(program);
  for (uint32_t group = 1; group < captures; ++group) {
    if (!tz_re_mandatory(program, group)) {
      return true;
    }
  }

  return false;
}

bool LibraryRegexRegistry::isFunctionReplacement(const Expr *replacement) const {
  auto *name = dyn_cast<NameExpr>(replacement);
  if (name == nullptr) {
    return isa<FunctionExpr>(replacement);
  }
  auto entry = this->state.walk.scopes.findName(name->name);
  const VarSymbol *variable = entry.var();
  bool isFunctionVar = variable != nullptr && variable->type != nullptr &&
                       variable->type->nonOptional()->isFunction();

  return entry.func() != nullptr || isFunctionVar;
}

LibraryRegexRegistry::ResultGroupsReferenced
LibraryRegexRegistry::checkReplacementGroups(const RegexLitExpr *regex,
                                             const std::u16string &value) const {
  const uint32_t *program = regex->program.data();
  uint32_t groups = tz_re_captures(program) - 1;
  for (size_t at : this->groupReferences(value)) {
    auto reference = this->checkGroupReference(program, groups, value, at);
    if (reference.isErr()) {
      return err(reference.error());
    }
  }

  return ok(&value);
}

std::vector<size_t> LibraryRegexRegistry::groupReferences(const std::u16string &value) const {
  auto positions = std::vector<size_t>();
  size_t index = 0;
  while (index + 1 < value.size()) {
    bool isDollar = value[index] == u'$';
    bool isEscaped = isDollar && value[index + 1] == u'$';
    if (isDollar && !isEscaped) {
      positions.push_back(index);
    }
    index += isEscaped ? 2 : 1;
  }

  return positions;
}

LibraryRegexRegistry::ResultGroupsReferenced
LibraryRegexRegistry::checkGroupReference(const uint32_t *program, uint32_t groups,
                                          const std::u16string &value, size_t at) const {
  char16_t next = value[at + 1];
  bool isNumbered = next >= u'1' && next <= u'9';
  if (isNumbered && !this->isNumberedGroupKnown(value, at, groups)) {
    uint32_t digit = next - u'0';
    auto message = ErrorTextStringRegex::noGroup(groups, digit);

    return err(LibraryRegexRegistryReplacementGroupsError{
        .code = LibraryRegexRegistryReplacementGroupsErrorCode::NO_SUCH_GROUP,
        .message = message,
    });
  }
  if (next != u'<') {
    return ok(&value);
  }
  auto groupName = this->groupNameAt(value, at);
  if (!this->hasNamedGroup(program, groups, groupName)) {
    bool isNamed = this->hasNamedGroups(program, groups);
    auto message = ErrorTextStringRegex::noNamed(isNamed, groupName);

    return err(LibraryRegexRegistryReplacementGroupsError{
        .code = LibraryRegexRegistryReplacementGroupsErrorCode::NO_SUCH_NAMED_GROUP,
        .message = message,
    });
  }

  return ok(&value);
}

bool LibraryRegexRegistry::isNumberedGroupKnown(const std::u16string &value, size_t at,
                                                uint32_t groups) const {
  uint32_t one = value[at + 1] - u'0';
  char16_t next = at + 2 < value.size() ? value[at + 2] : 0;
  uint32_t two = next >= u'0' && next <= u'9' ? one * 10 + (next - u'0') : 0;

  return (two != 0 && two <= groups) || one <= groups;
}

std::string LibraryRegexRegistry::groupNameAt(const std::u16string &value, size_t at) const {
  size_t close = value.find(u'>', at + 2);
  if (close == std::u16string::npos) {
    return "";
  }

  return ToUtf8::toUtf8(value.substr(at + 2, close - at - 2));
}

bool LibraryRegexRegistry::hasNamedGroup(const uint32_t *program, uint32_t groups,
                                         const std::string &groupName) const {
  for (uint32_t group = 1; group <= groups; ++group) {
    uint32_t start = 0;
    uint32_t length = tz_re_name(program, group, &start);
    if (length != 0 && this->groupName(program, start, length) == groupName) {
      return true;
    }
  }

  return false;
}

std::string LibraryRegexRegistry::groupName(const uint32_t *program, uint32_t start,
                                            uint32_t length) const {
  auto units = this->units(program, start, length);

  return ToUtf8::toUtf8(units);
}

bool LibraryRegexRegistry::hasNamedGroups(const uint32_t *program, uint32_t groups) const {
  for (uint32_t group = 1; group <= groups; ++group) {
    uint32_t start = 0;
    if (tz_re_name(program, group, &start) != 0) {
      return true;
    }
  }

  return false;
}

void LibraryRegexRegistry::compileRegexLit(RegexLitExpr *literal) {
  literal->type = Type::regExp();
  llvm::StringRef source(literal->loc().getPointer(), literal->written);
  auto parsed = this->checkParsedFlags(literal->flags);
  if (parsed.isErr()) {
    llvm::SMLoc at =
        literal->written
            ? llvm::SMLoc::getFromPointer(source.end() - ToUtf8::toUtf8(literal->flags).size())
            : literal->loc();
    this->state.diagnostics.error(at, parsed.error().message);
    return;
  }
  uint32_t flags = parsed.value();
  std::vector<uint16_t> pattern(literal->pattern.begin(), literal->pattern.end());
  const char *error = nullptr;
  size_t errorAt = 0;
  uint32_t *program =
      tz_re_compile(pattern.data(), pattern.size(), flags | TZ_RE_STRICT, &error, &errorAt);
  if (program == nullptr) {
    llvm::SMLoc at = literal->loc();
    if (literal->written) {
      const char *p = source.begin() + 1;
      for (size_t units = 0; units < errorAt && p < source.end();) {
        unsigned char lead = static_cast<unsigned char>(*p);
        size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        units += size == 4 ? 2 : 1;
        p += size;
      }
      at = llvm::SMLoc::getFromPointer(p);
    }
    auto message = ErrorTextRegexLit::invalid(error);
    this->state.diagnostics.error(at, message);
    return;
  }
  literal->program.assign(program, program + program[TZ_RE_H_SIZE]);
  tz_re_free(program);
}

}
