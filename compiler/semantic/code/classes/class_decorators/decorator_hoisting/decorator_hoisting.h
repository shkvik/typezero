#pragma once

#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_rules/decorator_rules.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class DecoratorHoisting {
public:
  DecoratorHoisting(SemanticState &owner, DecoratorRules &sharedDecoratorRules);

  void decorate(const Decorate &task);
  void makeMethodDescriptor(const MakeMethodDescriptor &task);
  void nextHoistedDecorator(const NextHoistedDecorator &task);
  void afterDescriptorDeclared(const AfterDescriptorDeclared &task);
  void declareHoistedDecorator(const DeclareHoistedDecorator &task);
  void afterFactoryMethodReturn(const AfterFactoryMethodReturn &task);
  void afterDescriptorMethodReturn(const AfterDescriptorMethodReturn &task);

private:
  struct KnownParams {
    std::vector<const Type *> params;
    size_t required = 0;
  };

  using ResultDecoratorArgsFit = Result<size_t, CodeClassesRegistryDecoratorFitsError>;
  using ResultDescriptorUsable =
      Result<const ClassDecl *, CodeClassesRegistryGenericDescriptorError>;
  using ResultPlainMemberFound =
      Result<const FunctionDecl *, CodeClassesRegistryMemberDecoratableError>;

  void startDescriptor(DecorateRun run);
  void startKnownDescriptor(DecorateRun run);
  void startDecoratorApplies(DecorateRun run);
  void startHoistedDecorator(DecorateRun run);
  void afterHoistedDecorators(DecorateRun run);
  void addProperty(ObjectLitExpr *literal, BuiltinProperty property, Expr *value,
                   llvm::SMLoc at) const;


  bool matchDecorators(DecorateRun &run);
  bool isMetadataCall(const Expr *expr) const;
  bool isPlainReference(const Expr *expr) const;
  bool matchDecorator(DecorateRun &run, size_t index);
  bool hasKnownSignature(const FuncSymbol *function, const Type *returnType) const;
  bool isAskingMethodReturn(const DecorateRun &run, const DecoratorUse &use,
                            const Expr *expr) const;


  const Type *methodDescriptor(DecorateRun &run);
  const Type *factoryHint(DecorateRun &run, const Expr *expr);
  const Type *thirdParam(const std::optional<KnownParams> &known) const;
  const Type *methodDescriptorType(const Type *args, const Type *returnType);


  const FuncSymbol *genericFactory(const DecorateRun &run, const Expr *expr) const;


  ResultDescriptorUsable checkGenericDescriptor(const ClassDecl *classDecl,
                                                bool hasDescriptor) const;
  ResultPlainMemberFound checkMemberDecoratable(const ClassField *field,
                                                const FunctionDecl *method) const;
  ResultDecoratorArgsFit checkDecoratorFits(llvm::StringRef label, DecoratorPlace place,
                                            size_t needs, size_t given, const Type *third) const;


  std::optional<KnownParams> knownParams(Expr *callee);
  std::optional<KnownParams> namedParams(const NameExpr *name);
  std::optional<KnownParams> typeParams(const Type *type) const;
  std::optional<KnownParams> functionParams(const FuncSymbol *function) const;


  size_t argumentsOf(DecoratorPlace place) const;
  VarSymbol *declareMethodImpl(const DecorateRun &run);
  std::string argumentsText(DecoratorPlace place) const;
  const FuncSymbol *staticMethodSymbol(const MemberExpr *member);
  const FunctionDecl *designConstructor(const DecorateRun &run) const;
  ObjectLitExpr *descriptorLiteral(const DecorateRun &run, const VarSymbol *implementation);

  SemanticState &state;
  DecoratorRules &decoratorRules;
};

}
