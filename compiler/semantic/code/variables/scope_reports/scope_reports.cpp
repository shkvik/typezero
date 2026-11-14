#include "compiler/semantic/code/variables/scope_reports/scope_reports.h"

namespace tz {

ScopeReports::ScopeReports(SemanticState &owner)
    : state(owner) {}

void ScopeReports::closeScope() {
  if (this->state.code.variables.hasUntyped()) {
    this->reportUntyped();
  }

  return this->state.walk.scopes.closeScope();
}

ScopeReports::ResultResultCheck ScopeReports::resultChecked(const VarSymbol *variable) const {
  const Type *held = variable->type->nonOptional();
  if (held->isPromise()) {
    auto message = ErrorTextResultUse::promise(variable->name);

    return err(CodeVariablesRegistryResultCheckedError{
        .code = CodeVariablesRegistryResultCheckedErrorCode::PROMISE_NEVER_AWAITED,
        .message = message,
    });
  }

  auto message = ErrorTextResultUse::unchecked(variable->name);

  return err(CodeVariablesRegistryResultCheckedError{
      .code = CodeVariablesRegistryResultCheckedErrorCode::RESULT_NEVER_CHECKED,
      .message = message,
  });
}

ScopeReports::ResultVarHasTypes ScopeReports::varTyped(const Untyped &untyped,
                                                       const Type *type) const {
  llvm::StringRef name = untyped.var->name;
  auto hint = this->typeHint(untyped, type);
  if (untyped.initUndefined) {
    auto message = ErrorTextVarTyped::undefined(hint);

    return err(CodeVariablesRegistryVarTypedError{
        .code = CodeVariablesRegistryVarTypedErrorCode::UNDEFINED_ONLY,
        .message = message,
    });
  }

  auto message = ErrorTextVarTyped::needsType(name, hint);

  return err(CodeVariablesRegistryVarTypedError{
      .code = CodeVariablesRegistryVarTypedErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

std::string ScopeReports::typeHint(const Untyped &untyped, const Type *type) const {
  llvm::StringRef name = untyped.var->name;
  bool isKnown = type != nullptr && !type->isError() && !type->isUndefined();
  if (isKnown) {
    return ErrorTextVarTyped::writeType(name, type->name());
  }
  if (untyped.initUndefined) {
    return ErrorTextVarTyped::addOne(name);
  }

  return ErrorTextVarTyped::example(name);
}

void ScopeReports::reportUncheckedResults() {
  auto readAt = llvm::DenseMap<const char *, bool>();
  auto resultVars = this->state.code.variables.getResultVars();
  for (const VarSymbol *variable : resultVars) {
    readAt[variable->loc.getPointer()] |= variable->read;
  }
  for (const VarSymbol *variable : resultVars) {
    bool &isDone = readAt[variable->loc.getPointer()];
    if (!isDone) {
      isDone = true;
      this->reportUncheckedResult(variable);
    }
  }
  this->state.code.variables.forgetResultVars();
}

void ScopeReports::reportUncheckedResult(const VarSymbol *variable) {
  auto checked = this->resultChecked(variable);
  if (checked.isErr()) {
    this->state.diagnostics.error(variable->loc, checked.error().message);
  }
}

void ScopeReports::reportUntyped() {
  auto &marks = this->state.code.variables;
  for (const Untyped &untyped : marks.getUntyped()) {
    if (this->isUntypedHere(untyped)) {
      this->reportUntypedVariable(untyped);
      marks.forgetUntyped(untyped.var);
    }
  }
}

bool ScopeReports::isUntypedHere(const Untyped &untyped) const {
  auto found = this->state.walk.scopes.findInCurrentScope(untyped.var->name);

  return found.has_value() && found->var() == untyped.var;
}

void ScopeReports::reportUntypedVariable(const Untyped &untyped) {
  const auto *type = this->assignedType(untyped);
  auto typed = this->varTyped(untyped, type);
  if (typed.isErr()) {
    this->state.diagnostics.error(untyped.var->loc, typed.error().message);
  }
}

const Type *ScopeReports::assignedType(const Untyped &untyped) const {
  const Type *type = untyped.initUndefined ? Type::undefinedTy() : nullptr;
  for (const Type *candidate : untyped.assigned) {
    const auto *common = this->commonAssigned(type, candidate);
    type = common != nullptr ? common : Type::error();
  }

  return type;
}

const Type *ScopeReports::commonAssigned(const Type *type, const Type *candidate) const {
  if (type == nullptr) {
    return candidate;
  }
  const auto *common = this->state.types.relations.unify(type, candidate);
  if (common != nullptr) {
    return common;
  }
  bool isNumbers = type->nonOptional()->isNumeric() && candidate->isNumeric();
  if (!isNumbers) {
    return nullptr;
  }
  if (type->nonOptional()->isFloat()) {
    return type;
  }

  return candidate->isFloat() ? candidate : nullptr;
}

}
