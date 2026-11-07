#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/decorator_checker/decorator_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class DecoratorChecker {
public:
  explicit DecoratorChecker(Checker &owner);

  void decorateClass(ClassDecl *cls);
  const Type *methodDescriptorType(const Type *args, const Type *ret);

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

  static size_t argumentsOf(Place place);
  void checkDecoratedNames(ClassDecl *cls);
  static std::string placeText(Place place);
  static std::string argumentsText(Place place);
  static bool isPlainReference(const Expr *expr);
  static std::string decoratorName(const Expr *expr);
  static bool decoratedParams(const FunctionDecl *fn);
  static ResultMetadataArgsPair metadataArgs(const CallExpr *metadata);
  static ResultDescriptorUsable genericDescriptor(const ClassDecl *cls, bool descriptor);
  bool decoratorParams(Expr *callee, std::vector<const Type *> &params, size_t &required);
  static ResultFirstConstructor secondConstructor(const ClassDecl *cls, const FunctionDecl *first);
  static ResultPlainMemberFound memberDecoratable(const ClassField *field,
                                                  const FunctionDecl *method);
  static ResultNamesAreDistinct namesDistinct(const DecoratedName &a, const DecoratedName &b,
                                              llvm::StringRef cls);
  void decorate(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                llvm::ArrayRef<Checker::DecoratorUse> uses);
  static ResultDecoratorArgsFit decoratorFits(llvm::StringRef label, Place place, size_t needs,
                                              size_t given, const Type *third);
  static ResultDecoratorReturns decoratorResult(llvm::StringRef label, const Type *result,
                                                Place place, bool descriptor,
                                                const ClassDecl *current);

  Checker &checker;
};

}
