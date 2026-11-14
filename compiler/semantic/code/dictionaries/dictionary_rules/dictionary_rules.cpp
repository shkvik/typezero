#include "compiler/semantic/code/dictionaries/dictionary_rules/dictionary_rules.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DictionaryRules::DictionaryRules(SemanticState &owner)
    : state(owner) {}

void DictionaryRules::finishDictNew(NewExpr *expr, const Type *type, std::vector<Expr *> args) {
  llvm::SMLoc loc = expr->loc();
  auto keyAllowed = this->checkKeyType(type->keyType());
  if (keyAllowed.isErr()) {
    this->state.diagnostics.error(loc, keyAllowed.error().message);
    return this->failDictNew(expr);
  }
  auto *callee = this->state.ast.make<NameExpr>(loc, expr->name);
  auto *call = this->state.ast.make<CallExpr>(loc, callee, std::move(args));
  call->callee->type = type;
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return this->state.walk.tasks.pushChecked(call);
}

void DictionaryRules::failDictNew(NewExpr *expr) {
  expr->type = Type::error();
  this->state.walk.tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

DictionaryRules::ResultKeyTypeIsAllowed DictionaryRules::checkKeyType(const Type *type) {
  if (type->isUnknown()) {
    auto message = ErrorTextKeyType::unknown();

    return err(CodeDictionariesRegistryCheckKeyTypeError{
        .code = CodeDictionariesRegistryCheckKeyTypeErrorCode::UNKNOWN_KEY,
        .message = message,
    });
  }
  if (type->nonOptional()->isResult()) {
    auto message = ErrorTextKeyType::result();

    return err(CodeDictionariesRegistryCheckKeyTypeError{
        .code = CodeDictionariesRegistryCheckKeyTypeErrorCode::RESULT_KEY,
        .message = message,
    });
  }

  return ok(type);
}

const Type *DictionaryRules::wantedElement(const Type *type, bool isMap) const {
  if (type == nullptr) {
    return nullptr;
  }
  if (isMap) {
    return Type::tuple({type->keyType(), type->valueType()});
  }

  return type->keyType();
}

}
