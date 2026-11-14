#pragma once

#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class DecoratorRules {
public:
  explicit DecoratorRules(SemanticState &owner);

  bool isParamUse(const DecoratorUse &use) const;
  bool isDecoratedParam(const Param &param) const;
  bool hasDecoratedParams(const FunctionDecl *functionDecl) const;


  std::string placeText(DecoratorPlace place) const;
  std::string decoratorName(const Expr *expr) const;
  std::string decoratorLabel(const Expr *expr) const;


  DecoratorPlace usePlace(const DecorateRun &run, const DecoratorUse &use) const;
  std::vector<const FunctionDecl *> decoratedConstructors(const ClassDecl *classDecl) const;

private:
  const Expr *withoutCalls(const Expr *expr) const;

  SemanticState &state;
};

}
