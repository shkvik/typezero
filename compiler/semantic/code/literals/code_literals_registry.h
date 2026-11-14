#pragma once

#include "compiler/semantic/code/literals/array_literals/array_literals.h"
#include "compiler/semantic/code/literals/literal_rules/literal_rules.h"
#include "compiler/semantic/code/literals/method_literals/method_literals.h"
#include "compiler/semantic/code/literals/object_literal_keys/object_literal_keys.h"
#include "compiler/semantic/code/literals/object_literal_properties/object_literal_properties.h"
#include "compiler/semantic/code/literals/scalar_literals/scalar_literals.h"
#include "compiler/semantic/code/literals/spread_literals/spread_literals.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeLiteralsRegistry {
public:
  explicit CodeLiteralsRegistry(SemanticState &owner);

  void checkTemplate(const CheckTemplate &task);
  void afterNamedProp(const AfterNamedProp &task);
  void checkIntLiteral(const CheckIntLiteral &task);
  void afterUnionNeeds(const AfterUnionNeeds &task);
  void nextInferredKey(const NextInferredKey &task);
  void afterSpreadProp(const AfterSpreadProp &task);
  void nextTemplatePart(const NextTemplatePart &task);
  void afterComputedKey(const AfterComputedKey &task);
  void afterInferredKey(const AfterInferredKey &task);
  void nextArrayElement(const NextArrayElement &task);
  void afterSpreadMatch(const AfterSpreadMatch &task);
  void afterTemplatePart(const AfterTemplatePart &task);
  void afterTemplateType(const AfterTemplateType &task);
  void nextKeyedProperty(const NextKeyedProperty &task);
  void nextNamedProperty(const NextNamedProperty &task);
  void afterRecordSpread(const AfterRecordSpread &task);
  void nextLiteralMember(const NextLiteralMember &task);
  void afterLiteralField(const AfterLiteralField &task);
  void walkLiteralMethod(const WalkLiteralMethod &task);
  void checkArrayLiteral(const CheckArrayLiteral &task);
  void finishPairLiteral(const FinishPairLiteral &task);
  void checkObjectLiteral(const CheckObjectLiteral &task);
  void nextRecordProperty(const NextRecordProperty &task);
  void nextSpreadProperty(const NextSpreadProperty &task);
  void walkLiteralMethods(const WalkLiteralMethods &task);
  void matchLiteralTarget(const MatchLiteralTarget &task);
  void afterSpreadElement(const AfterSpreadElement &task);
  void afterLiteralMethods(const AfterLiteralMethods &task);
  void afterLiteralElement(const AfterLiteralElement &task);
  void afterUntargetedField(const AfterUntargetedField &task);
  void afterLiteralMethodReturn(const AfterLiteralMethodReturn &task);

private:
  LiteralRules literalRules;
  ArrayLiterals arrayLiterals;
  ScalarLiterals scalarLiterals;
  SpreadLiterals spreadLiterals;
  MethodLiterals methodLiterals;
  ObjectLiteralKeys objectLiteralKeys;
  ObjectLiteralProperties objectLiteralProperties;
};

}
