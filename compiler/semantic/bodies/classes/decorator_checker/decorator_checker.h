#pragma once

#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
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
  enum class Place { CLASS, FIELD, METHOD, PARAMETER };

  struct DecoratedName {
    llvm::StringRef name;
    llvm::SMLoc loc;
    bool isStatic;
    std::vector<std::string> decorators;
  };

  struct DecoratedMember {
    ClassField *field = nullptr;
    FunctionDecl *method = nullptr;
    bool isStatic = false;
    const char *at = nullptr;
  };

  struct KnownParams {
    std::vector<const Type *> params;
    size_t required = 0;
  };

  struct DecorateRun {
    ClassDecl *classDecl = nullptr;
    ClassField *field = nullptr;
    FunctionDecl *method = nullptr;
    llvm::ArrayRef<DecoratorUse> uses;
    Place ownPlace = Place::CLASS;
    llvm::StringRef memberName;
    llvm::SMLoc at;
    const Type *earlyDescriptor = nullptr;
    std::vector<Expr *> callees;
    std::vector<size_t> takes;
    bool hasDescriptor = false;
    VarSymbol *descriptorVar = nullptr;
    const Type *descriptorType = nullptr;
    ClassDecl *current = nullptr;
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
  std::string placeText(Place place) const;
  std::string argumentsText(Place place) const;
  Place usePlace(const DecorateRun &run, const DecoratorUse &use) const;
  bool isParamUse(const DecoratorUse &use) const;
  bool isConstructorParamUse(const DecoratorUse &use) const;
  llvm::StringRef useOwnerName(const DecorateRun &run, const DecoratorUse &use) const;
  bool isPlainReference(const Expr *expr) const;
  std::string decoratorName(const Expr *expr) const;
  std::string decoratorLabel(const Expr *expr) const;
  const Expr *withoutCalls(const Expr *expr) const;
  bool hasDecoratedParams(const FunctionDecl *functionDecl) const;
  bool isDecoratedParam(const Param &param) const;
  std::optional<KnownParams> knownParams(Expr *callee);
  std::optional<KnownParams> namedParams(const NameExpr *name);
  std::optional<KnownParams> typeParams(const Type *type) const;
  std::optional<KnownParams> functionParams(const FuncSymbol *function) const;
  const FuncSymbol *staticMethodSymbol(const MemberExpr *member);
  const Type *thirdParam(const std::optional<KnownParams> &known) const;
  void walkDecoratedNames(ClassDecl *classDecl);
  std::vector<DecoratedName> decoratedNames(const ClassDecl *classDecl) const;
  std::vector<std::string> decoratorNames(llvm::ArrayRef<Decorator> decorators) const;
  std::vector<std::string> methodDecoratorNames(const FunctionDecl *functionDecl) const;
  void addDecoratorNames(std::vector<std::string> &names,
                         llvm::ArrayRef<Decorator> decorators) const;
  void walkNameClash(llvm::ArrayRef<DecoratedName> members, size_t index,
                     llvm::StringRef className);
  std::string sharedDecorator(const DecoratedName &earlier, const DecoratedName &later) const;
  void walkDecoratedConstructors(const ClassDecl *classDecl);
  std::vector<const FunctionDecl *> decoratedConstructors(const ClassDecl *classDecl) const;
  std::vector<DecoratedMember> decoratedMembers(ClassDecl *classDecl) const;
  bool isMemberBefore(const DecoratedMember &first, const DecoratedMember &second) const;
  std::vector<DecoratorUse> memberUses(const DecoratedMember &member) const;
  std::vector<DecoratorUse> classUses(const ClassDecl *classDecl) const;
  std::vector<DecoratorUse> ownUses(llvm::ArrayRef<Decorator> decorators) const;
  void addParamUses(std::vector<DecoratorUse> &uses, const FunctionDecl *functionDecl) const;
  void addParamUse(std::vector<DecoratorUse> &uses, const FunctionDecl *functionDecl,
                   size_t index) const;
  ResultMetadataArgsPair checkMetadataArgs(const CallExpr *metadata) const;
  ResultDescriptorUsable checkGenericDescriptor(const ClassDecl *classDecl,
                                                bool hasDescriptor) const;
  ResultFirstConstructor
  checkOneDecoratedConstructor(const ClassDecl *classDecl,
                               llvm::ArrayRef<const FunctionDecl *> decorated) const;
  ResultPlainMemberFound checkMemberDecoratable(const ClassField *field,
                                                const FunctionDecl *method) const;
  ResultNamesAreDistinct checkNamesDistinct(const DecoratedName &earlier,
                                            const DecoratedName &later,
                                            llvm::StringRef className) const;
  void decorate(ClassDecl *classDecl, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<DecoratorUse> uses);
  Place placeOfMember(const ClassField *field, const FunctionDecl *method) const;
  llvm::StringRef nameOfMember(const ClassField *field, const FunctionDecl *method) const;
  bool hasKnownSignature(FuncSymbol *function) const;
  bool isMetadataCall(const Expr *expr) const;
  const Type *methodDescriptor(DecorateRun &run);
  const Type *factoryHint(DecorateRun &run, const Expr *expr);
  void hoistDecorators(DecorateRun &run);
  Expr *decoratorCallee(DecorateRun &run, const DecoratorUse &use);
  Expr *hoistDecorator(DecorateRun &run, const DecoratorUse &use);
  bool matchDecorators(DecorateRun &run);
  bool matchDecorator(DecorateRun &run, size_t index);
  void declareDescriptor(DecorateRun &run);
  VarSymbol *declareMethodImpl(const DecorateRun &run);
  Expr *descriptorInit(const DecorateRun &run, const VarSymbol *implementation);
  void addProperty(ObjectLitExpr *literal, BuiltinProperty property, Expr *value,
                   llvm::SMLoc at) const;
  const FunctionDecl *designConstructor(const DecorateRun &run) const;
  void applyDecorator(DecorateRun &run, size_t index);
  void applyMetadata(DecorateRun &run, const DecoratorUse &use);
  std::vector<Expr *> decoratorArgs(const DecorateRun &run, const DecoratorUse &use, Place place);
  Expr *memberKey(const DecorateRun &run, const DecoratorUse &use);
  Expr *decoratorOutcome(DecorateRun &run, const DecoratorUse &use, Place place, Expr *applied);
  Expr *assignDescriptor(const DecorateRun &run, const DecoratorUse &use, Expr *applied);
  void storeDescriptorValue(const DecorateRun &run);
  ResultDecoratorArgsFit checkDecoratorFits(llvm::StringRef label, Place place, size_t needs,
                                            size_t given, const Type *third) const;
  ResultDecoratorReturns checkDecoratorResult(llvm::StringRef label, const Type *result,
                                              Place place, bool hasDescriptor,
                                              const ClassDecl *current) const;

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
