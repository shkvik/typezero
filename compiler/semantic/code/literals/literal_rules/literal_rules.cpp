#include "compiler/semantic/code/literals/literal_rules/literal_rules.h"

using llvm::dyn_cast;

namespace tz {

LiteralRules::LiteralRules(SemanticState &owner)
    : state(owner) {}

LiteralRules::ResultUsableFieldValue
LiteralRules::checkValueTyped(const Type *type, llvm::StringRef what, llvm::StringRef name) const {
  if (type->isVoid()) {
    auto message = ErrorTextObjectLiteral::voidValue(what);

    return err(CodeLiteralsRegistryValueTypedError{
        .code = CodeLiteralsRegistryValueTypedErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    auto message = ErrorTextObjectLiteral::undefOf(what, name);

    return err(CodeLiteralsRegistryValueTypedError{
        .code = CodeLiteralsRegistryValueTypedErrorCode::UNDEFINED_VALUE,
        .message = message,
    });
  }

  return ok(type);
}

const Field *LiteralRules::fieldNamed(const std::vector<Field> &fields,
                                      llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

void LiteralRules::nameFunctionValue(ObjectLitExpr::Property &property) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(property.value);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = property.name;
  }
}

bool LiteralRules::reportUnusableValue(const ObjectLitExpr::Property &property, const Type *type) {
  auto what = ErrorTextObjectLiteral::fieldName(property.name);
  auto usable = this->checkValueTyped(type, what, property.name);
  if (usable.isOk()) {
    return false;
  }
  this->state.diagnostics.error(property.value->loc(), usable.error().message);

  return true;
}

ClassDecl *LiteralRules::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
