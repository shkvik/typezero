#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class LateClasses;
class StmtChecker;
class TypeRelations;
class ClassDeclarer;
class MetadataChecker;
class ClassValueChecker;

struct DecoratorUse {
  const Decorator *decorator;
  int param = -1;
  const FunctionDecl *owner = nullptr;
};

class DecoratorChecker {
public:
  DecoratorChecker(SemanticState &owner, SemanticParts &all);

  void decorateClass(ClassDecl *classDecl);
  const Type *methodDescriptorType(const Type *args, const Type *returnType);

private:
  enum class Place { Class, Field, Method, Parameter };

  struct DecoratedName {
    llvm::StringRef name;
    llvm::SMLoc loc;
    bool isStatic;
    std::vector<std::string> decorators;
  };

  using ResultDecoratorArgsFit = Result<size_t, DecoratorCheckerDecoratorFitsError>;
  using ResultDecoratorReturns = Result<const Type *, DecoratorCheckerDecoratorResultError>;
  using ResultMetadataArgsPair = Result<const CallExpr *, DecoratorCheckerMetadataArgsError>;
  using ResultFirstConstructor = Result<const ClassDecl *, DecoratorCheckerSecondConstructorError>;
  using ResultDescriptorUsable = Result<const ClassDecl *, DecoratorCheckerGenericDescriptorError>;
  using ResultNamesAreDistinct = Result<const DecoratedName *, DecoratorCheckerNamesDistinctError>;
  using ResultPlainMemberFound =
      Result<const FunctionDecl *, DecoratorCheckerMemberDecoratableError>;

  size_t argumentsOf(Place place) const;
  void walkDecoratedNames(ClassDecl *classDecl);
  std::string placeText(Place place) const;
  std::string argumentsText(Place place) const;
  bool isPlainReference(const Expr *expr) const;
  std::string decoratorName(const Expr *expr) const;
  const Expr *withoutCalls(const Expr *expr) const;
  bool decoratedParams(const FunctionDecl *functionDecl) const;
  ResultMetadataArgsPair metadataArgs(const CallExpr *metadata) const;
  ResultDescriptorUsable genericDescriptor(const ClassDecl *classDecl, bool descriptor) const;
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params, size_t &required);
  ResultFirstConstructor secondConstructor(const ClassDecl *classDecl,
                                           const FunctionDecl *first) const;
  ResultPlainMemberFound memberDecoratable(const ClassField *field,
                                           const FunctionDecl *method) const;
  ResultNamesAreDistinct namesDistinct(const DecoratedName &a, const DecoratedName &b,
                                       llvm::StringRef className) const;
  void decorate(ClassDecl *classDecl, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<DecoratorUse> uses);
  struct DecorateRun {
    ClassDecl *cls = nullptr;
    ClassField *field = nullptr;
    FunctionDecl *method = nullptr;
    llvm::ArrayRef<DecoratorUse> uses;
    Place own = Place::Class;
    llvm::StringRef memberName;
    llvm::SMLoc at;
    const Type *earlyDescriptor = nullptr;
    std::vector<Expr *> callees;
    std::vector<size_t> takes;
    bool descriptor = false;
    VarSymbol *descVar = nullptr;
    const Type *descType = nullptr;
    ClassDecl *current = nullptr;
  };

  bool hasKnownSignature(FuncSymbol *function) const;
  bool isMetadataCall(const Expr *expr) const;
  const Type *methodDescriptor(DecorateRun &run);
  const Type *factoryHint(DecorateRun &run, const Expr *expr);
  void hoistDecorators(DecorateRun &run);
  Expr *hoistDecorator(DecorateRun &run, const DecoratorUse &use);
  bool decoratorsFit(DecorateRun &run);
  bool decoratorFitsAt(DecorateRun &run, size_t index);
  void declareDescriptor(DecorateRun &run);
  VarSymbol *declareMethodImpl(const DecorateRun &run);
  Expr *descriptorInit(const DecorateRun &run, const VarSymbol *implementation);
  void addProperty(ObjectLitExpr *literal, llvm::StringRef name, Expr *value, llvm::SMLoc at) const;
  const FunctionDecl *designConstructor(const DecorateRun &run) const;
  void applyDecorator(DecorateRun &run, size_t index);
  void applyMetadata(DecorateRun &run, const DecoratorUse &use);
  std::vector<Expr *> decoratorArgs(const DecorateRun &run, const DecoratorUse &use, Place place);
  Expr *decoratorOutcome(DecorateRun &run, const DecoratorUse &use, Place place, Expr *applied);
  void storeDescriptorValue(const DecorateRun &run);
  ResultDecoratorArgsFit decoratorFits(llvm::StringRef label, Place place, size_t needs,
                                       size_t given, const Type *third) const;
  ResultDecoratorReturns decoratorResult(llvm::StringRef label, const Type *result, Place place,
                                         bool descriptor, const ClassDecl *current) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LateClasses *lateClasses = nullptr;
  StmtChecker *stmtChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  MetadataChecker *metadataChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
