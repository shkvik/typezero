#include "compiler/semantic/library/json/json_static_checker/json_static_checker.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/json/json_static_table/json_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

JsonStaticChecker::JsonStaticChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->prelude = &all.prelude;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->libraryCalls = &all.libraryCalls;
  this->typeRelations = &all.typeRelations;
  this->argumentChecker = &all.argumentChecker;
  this->bufferStaticChecker = &all.bufferStaticChecker;
}

bool JsonStaticChecker::mayHoldItself(const Type *type) const {
  auto open = std::vector<const Type *>();
  std::vector<HoldStep> steps = {EnterHold{type}};
  while (!steps.empty()) {
    HoldStep step = steps.back();
    steps.pop_back();
    if (step.is<LeaveHold>()) {
      open.pop_back();
    } else if (this->enterHold(step.as<EnterHold>()->type, open, steps)) {
      return true;
    }
  }

  return false;
}

bool JsonStaticChecker::enterHold(const Type *type, std::vector<const Type *> &open,
                                  std::vector<HoldStep> &steps) const {
  const Type *plain = type->nonOptional();
  if (plain->isParam() || plain->isAnyObject()) {
    return false;
  }
  if (plain->isJson() || (plain->isBase() && plain->classDecl() == nullptr) || plain->isUnknown() ||
      llvm::is_contained(open, plain)) {
    return true;
  }
  open.push_back(plain);
  steps.push_back(LeaveHold{});
  auto parts = this->heldParts(plain);
  for (const Type *part : llvm::reverse(parts)) {
    steps.push_back(EnterHold{part});
  }

  return false;
}

std::vector<const Type *> JsonStaticChecker::heldParts(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl != nullptr) {
    return this->familyFields(classDecl);
  }
  if (type->isArray()) {
    return {type->inner()};
  }
  if (type->isResult()) {
    return {type->okType(), type->errType()};
  }
  if (type->isDict()) {
    return {type->valueType()};
  }
  if (type->isObject()) {
    return this->fieldTypes(type);
  }
  if (type->isUnion()) {
    return std::vector<const Type *>(type->members().begin(), type->members().end());
  }

  return {};
}

std::vector<const Type *> JsonStaticChecker::familyFields(const ClassDecl *classDecl) const {
  std::vector<const ClassDecl *> family{classDecl};
  for (size_t index = 0; index < family.size(); ++index) {
    family.insert(family.end(), family[index]->children.begin(), family[index]->children.end());
  }
  auto parts = std::vector<const Type *>();
  for (const ClassDecl *member : family) {
    if (member->type != nullptr) {
      auto fields = this->fieldTypes(member->type);
      parts.insert(parts.end(), fields.begin(), fields.end());
    }
  }

  return parts;
}

std::vector<const Type *> JsonStaticChecker::fieldTypes(const Type *type) const {
  auto types = std::vector<const Type *>();
  for (const Field &field : type->fields()) {
    types.push_back(field.type);
  }

  return types;
}

const ClassDecl *JsonStaticChecker::classWithToJson(const Type *type) const {
  auto parts = TypeParts::collect(type, kToJsonRoute);
  for (const Type *part : parts.getTypes()) {
    const ClassDecl *classDecl = part->classDecl();
    if (classDecl != nullptr && this->hasToJson(classDecl)) {
      return classDecl;
    }
  }

  return nullptr;
}

bool JsonStaticChecker::hasToJson(const ClassDecl *classDecl) const {
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    if (functionDecl->name == "toJSON" && !functionDecl->isStatic()) {
      return true;
    }
  }

  return false;
}

bool JsonStaticChecker::mayStringifyToUndefined(const Type *type) const {
  return type->isUndefined() || type->isOptional() || type->isFunction() || type->isNever() ||
         type->isUnknown();
}

Expr *JsonStaticChecker::checkJsonStatic(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }
  auto fn = JsonStaticTable::find(member->name);
  if (fn == JsonStatic::PARSE) {
    return this->checkJsonParse(call, member);
  }
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextJsonStatic::typeArgs(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
  }
  if (fn != JsonStatic::STRINGIFY) {
    auto message = ErrorTextJsonStatic::noMember(member->name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }

  return this->checkStringify(call, member);
}

void JsonStaticChecker::walkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    if (!this->jsonChecker->isNullName(arg)) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }
}

Expr *JsonStaticChecker::checkJsonParse(CallExpr *call, MemberExpr *member) {
  if (call->args.size() == 2) {
    auto message = ErrorTextJsonStatic::reviver();
    this->state.getDiagnostics().error(call->args[1]->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextJsonStatic::parseTypes();
    this->state.getDiagnostics().error(member->loc(), message);
    this->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->argumentChecker->checkArg(call->args[0], Type::string());
  this->walkConstantJson(call->args[0]);
  call->target = BuiltinCall{.builtin = Builtin::JsonParse};
  call->type = Type::result(Type::json(), this->prelude->builtinType(PreludeType::JSON_ERROR));

  return call;
}

Expr *JsonStaticChecker::checkStringify(CallExpr *call, MemberExpr *member) {
  auto arity = this->argumentChecker->checkArity(call, 1, 3);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  call->args[0] = this->exprChecker->checkExpr(call->args[0]);
  Expr *value = call->args[0];
  bool isOk = this->stringifiedValueFits(value);
  std::vector<Expr *> args{value};
  isOk &= this->stringifyReplacerFits(call);
  isOk &= this->stringifySpaceFits(call, args);
  if (!isOk) {
    return call;
  }
  call->args = std::move(args);
  call->target = BuiltinCall{.builtin = Builtin::JsonStringify};
  call->type =
      this->mayStringifyToUndefined(value->type) ? Type::optional(Type::string()) : Type::string();
  bool isCyclic = !this->libraryCalls->inLibrary(call->loc()) && this->mayHoldItself(value->type);
  if (isCyclic) {
    call->type = Type::result(call->type, this->prelude->builtinType(PreludeType::JSON_ERROR));
  }

  return call;
}

bool JsonStaticChecker::stringifiedValueFits(const Expr *value) {
  bool isOk = !value->type->isError();
  if (value->type->isVoid()) {
    auto message = ErrorTextJsonStatic::voidValue();
    this->state.getDiagnostics().error(value->loc(), message);
    isOk = false;
  }
  const ClassDecl *classDecl = isOk ? this->classWithToJson(value->type) : nullptr;
  if (classDecl != nullptr) {
    auto message = ErrorTextJsonStatic::toJson(classDecl->name);
    this->state.getDiagnostics().error(value->loc(), message);
    isOk = false;
  }

  return isOk;
}

bool JsonStaticChecker::stringifyReplacerFits(CallExpr *call) {
  if (call->args.size() < 2) {
    return true;
  }
  Expr *replacer = call->args[1];
  if (this->jsonChecker->isNullName(replacer) || isa<UndefinedExpr>(replacer)) {
    return true;
  }
  replacer = this->exprChecker->checkExpr(replacer);
  if (!replacer->type->isError()) {
    auto message = ErrorTextJsonStatic::replacer();
    this->state.getDiagnostics().error(replacer->loc(), message);
  }

  return false;
}

bool JsonStaticChecker::stringifySpaceFits(CallExpr *call, std::vector<Expr *> &args) {
  if (call->args.size() != 3) {
    return true;
  }
  Expr *space = call->args[2];
  if (this->jsonChecker->isNullName(space) || isa<UndefinedExpr>(space)) {
    return true;
  }
  space = this->exprChecker->checkExpr(space);
  const Type *type = space->type;
  if (type->isNumeric()) {
    args.push_back(this->typeRelations->convert(space, Type::doubleTy()));
    return true;
  }
  if (type->isString()) {
    args.push_back(space);
    return true;
  }
  if (type->isError()) {
    return true;
  }
  auto message = ErrorTextJsonStatic::space(type->name());
  this->state.getDiagnostics().error(space->loc(), message);

  return false;
}

bool JsonStaticChecker::readEmbedded(CallExpr *call, llvm::StringRef functionName,
                                     std::string &bytes, std::string &shown, std::string &full) {
  Expr *arg = call->args[0];
  auto example = ErrorTextEmbed::example(functionName);
  auto path = std::u16string();
  auto *literal = dyn_cast<StringLitExpr>(arg);
  if (literal != nullptr) {
    path = literal->value;
  } else if (auto *templateExpr = dyn_cast<TemplateExpr>(arg);
             templateExpr != nullptr && templateExpr->exprs.empty()) {
    path = templateExpr->quasis[0];
  } else {
    call->args[0] = this->exprChecker->checkExpr(arg);
    auto message = ErrorTextEmbed::literal(functionName, example);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  shown = ToUtf8::toUtf8(path);
  auto relative = this->embeddedPath(functionName, shown);
  if (relative.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), relative.error().message);
    return false;
  }
  llvm::SourceMgr &sources = this->state.getDiagnostics().sourceMgr();
  unsigned id = sources.FindBufferContainingLoc(call->loc());
  llvm::StringRef source = id ? sources.getMemoryBuffer(id)->getBufferIdentifier() : "";
  llvm::SmallString<256> path8(llvm::sys::path::parent_path(source));
  llvm::sys::path::append(path8, shown);
  llvm::sys::path::remove_dots(path8, true);
  llvm::sys::path::native(path8);
  full = std::string(path8);
  auto buffer = llvm::MemoryBuffer::getFile(full, false, false);
  if (!buffer) {
    std::string why = buffer.getError().message();
    auto message = ErrorTextEmbed::unread(shown, why);
    this->state.getDiagnostics().error(arg->loc(), message);
    return false;
  }
  bytes = (*buffer)->getBuffer().str();

  return true;
}

void JsonStaticChecker::walkConstantJson(const Expr *arg) {
  while (auto *conversion = llvm::dyn_cast<CastExpr>(arg)) {
    if (!conversion->isImplicit()) {
      return;
    }
    arg = conversion->operand;
  }
  auto *literal = llvm::dyn_cast<StringLitExpr>(arg);
  if (literal == nullptr) {
    return;
  }
  auto text = ToUtf8::toUtf8(literal->value);
  auto json = this->constantJson(text);
  if (json.isErr()) {
    this->state.getDiagnostics().error(literal->loc(), json.error().message);
  }
}

JsonStaticChecker::ResultEmbeddedPath JsonStaticChecker::embeddedPath(llvm::StringRef functionName,
                                                                      llvm::StringRef shown) const {
  if (shown.contains('\\')) {
    auto message = ErrorTextEmbed::backslash();

    return err(JsonStaticCheckerEmbeddedPathError{
        .code = JsonStaticCheckerEmbeddedPathErrorCode::BACKSLASH,
        .message = message,
    });
  }
  if (shown.empty() || llvm::sys::path::is_absolute(shown) || shown[0] == '/') {
    auto example = ErrorTextEmbed::example(functionName);
    auto message = ErrorTextEmbed::relative(functionName, example);

    return err(JsonStaticCheckerEmbeddedPathError{
        .code = JsonStaticCheckerEmbeddedPathErrorCode::NOT_RELATIVE,
        .message = message,
    });
  }

  return ok(shown);
}

JsonStaticChecker::ResultConstantJson JsonStaticChecker::constantJson(llvm::StringRef text) const {
  if (text.starts_with("\xEF\xBB\xBF")) {
    auto message = ErrorTextConstantJson::bom();

    return err(JsonStaticCheckerConstantJsonError{
        .code = JsonStaticCheckerConstantJsonErrorCode::BYTE_ORDER_MARK,
        .message = message,
    });
  }
  pkg::JsonProblem problem = pkg::JsonText::checkJsonText(text);
  if (problem.message.empty()) {
    return ok(text);
  }
  size_t line = 1;
  size_t column = 1;
  for (size_t index = 0; index < problem.offset && index < text.size(); ++index) {
    if (text[index] == '\n') {
      ++line;
      column = 1;
    } else if ((static_cast<unsigned char>(text[index]) & 0xC0) != 0x80) {
      ++column;
    }
  }
  auto message = ErrorTextConstantJson::invalid(problem.message, line, column);

  return err(JsonStaticCheckerConstantJsonError{
      .code = JsonStaticCheckerConstantJsonErrorCode::INVALID,
      .message = message,
  });
}

Expr *JsonStaticChecker::checkJsonGlobal(CallExpr *call, NameExpr *name) {
  bool isEmbedingText = this->callChecker->isGlobalName(name, GlobalName::EMBED_TEXT);
  bool isEmbedingBytes = this->callChecker->isGlobalName(name, GlobalName::EMBED_BYTES);
  if (!isEmbedingText && !isEmbedingBytes) {
    return nullptr;
  }
  llvm::StringRef functionName = name->name;
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
  };
  if (this->callChecker->rejectSpread(call)) {
    return call;
  }

  if (!name->typeArgs.empty()) {
    auto message = ErrorTextEmbed::typeArgs(functionName);
    this->state.getDiagnostics().error(name->loc(), message);
  }

  if (isEmbedingBytes) {
    return this->bufferStaticChecker->checkEmbedBytes(call, name);
  }
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(name->loc(), arity.error().message);
    return call;
  }
  std::string bytes, shown, full;
  if (!this->readEmbedded(call, functionName, bytes, shown, full)) {
    return call;
  }
  llvm::StringRef text(bytes);
  auto units = llvm::SmallVector<llvm::UTF16, 64>();
  if (!llvm::convertUTF8ToUTF16String(text, units)) {
    auto message = ErrorTextEmbed::notUtf8(shown);
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return call;
  }
  std::u16string content(units.begin(), units.end());

  auto *literal = this->state.getAst().make<StringLitExpr>(call->loc(), std::move(content));
  literal->type = Type::string();

  return literal;
}

}
