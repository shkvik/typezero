#include "compiler/program/symbols/var_symbol/var_symbol.h"

namespace tz {

VarPlace VarSymbol::localPlace(FunctionDecl *owner, unsigned declSeq) {
  LocalVarState state{.declSeq = declSeq};
  if (!owner) {
    return TopLevelLocalVar{.state = state};
  }

  return FunctionLocalVar{.owner = owner, .state = state};
}

LocalVarState *VarSymbol::local() {
  LocalVarState *state = nullptr;
  switch (this->place.kind()) {
    case VarPlaceKind::GLOBAL_VAR:
      break;
    case VarPlaceKind::TOP_LEVEL_LOCAL_VAR:
      state = &this->place.as<TopLevelLocalVar>()->state;
      break;
    case VarPlaceKind::FUNCTION_LOCAL_VAR:
      state = &this->place.as<FunctionLocalVar>()->state;
      break;
  }

  return state;
}

const LocalVarState *VarSymbol::local() const {
  const LocalVarState *state = nullptr;
  switch (this->place.kind()) {
    case VarPlaceKind::GLOBAL_VAR:
      break;
    case VarPlaceKind::TOP_LEVEL_LOCAL_VAR:
      state = &this->place.as<TopLevelLocalVar>()->state;
      break;
    case VarPlaceKind::FUNCTION_LOCAL_VAR:
      state = &this->place.as<FunctionLocalVar>()->state;
      break;
  }

  return state;
}

bool VarSymbol::isGlobal() const {
  return this->place.is<GlobalVar>();
}

bool VarSymbol::usedInFunction() const {
  const GlobalVar *global = this->place.as<GlobalVar>();

  return global && global->usedInFunction;
}

FunctionDecl *VarSymbol::owner() const {
  const FunctionLocalVar *inFunction = this->place.as<FunctionLocalVar>();

  return inFunction ? inFunction->owner : nullptr;
}

unsigned VarSymbol::declSeq() const {
  const LocalVarState *state = this->local();

  return state ? state->declSeq : 0;
}

bool VarSymbol::captured() const {
  const LocalVarState *state = this->local();

  return state && state->captured;
}

bool VarSymbol::capturedEarly() const {
  const LocalVarState *state = this->local();

  return state && state->capturedEarly;
}

bool VarSymbol::unshared() const {
  const LocalVarState *state = this->local();

  return state && state->unshared;
}

bool VarSymbol::boxed() const {
  return this->captured() && (this->reassigned || this->capturedEarly());
}

FunctionDecl *VarSymbol::function() const {
  const DeclaredFunction *declared = this->holds.as<DeclaredFunction>();

  return declared ? declared->function : nullptr;
}

FunctionDecl *VarSymbol::lambda() const {
  const Lambda *held = this->holds.as<Lambda>();

  return held ? held->lambda : nullptr;
}

const RegexLitExpr *VarSymbol::regex() const {
  const RegexLiteral *literal = this->holds.as<RegexLiteral>();

  return literal ? literal->regex : nullptr;
}

const FunctionDecl *VarSymbol::reflectMethod() const {
  const ReflectedMethod *reflected = this->holds.as<ReflectedMethod>();

  return reflected ? reflected->method : nullptr;
}

const std::vector<llvm::StringRef> &VarSymbol::reflectNames() const {
  static const std::vector<llvm::StringRef> none;
  const ReflectedNames *reflected = this->holds.as<ReflectedNames>();

  return reflected ? reflected->names : none;
}

}
