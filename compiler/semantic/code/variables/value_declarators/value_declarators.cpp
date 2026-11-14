#include "compiler/semantic/code/variables/value_declarators/value_declarators.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ValueDeclarators::ValueDeclarators(SemanticState &owner)
    : state(owner) {}

void ValueDeclarators::rememberValueDeclarator(const RememberValueDeclarator &task) {
  auto draft = task.draft;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  VarSymbol *variable = declarator.sym;
  bool isConst = draft.decl->isConst;
  if (isConst && !declarator.destructured()) {
    this->rememberReflection(declarator, draft.value);
    this->markUnsharedArray(declarator, draft);
  }
  this->rememberConstLiteral(draft);
  if (!draft.places.empty()) {
    this->state.inference.destinations.rememberDestinations(variable, std::move(draft.places));
  }
  this->rememberResultVariable(draft);
  if (draft.isUntyped) {
    bool hasInit = declarator.init != nullptr;
    this->state.code.variables.addUntyped(variable, hasInit);
  }
  this->rememberFixedLength(draft);
  bool isInteger = isConst && declarator.init != nullptr && draft.type->isInteger();
  if (isInteger) {
    this->rememberConstNumber(draft);
    this->state.walk.tasks.pushTask(NoteUnsignedVar{.variable = variable, .init = declarator.init});
  }
  this->rememberConstText(draft);
  if (draft.value != nullptr) {
    this->state.walk.tasks.pushTask(NoteAssigned{.variable = variable, .value = draft.value});
  }

  return this->state.walk.tasks.pushTask(NoteRegexVariable{
      .variable = variable,
      .value = draft.value,
      .isConst = isConst,
      .block = draft.block,
      .blockIndex = draft.blockIndex,
  });
}

void ValueDeclarators::rememberReflection(VarDeclarator &declarator, const Expr *value) {
  auto *call = dyn_cast_or_null<CallExpr>(value);
  if (call == nullptr) {
    return;
  }
  bool isSingleMethod = call->builtin() == Builtin::ReflectMethod &&
                        !this->state.library.reflection.hasSeveralVariants(call);
  if (isSingleMethod) {
    declarator.sym->holds = ReflectedMethod{.method = call->method()->decl};
  }
  bool isReadingByKey = call->builtin() == Builtin::ReflectGet && call->args.size() == 2;
  if (isReadingByKey) {
    this->rememberReflectedNames(declarator, call->args[1]);
  }
}

void ValueDeclarators::markUnsharedArray(const VarDeclarator &declarator,
                                         const DeclaratorDraft &draft) {
  LocalVarState *localState = declarator.sym->local();
  if (localState == nullptr) {
    return;
  }
  bool isFresh = this->isFreshArray(draft.value);
  localState->unshared =
      isFresh && draft.type->isArray() &&
      this->state.flow.changes.isStayingLocal(declarator.sym->owner(), declarator.name);
}

void ValueDeclarators::rememberConstLiteral(const DeclaratorDraft &draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  const Expr *value = draft.value;
  auto &constants = this->state.code.constants;
  bool isLiteral = draft.decl->isConst && value != nullptr && this->isLiteralValue(value);
  if (isLiteral) {
    constants.rememberConstLiteral(declarator.sym, value);
    declarator.sym->literalConst = draft.declared == nullptr && draft.type->isString();
  }
  auto *alias = dyn_cast_or_null<NameExpr>(value);
  bool isLiteralAlias = alias != nullptr && alias->var() != nullptr && alias->var()->literalConst;
  bool isTextAlias =
      draft.decl->isConst && isLiteralAlias && draft.declared == nullptr && draft.type->isString();
  if (isTextAlias) {
    const auto *aliased = constants.findConstLiteral(alias->var());
    constants.rememberConstLiteral(declarator.sym, aliased);
    declarator.sym->literalConst = true;
  }
}

void ValueDeclarators::rememberResultVariable(const DeclaratorDraft &draft) {
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  const Type *held = draft.type->nonOptional();
  bool isHoldingResult = held->isResult() || (held->isPromise() && held->promised()->isResult());
  bool isTracked = !declarator.destructured() &&
                   !this->state.generics.copies.isCheckingInstance() && isHoldingResult &&
                   !this->isExported(declarator.name);
  if (isTracked) {
    this->state.code.variables.addResultVar(declarator.sym);
  }
}

void ValueDeclarators::rememberFixedLength(const DeclaratorDraft &draft) {
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  const Type *base = draft.type->nonOptional();
  bool hasBytes = base->isByteView() || base->bytes() == Type::Bytes::ArrayBuffer;
  bool isFixed =
      draft.decl->isConst && draft.value != nullptr && !declarator.destructured() && hasBytes;
  if (!isFixed) {
    return;
  }
  auto length = this->state.flow.measurements.knownLength(draft.value);
  bool isConstantLength = length.has_value() && length->term.isZero();
  if (isConstantLength) {
    this->state.flow.lengths.rememberFixedLength(declarator.sym, length->offset);
  }
}

void ValueDeclarators::rememberConstNumber(const DeclaratorDraft &draft) {
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  auto constant = this->state.code.constants.intConstant(declarator.init);
  if (!constant.has_value()) {
    return;
  }
  this->state.code.constants.rememberConstVar(declarator.sym, *constant);
  declarator.sym->literalConst = draft.declared == nullptr && draft.type->isInt() &&
                                 this->state.code.conversions.isIntLiteralTree(declarator.init);
}

void ValueDeclarators::rememberConstText(const DeclaratorDraft &draft) {
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  bool isText = draft.decl->isConst && declarator.init != nullptr && draft.type->isString();
  if (!isText) {
    return;
  }
  auto key = this->state.code.constants.constantKey(declarator.init);
  if (key.has_value()) {
    this->state.code.constants.rememberConstKey(declarator.sym, std::move(*key));
  }
  auto prefix = this->state.library.tzModules.knownPrefix(declarator.init);
  if (!prefix.empty()) {
    this->state.code.constants.rememberConstPrefix(declarator.sym, std::move(prefix));
  }
}

bool ValueDeclarators::isTextChoice(const Expr *expr) const {
  for (const Expr *branch : ExprParts::choiceBranches(expr)) {
    if (!isa<StringLitExpr>(branch)) {
      return false;
    }
  }

  return true;
}

void ValueDeclarators::rememberReflectedNames(VarDeclarator &declarator, const Expr *key) {
  auto names = this->reflectedNames(key);
  if (!names.empty()) {
    declarator.sym->holds = ReflectedNames{.names = std::move(names)};
  }
}

std::vector<llvm::StringRef> ValueDeclarators::reflectedNames(const Expr *key) {
  const auto *bare = ExprParts::withoutImplicitCasts(key);
  auto texts = this->literalKeyTexts(bare);
  auto names = std::vector<llvm::StringRef>();
  for (const std::u16string &text : texts) {
    auto utf8 = ToUtf8::toUtf8(text);
    names.push_back(this->state.ast.keepName(utf8));
  }

  return names;
}

std::vector<std::u16string> ValueDeclarators::literalKeyTexts(const Expr *key) const {
  auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal != nullptr) {
    return {literal->value};
  }
  if (key->type == nullptr) {
    return {};
  }
  const Type *keyType = key->type->nonOptional();
  auto texts = std::vector<std::u16string>();
  for (const Type *member : this->keyMembers(keyType)) {
    bool isLiteralText = member->isLiteral() && member->literalBase()->isString();
    if (!isLiteralText) {
      return {};
    }
    texts.push_back(member->literalText());
  }

  return texts;
}

std::vector<const Type *> ValueDeclarators::keyMembers(const Type *keyType) const {
  if (!keyType->isUnion()) {
    return {keyType};
  }
  auto members = keyType->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

bool ValueDeclarators::isFreshArray(const Expr *value) const {
  if (llvm::isa_and_nonnull<ArrayLitExpr>(value)) {
    return true;
  }
  auto *made = dyn_cast_or_null<CallExpr>(value);
  if (made == nullptr) {
    return false;
  }
  Builtin builtin = made->builtin();

  return builtin == Builtin::ArrMap || builtin == Builtin::ArrFilter ||
         builtin == Builtin::ArrSlice || builtin == Builtin::ArrConcat ||
         builtin == Builtin::ArrFlatMap || builtin == Builtin::ArrayFrom;
}

bool ValueDeclarators::isLiteralValue(const Expr *value) const {
  bool isTextChoice = isa<ConditionalExpr>(value) && this->isTextChoice(value);

  return isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value) || isTextChoice;
}

bool ValueDeclarators::isExported(llvm::StringRef name) const {
  if (!this->state.walk.scopes.isTopLevel()) {
    return false;
  }
  const auto *module = this->state.declaration.modules.getCurrentModule();
  if (module == nullptr) {
    return false;
  }
  for (const ExportDecl &exportDecl : module->file.exports) {
    if (this->isExportingName(exportDecl, name)) {
      return true;
    }
  }

  return false;
}

bool ValueDeclarators::isExportingName(const ExportDecl &exportDecl, llvm::StringRef name) const {
  const LocalExport *local = exportDecl.form.as<LocalExport>();
  if (local == nullptr) {
    return false;
  }
  for (const ModuleName &exported : local->names) {
    if (exported.name == name) {
      return true;
    }
  }

  return false;
}

}
