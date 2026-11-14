#include "compiler/semantic/code/classes/class_decorators/decorator_rules/decorator_rules.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";

}

DecoratorRules::DecoratorRules(SemanticState &owner)
    : state(owner) {}

bool DecoratorRules::isDecoratedParam(const Param &param) const {
  return !param.decorators.empty();
}

std::string DecoratorRules::placeText(DecoratorPlace place) const {
  switch (place) {
    case DecoratorPlace::CLASS:
      return ErrorTextDecoratorPlace::aClass();
    case DecoratorPlace::FIELD:
      return ErrorTextDecoratorPlace::aField();
    case DecoratorPlace::METHOD:
      return ErrorTextDecoratorPlace::aMethod();
    case DecoratorPlace::PARAMETER:
      return ErrorTextDecoratorPlace::aParameter();
  }

  return ErrorTextDecoratorPlace::aParameter();
}

DecoratorPlace DecoratorRules::usePlace(const DecorateRun &run, const DecoratorUse &use) const {
  return this->isParamUse(use) ? DecoratorPlace::PARAMETER : run.ownPlace;
}

bool DecoratorRules::isParamUse(const DecoratorUse &use) const {
  return use.param >= 0;
}

std::string DecoratorRules::decoratorName(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = this->withoutCalls(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    names.push_back(member->name);
    link = this->withoutCalls(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr || root->name.starts_with(kHiddenPrefix)) {
    return "";
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

std::string DecoratorRules::decoratorLabel(const Expr *expr) const {
  auto name = this->decoratorName(expr);

  return ErrorTextDecoratorPlace::label(name);
}

const Expr *DecoratorRules::withoutCalls(const Expr *expr) const {
  const Expr *callee = expr;
  const auto *call = dyn_cast<CallExpr>(callee);
  while (call != nullptr) {
    callee = call->callee;
    call = dyn_cast<CallExpr>(callee);
  }

  return callee;
}

bool DecoratorRules::hasDecoratedParams(const FunctionDecl *functionDecl) const {
  auto isDecorated = std::bind_front(&DecoratorRules::isDecoratedParam, this);

  return llvm::any_of(functionDecl->params, isDecorated);
}

std::vector<const FunctionDecl *>
DecoratorRules::decoratedConstructors(const ClassDecl *classDecl) const {
  auto decorated = std::vector<const FunctionDecl *>();
  for (const FunctionDecl *constructor : classDecl->constructors) {
    if (this->hasDecoratedParams(constructor)) {
      decorated.push_back(constructor);
    }
  }

  return decorated;
}

}
