#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_rules/decorator_rules.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class DecoratorApplying {
public:
  DecoratorApplying(SemanticState &owner, ClassFacts &sharedFacts,
                    DecoratorRules &sharedDecoratorRules);

  void dropDiagnosticList();
  void relinkClass(const RelinkClass &task);
  void nextAppliedDecorator(const NextAppliedDecorator &task);
  void afterDecoratorApplied(const AfterDecoratorApplied &task);

private:
  using ResultNoOldObjectHeld = Result<const ClassField *, CodeClassesRegistryOldObjectError>;
  using ResultDecoratorReturns = Result<const Type *, CodeClassesRegistryDecoratorResultError>;
  using ResultMetadataArgsPair = Result<const CallExpr *, CodeClassesRegistryMetadataArgsError>;
  using ResultFieldNameIsFree = Result<const ClassField *, CodeClassesRegistryReplacedFieldError>;

  void pushRelink(ClassDecl *classDecl);
  void addInventorySubtype(const Type *type);
  void storeDescriptorValue(const DecorateRun &run);
  void swapTypes(ClassDecl *classDecl, ClassDecl *replacement);
  void applyMetadata(const DecorateRun &run, const DecoratorUse &use);
  void moveChild(ClassDecl *classDecl, ClassDecl *replacement, ClassDecl *child) const;
  void replaceClass(ClassDecl *classDecl, ClassDecl *replacement, llvm::SMLoc loc,
                    std::vector<ClassDecl *> &moved);


  bool isConstructorParamUse(const DecoratorUse &use) const;
  bool isHoldingType(const Type *type, const Type *target) const;


  Expr *classValueAt(ClassDecl *classDecl, llvm::SMLoc loc);
  Expr *memberKey(const DecorateRun &run, const DecoratorUse &use);
  Expr *assignDescriptor(const DecorateRun &run, const DecoratorUse &use, Expr *applied);
  Expr *decoratorOutcome(DecorateRun &run, const DecoratorUse &use, DecoratorPlace place,
                         Expr *applied, std::vector<ClassDecl *> &moved);


  ResultNoOldObjectHeld oldObject(const ClassDecl *classDecl, const ClassField &field) const;
  ResultFieldNameIsFree replacedField(const ClassDecl *classDecl, const ClassDecl *replacement,
                                      const ClassDecl *child, const ClassField &field) const;


  ResultMetadataArgsPair checkMetadataArgs(const CallExpr *metadata) const;
  ResultDecoratorReturns checkDecoratorResult(llvm::StringRef label, const Type *result,
                                              DecoratorPlace place, bool hasDescriptor,
                                              const ClassDecl *current) const;


  llvm::StringRef useOwnerName(const DecorateRun &run, const DecoratorUse &use) const;
  std::vector<ClassDecl *> declaredChildren(const ClassDecl *classDecl,
                                            const ClassDecl *replacement);
  std::vector<Expr *> decoratorArgs(const DecorateRun &run, const DecoratorUse &use,
                                    DecoratorPlace place);

  ClassFacts &facts;
  SemanticState &state;
  DecoratorRules &decoratorRules;
};

}
