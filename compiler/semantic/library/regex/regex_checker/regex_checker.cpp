#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/regex/regex_member_table/regex_member_table.h"
#include "compiler/program/library/regex/regex_static_table/regex_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::dyn_cast;

namespace tz {

namespace {

struct RegexFlag {
  llvm::StringLiteral name;
  uint32_t bit;
};

const RegexFlag kFlags[] = {
    {.name = "hasIndices", .bit = TZ_RE_HAS_INDICES},
    {.name = "global", .bit = TZ_RE_GLOBAL},
    {.name = "ignoreCase", .bit = TZ_RE_IGNORE_CASE},
    {.name = "multiline", .bit = TZ_RE_MULTILINE},
    {.name = "dotAll", .bit = TZ_RE_DOT_ALL},
    {.name = "unicode", .bit = TZ_RE_UNICODE},
    {.name = "unicodeSets", .bit = TZ_RE_UNICODE_SETS},
    {.name = "sticky", .bit = TZ_RE_STICKY},
};

const llvm::StringLiteral kLegacyStatics[] = {"input", "lastMatch", "lastParen", "leftContext",
                                              "rightContext"};

}

RegexChecker::RegexChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->varChecker = &all.varChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->objectChecker = &all.objectChecker;
  this->typeRelations = &all.typeRelations;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

std::u16string RegexChecker::units(const uint32_t *program, uint32_t start, uint32_t length) const {
  auto text = std::u16string();
  for (uint32_t index = 0; index < length; ++index) {
    text += static_cast<char16_t>(tz_re_name_unit(program, start, index));
  }

  return text;
}

std::optional<uint32_t> RegexChecker::flagBit(llvm::StringRef name) const {
  for (const RegexFlag &flag : kFlags) {
    if (flag.name == name) {
      return flag.bit;
    }
  }

  return std::nullopt;
}

RegexChecker::ResultParsedFlags RegexChecker::checkParsedFlags(const std::u16string &flags) const {
  std::vector<uint16_t> flagUnits(flags.begin(), flags.end());
  uint32_t parsed = 0;
  if (!tz_re_parse_flags(flagUnits.data(), flagUnits.size(), &parsed)) {
    auto written = ToUtf8::toUtf8(flags);
    auto message = ErrorTextRegexFlags::invalid(written);

    return err(RegexCheckerParsedFlagsError{
        .code = RegexCheckerParsedFlagsErrorCode::INVALID_FLAGS,
        .message = message,
    });
  }

  return ok(parsed);
}

Expr *RegexChecker::checkRegexLit(RegexLitExpr *literal) {
  literal->type = Type::regExp();
  llvm::StringRef source(literal->loc().getPointer(), literal->written);
  auto parsed = this->checkParsedFlags(literal->flags);
  if (parsed.isErr()) {
    llvm::SMLoc at =
        literal->written
            ? llvm::SMLoc::getFromPointer(source.end() - ToUtf8::toUtf8(literal->flags).size())
            : literal->loc();
    this->state.getDiagnostics().error(at, parsed.error().message);
    return literal;
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
    this->state.getDiagnostics().error(at, message);
    return literal;
  }
  literal->program.assign(program, program + program[TZ_RE_H_SIZE]);
  tz_re_free(program);

  return literal;
}

const RegexLitExpr *RegexChecker::knownRegex(const Expr *expr) const {
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

void RegexChecker::noteRegexVariable(VarSymbol *variable, const Expr *init, bool isConst,
                                     llvm::ArrayRef<Stmt *> block, size_t index) {
  auto *literal = llvm::dyn_cast_or_null<RegexLitExpr>(init);
  if (literal == nullptr || literal->program.empty()) {
    return;
  }
  if (!isConst && !this->isStableRegexName(variable->name, block, index)) {
    return;
  }
  variable->holds = RegexLiteral{.regex = literal};
}

bool RegexChecker::isStableRegexName(llvm::StringRef name, llvm::ArrayRef<Stmt *> block,
                                     size_t index) const {
  if (block.empty()) {
    return false;
  }
  auto following = this->varChecker->followingStatements(block, index);

  return !this->isAssigningName(following, name);
}

bool RegexChecker::isAssigningName(llvm::ArrayRef<const Stmt *> stmts, llvm::StringRef name) const {
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

const Type *RegexChecker::matchType(const RegexLitExpr *regex) {
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

void RegexChecker::addNamedGroup(std::vector<Field> &fields, const uint32_t *program,
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

bool RegexChecker::hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

Expr *RegexChecker::regexToString(Expr *regex) {
  auto stringName = GlobalNameTable::getName(GlobalName::STRING);
  auto *callee = this->state.getAst().make<NameExpr>(regex->loc(), stringName);

  return this->processChecker->builtinCall(regex->loc(), callee, Builtin::RegExpToString, {regex},
                                           Type::string());
}

Expr *RegexChecker::checkRegexMethod(CallExpr *call, MemberExpr *member) {
  const auto *regex = this->knownRegex(member->object);
  call->regex = regex;
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
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
  this->state.getDiagnostics().error(member->loc(), message);
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *RegexChecker::checkRegexRun(CallExpr *call, MemberExpr *member, const RegexLitExpr *regex,
                                  RegexMember method) {
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  uint32_t flags = regex != nullptr ? tz_re_flags(regex->program.data()) : 0;
  bool isTest = method == RegexMember::TEST;
  if (isTest && (flags & TZ_RE_GLOBAL) != 0) {
    auto message = ErrorTextRegexMethod::testG();
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  if (isTest) {
    call->target = BuiltinCall{.builtin = Builtin::RegExpTest};
    call->type = Type::boolTy();
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::RegExpExec};
  const auto *match = this->matchType(regex);
  call->type = Type::optional(match);

  return call;
}

Expr *RegexChecker::checkRegexToString(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  auto *text = this->regexToString(member->object);
  call->args.push_back(text);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return call;
}

Expr *RegexChecker::checkRegexMember(MemberExpr *member) {
  llvm::StringRef name = member->name;
  auto known = RegexMemberTable::find(name);
  if (known == RegexMember::LAST_INDEX) {
    auto field = this->objectChecker->checkField(member);
    if (field.isErr()) {
      this->state.getDiagnostics().error(member->loc(), field.error().message);
      return member;
    }
    return field.value();
  }
  if (known == RegexMember::SOURCE) {
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpSource, {},
                                             Type::string());
  }
  if (known == RegexMember::FLAGS) {
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpFlags, {},
                                             Type::string());
  }
  auto flag = this->flagBit(name);
  if (flag.has_value()) {
    auto *bit = this->state.getAst().make<IntLitExpr>(member->loc(), flag.value());
    bit->type = Type::intTy();
    return this->processChecker->builtinCall(member->loc(), member, Builtin::RegExpFlag, {bit},
                                             Type::boolTy());
  }
  bool isMethod = known == RegexMember::TEST || known == RegexMember::EXEC;
  auto message = ErrorTextRegexMethod::notCalled(name, isMethod);
  this->state.getDiagnostics().error(member->loc(), message);
  member->type = Type::error();

  return member;
}

Expr *RegexChecker::checkRegExpConstructor(llvm::SMLoc loc, std::vector<Expr *> &args) {
  if (args.empty() || args.size() > 2) {
    auto message = ErrorTextRegExpNew::arity(args.size());
    this->state.getDiagnostics().error(loc, message);
    return this->failRegExpNew(loc, args);
  }
  std::optional<std::u16string> pattern = this->constantText(args[0]);
  std::optional<std::u16string> flags =
      args.size() > 1 ? this->constantText(args[1]) : std::u16string();
  if (pattern.has_value() && flags.has_value()) {
    auto *literalExpr = this->state.getAst().make<RegexLitExpr>(loc, *pattern, *flags);
    return this->checkRegexLit(literalExpr);
  }
  if (!this->isRegExpEscape(args[0]) || !flags.has_value()) {
    auto message = ErrorTextRegExpNew::unknown(args.size() > 1);
    this->state.getDiagnostics().error(loc, message);
    return this->failRegExpNew(loc, args);
  }
  auto parsed = this->checkParsedFlags(*flags);
  if (parsed.isErr()) {
    this->state.getDiagnostics().error(args[1]->loc(), parsed.error().message);
    return this->failRegExpNew(loc, args);
  }
  args[0] = this->exprChecker->checkExpr(args[0]);
  args[0] = this->typeRelations->coerce(args[0], Type::string());
  if (args.size() > 1) {
    args[1] = this->argumentChecker->checkArg(args[1], Type::string());
  }
  auto regExpName = GlobalNameTable::getName(GlobalName::REG_EXP);
  auto *callee = this->state.getAst().make<NameExpr>(loc, regExpName);

  return this->processChecker->builtinCall(loc, callee, Builtin::RegExpNew, args, Type::regExp());
}

Expr *RegexChecker::failRegExpNew(llvm::SMLoc loc, std::vector<Expr *> &args) {
  for (Expr *&arg : args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  auto *error = this->state.getAst().make<UndefinedExpr>(loc);
  error->type = Type::error();

  return error;
}

std::optional<std::u16string> RegexChecker::constantText(const Expr *expr) const {
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

bool RegexChecker::isRegExpEscape(const Expr *expr) const {
  const auto *escaped = dyn_cast<CallExpr>(expr);
  const auto *escape = escaped != nullptr ? dyn_cast<MemberExpr>(escaped->callee) : nullptr;
  if (escape == nullptr) {
    return false;
  }
  auto function = RegexStaticTable::find(escape->name);

  return function == RegexStatic::ESCAPE &&
         this->callChecker->isGlobalName(escape->object, GlobalName::REG_EXP);
}

Expr *RegexChecker::checkRegExpStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto function = RegexStaticTable::find(member->name);
  if (function == RegexStatic::PARSE) {
    bool isRejected = this->callChecker->rejectSpread(call);
    auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
    }
    if (isRejected || arity.isErr()) {
      return call;
    }
    for (Expr *&arg : call->args) {
      arg = this->argumentChecker->checkArg(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::RegExpParse};
    const auto *regexError = this->prelude->builtinType(PreludeType::REGEX_ERROR);
    call->type = Type::result(Type::regExp(), regexError);
    return call;
  }
  if (function != RegexStatic::ESCAPE) {
    this->checkRegExpStaticMember(member);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  call->target = BuiltinCall{.builtin = Builtin::RegExpEscape};
  call->type = Type::string();

  return call;
}

Expr *RegexChecker::checkRegExpStaticMember(MemberExpr *member) {
  member->type = Type::error();
  auto message = this->staticMemberText(member->name);
  this->state.getDiagnostics().error(member->loc(), message);

  return member;
}

std::string RegexChecker::staticMemberText(llvm::StringRef name) const {
  if (RegexStaticTable::find(name) == RegexStatic::ESCAPE) {
    return ErrorTextRegExpStatic::escape();
  }
  bool isLegacy = name.starts_with("$") || llvm::is_contained(kLegacyStatics, name);
  if (isLegacy) {
    return ErrorTextRegExpStatic::legacy(name);
  }

  return ErrorTextRegExpStatic::noMember(name);
}

}
