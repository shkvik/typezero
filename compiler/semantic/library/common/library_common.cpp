#include "compiler/semantic/library/common/library_common.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/declaration/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

LibraryCommon::LibraryCommon(SemanticState &owner)
    : state(owner) {}

bool LibraryCommon::hasSpread(const CallExpr *call) const {
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (call->spreads(index)) {
      return true;
    }
  }

  return false;
}

bool LibraryCommon::isProcessMember(const Expr *expr, ProcessMember which) const {
  const auto *member = dyn_cast<MemberExpr>(expr);
  if (member == nullptr || member->optional) {
    return false;
  }
  if (member->name != ProcessMemberTable::getName(which)) {
    return false;
  }

  return this->state.isGlobalName(member->object, GlobalName::PROCESS);
}

LibraryCommon::ResultSpreadIsFree LibraryCommon::checkSpreadFree(const CallExpr *call,
                                                                 size_t index) const {
  if (call->spreads(index)) {
    auto message = ErrorTextSpread::notRest();

    return err(LibraryCommonSpreadFreeError{
        .code = LibraryCommonSpreadFreeErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok(call);
}

LibraryCommon::ResultArityMatches LibraryCommon::checkArity(const CallExpr *call, unsigned min,
                                                            unsigned max) const {
  size_t count = call->args.size();
  bool isFitting = count >= min && count <= max;
  if (!isFitting && call->tagged) {
    auto message = ErrorTextArity::tagged(min, max, count);

    return err(LibraryCommonArityError{
        .code = LibraryCommonArityErrorCode::WRONG_COUNT_TAGGED,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextArity::count(min, max, count);

    return err(LibraryCommonArityError{
        .code = LibraryCommonArityErrorCode::WRONG_COUNT,
        .message = message,
    });
  }

  return ok(call);
}

LibraryCommon::ResultKnownEncoding LibraryCommon::checkKnownEncoding(llvm::StringRef name) const {
  auto encoding = EncodingNameTable::find(name);
  if (!encoding.has_value()) {
    auto message = ErrorTextBufferEncoding::unknown(name);

    return err(LibraryCommonKnownEncodingError{
        .code = LibraryCommonKnownEncodingErrorCode::UNKNOWN_ENCODING,
        .message = message,
    });
  }

  return ok(int(*encoding));
}

Expr *LibraryCommon::findNodeOption(const CallExpr *call, size_t index, NodeOption option) const {
  if (index >= call->args.size()) {
    return nullptr;
  }
  const auto *literal = dyn_cast<ObjectLitExpr>(call->args[index]);
  if (literal == nullptr) {
    return nullptr;
  }
  for (const ObjectLitExpr::Property &property : llvm::reverse(literal->props)) {
    if (NodeOptionTable::find(property.name) == option) {
      return property.value;
    }
  }

  return nullptr;
}

std::optional<std::vector<std::string>> LibraryCommon::knownTexts(const Expr *arg) const {
  auto texts = std::vector<std::string>();
  auto stack = std::vector<const Expr *>{arg};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    auto lookup = this->lookupTexts(next);
    if (lookup.choice != nullptr) {
      stack.push_back(lookup.choice->elseExpr);
      stack.push_back(lookup.choice->thenExpr);
    } else if (lookup.texts.has_value()) {
      texts.insert(texts.end(), lookup.texts->begin(), lookup.texts->end());
    } else {
      return std::nullopt;
    }
  }

  return texts;
}

LibraryCommon::KnownTextLookup LibraryCommon::lookupTexts(const Expr *arg) const {
  auto typed = this->typedTexts(arg);
  if (typed.has_value()) {
    return KnownTextLookup{.texts = typed, .choice = nullptr};
  }
  const Expr *bare = ExprParts::withoutImplicitCasts(arg);
  auto narrowed = this->narrowedTexts(bare);
  if (narrowed.has_value()) {
    return KnownTextLookup{.texts = narrowed, .choice = nullptr};
  }
  const Expr *written = this->writtenValue(bare);
  const auto *choice = dyn_cast<ConditionalExpr>(written);
  if (choice != nullptr) {
    return KnownTextLookup{.texts = std::nullopt, .choice = choice};
  }
  auto text = this->writtenText(written);
  if (text.has_value()) {
    auto texts = std::vector<std::string>{ToUtf8::toUtf8(*text)};

    return KnownTextLookup{.texts = texts, .choice = nullptr};
  }

  return KnownTextLookup{.texts = this->typeTexts(written->type), .choice = nullptr};
}

std::optional<std::vector<std::string>> LibraryCommon::typedTexts(const Expr *arg) const {
  const Expr *expr = arg;
  auto texts = this->typeTexts(expr->type);
  const auto *conversion = dyn_cast<CastExpr>(expr);
  while (!texts.has_value() && conversion != nullptr && conversion->isImplicit()) {
    expr = conversion->operand;
    texts = this->typeTexts(expr->type);
    conversion = dyn_cast<CastExpr>(expr);
  }

  return texts;
}

std::optional<std::vector<std::string>> LibraryCommon::narrowedTexts(const Expr *bare) const {
  const VarSymbol *variable = this->namedVariable(bare);
  if (variable == nullptr) {
    return std::nullopt;
  }
  const Type *flowType = this->state.flow.tracked.typeIn(this->state.walk.flow.getFlow(), variable);

  return this->typeTexts(flowType);
}

const Expr *LibraryCommon::writtenValue(const Expr *bare) const {
  const VarSymbol *variable = this->namedVariable(bare);
  if (variable == nullptr) {
    return bare;
  }
  const auto *literal = this->state.code.constants.findConstLiteral(variable);
  if (literal == nullptr) {
    return bare;
  }

  return literal;
}

const VarSymbol *LibraryCommon::namedVariable(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);

  return entry.var();
}

std::optional<std::u16string> LibraryCommon::writtenText(const Expr *expr) const {
  const auto *literal = dyn_cast<StringLitExpr>(expr);
  const auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

std::optional<std::vector<std::string>> LibraryCommon::typeTexts(const Type *type) const {
  if (type == nullptr) {
    return std::nullopt;
  }
  const Type *plain = type->nonOptional();
  auto members = plain->isUnion() ? plain->members() : llvm::ArrayRef<const Type *>(plain);
  auto texts = std::vector<std::string>();
  for (const Type *member : members) {
    if (!member->isStringLiteral()) {
      return std::nullopt;
    }
    texts.push_back(ToUtf8::toUtf8(member->literalText()));
  }

  return texts;
}

FunctionDecl *LibraryCommon::soleImplementation(const ClassDecl *classDecl,
                                                FunctionDecl *variant) const {
  if (variant->openToLate()) {
    return nullptr;
  }
  if (!variant->overridden() && !variant->isAbstract()) {
    return variant;
  }
  if (variant->generic != nullptr && variant->overridden()) {
    return nullptr;
  }
  auto implementations = this->implementationsBelow(classDecl, variant);
  FunctionDecl *only = variant->isAbstract() ? nullptr : variant;
  unsigned count = only != nullptr ? 1 : 0;
  for (const OwnedMethod &implementation : implementations) {
    only = implementation.method;
    count += implementation.owner->isEmitted() ? 1 : 2;
  }

  return count == 1 ? only : nullptr;
}

std::vector<LibraryCommon::OwnedMethod>
LibraryCommon::implementationsBelow(const ClassDecl *classDecl, const FunctionDecl *variant) const {
  auto implementations = std::vector<OwnedMethod>();
  if (classDecl == nullptr) {
    return implementations;
  }
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    this->addImplementations(child, variant, implementations);
  }

  return implementations;
}

void LibraryCommon::addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                                       std::vector<OwnedMethod> &implementations) const {
  for (FunctionDecl *functionDecl : this->ownMethods(child)) {
    bool isImplementing = !functionDecl->isAbstract() && functionDecl != variant &&
                          this->isOverridingTransitively(functionDecl, variant);
    if (isImplementing) {
      implementations.push_back(OwnedMethod{.method = functionDecl, .owner = child});
    }
  }
}

bool LibraryCommon::isOverridingTransitively(const FunctionDecl *functionDecl,
                                             const FunctionDecl *base) const {
  for (const FunctionDecl *current = functionDecl; current != nullptr;
       current = current->overrides()) {
    if (current == base) {
      return true;
    }
  }

  return false;
}

std::vector<FunctionDecl *> LibraryCommon::ownMethods(const ClassDecl *classDecl) const {
  auto methods = std::vector<FunctionDecl *>();
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->isStatic()) {
      methods.push_back(functionDecl);
    }
  }
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

Expr *LibraryCommon::builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin,
                                 std::vector<Expr *> args, const Type *type) {
  auto *call = this->state.ast.make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}


FunctionDecl *LibraryCommon::freshCopy(const FunctionDecl *functionDecl) const {
  AstCloner cloner(this->state.ast);
  FunctionDecl *copy = cloner.copyFunction(functionDecl);
  copy->place = functionDecl->place;
  copy->isAsync = functionDecl->isAsync;
  this->copyMemberRole(functionDecl, copy);
  copy->sym = functionDecl->sym;
  copy->name = functionDecl->name;
  copy->typeArgs = functionDecl->typeArgs;
  copy->isInstance = functionDecl->isInstance;
  copy->parent = functionDecl->parent;
  copy->group = functionDecl->group;
  copy->var = functionDecl->var;
  copy->creationSeq = functionDecl->creationSeq;
  this->copyParams(functionDecl, copy);

  return copy;
}

void LibraryCommon::copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  const ClassMember *original = functionDecl->member();
  if (original == nullptr) {
    return;
  }
  auto copied = ClassMember{.cls = original->cls, .access = original->access};
  if (functionDecl->isStatic()) {
    copied.method = StaticMethod{};
  } else if (functionDecl->isConstructor()) {
    copied.method = Constructor{};
  }
  copy->role = std::move(copied);
}

void LibraryCommon::copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  bool hasList = !functionDecl->listName.empty() && copy->params.size() > functionDecl->listStart;
  if (hasList) {
    copy->params.resize(functionDecl->listStart);
    copy->listName = functionDecl->listName;
    copy->listStart = functionDecl->listStart;
    copy->listType = functionDecl->listType;
  }
  size_t shared = std::min(copy->params.size(), functionDecl->params.size());
  for (size_t index = 0; index < shared; ++index) {
    copy->params[index].declaredType = functionDecl->params[index].declaredType;
  }
  for (size_t index = copy->params.size(); index < functionDecl->params.size(); ++index) {
    copy->params.push_back(functionDecl->params[index]);
  }
}

std::string LibraryCommon::placeOf(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = this->state.diagnostics.sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (id == 0) {
    return "?";
  }
  auto [line, column] = sm.getLineAndColumn(loc, id);

  return sm.getMemoryBuffer(id)->getBufferIdentifier().str() + ":" + std::to_string(line) + ":" +
         std::to_string(column);
}

}
