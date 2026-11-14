#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/code/literals/literal_rules/literal_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class ObjectLiteralProperties {
public:
  ObjectLiteralProperties(SemanticState &owner, LiteralRules &sharedLiteralRules);

  void nextInferredKey(const NextInferredKey &task);
  void afterInferredKey(const AfterInferredKey &task);
  void nextKeyedProperty(const NextKeyedProperty &task);
  void nextNamedProperty(const NextNamedProperty &task);
  void afterUntargetedField(const AfterUntargetedField &task);

private:
  using ResultKeyedFieldsAgree = Result<const Type *, CodeLiteralsRegistryKeyedFieldError>;
  using ResultKeyedObjectTyped = Result<const Type *, CodeLiteralsRegistryKeyedTargetError>;
  using ResultRequiredFieldSet = Result<const Field *, CodeLiteralsRegistryFieldGivenError>;
  using ResultTargetFieldFound = Result<const Field *, CodeLiteralsRegistryTargetFieldError>;
  using ResultKeyedValuesAgree = Result<const Type *, CodeLiteralsRegistrySameKeyedTypeError>;
  using ResultSpreadAbsent = Result<const ObjectLitExpr *, CodeLiteralsRegistryNoSpreadError>;
  using ResultValueTypeIsKnown = Result<const Type *, CodeLiteralsRegistryInferredValueError>;
  using ResultKeyWithoutSpread = Result<const Type *, CodeLiteralsRegistryKeyedPlacementError>;

  void afterKeyedProperties(ObjectLitDraft draft);
  void finishObjectLit(const ObjectLitDraft &draft);
  void startMethodLiteral(ObjectLitExpr *literal, const Type *target);
  void failKeyedProperty(ObjectLitExpr *literal, ObjectLitExpr::Property &property,
                         const std::string &message);
  void startSpreadLit(ObjectLitExpr *literal, const Type *target, const std::vector<bool> &named,
                      bool isTyped);


  bool takePrechecked(const Expr *value);
  bool hasMethod(const ObjectLitExpr *literal) const;
  bool isPublicField(const ClassDecl *classDecl, const std::string &name) const;
  bool hasPropertyNamed(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool reportKeyedFields(const ObjectLitExpr::Property &property, const Type *value,
                         std::vector<Field> &fields);
  bool reportKeyedClash(const ObjectLitExpr::Property &property, llvm::StringRef name,
                        const Field &same, const Type *value);


  ResultValueTypeIsKnown checkInferredValue(const Type *value) const;
  ResultKeyedFieldsAgree checkKeyedField(const Type *target, const Type *keyType) const;
  ResultKeyedObjectTyped checkKeyedTarget(const Type *target, const Type *keyType) const;
  ResultTargetFieldFound checkTargetField(const Type *target, llvm::StringRef name) const;
  ResultKeyWithoutSpread checkKeyedPlacement(const ObjectLitExpr *literal,
                                             const Type *keyType) const;
  ResultKeyedValuesAgree checkSameKeyedType(llvm::StringRef name, const Field &same,
                                            const Type *value) const;
  ResultRequiredFieldSet checkFieldGiven(const ObjectLitExpr *literal, const Field &field,
                                         const Type *target) const;


  Type *makeLiteralClass(ObjectLitExpr *literal);
  ResultSpreadAbsent checkNoSpread(const ObjectLitExpr *literal) const;
  const Type *keyedFieldType(const Type *type, const Type *keyType) const;

  SemanticState &state;
  LiteralRules &literalRules;
};

}
