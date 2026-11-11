#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class FlowChecker;
class ClassDeclarer;
class TypeRelations;
class ArgumentChecker;
class ClassValueChecker;
class ClassMemberChecker;
class ReflectCallChecker;

class ReflectChecker {
public:
  ReflectChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  void noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast);
  void noteConstructorFlow(const Type *from, const Type *to);
  void finishReflection();
  bool onlyChecked() const;

private:
  using ResultConstructible = Result<ClassDecl *, ReflectCheckerConstructibleError>;
  using ResultFillableClass = Result<ClassDecl *, ReflectCheckerFillableClassError>;

  static ResultFillableClass fillableClass(ClassDecl *made);
  static void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out);
  static void constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out);
  static bool instanceOf(const Type *made, const Type *of);
  static Expr *stripImplicit(Expr *expr);

  Expr *okOf(Expr *value);
  const Type *reflectError();
  ResultConstructible constructible(ClassDecl *cls, llvm::ArrayRef<const Type *> args);
  void walkReflectNews();
  static void markReflectMethods(ClassDecl *cls);

  struct ReflectGet {
    const NameExpr *object;
    const StringLitExpr *key;
    const Type *type;
    const Type *base;
  };

  void walkArgsLoosely(CallExpr *call);
  bool hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);
  Expr *checkReflectGet(CallExpr *call, MemberExpr *member);
  static std::optional<std::string> asciiName(const std::u16string &text);
  Expr *checkKnownGet(CallExpr *call, const ReflectGet &get);
  Expr *readNamedField(const NameExpr *object, const StringLitExpr *key, const std::string &name);
  Expr *readClassField(Expr *seen, const StringLitExpr *key, const std::string &name);
  Expr *checkReflectMethodGet(CallExpr *call, Expr *seen, ClassDecl *cls, const std::string &name);
  Expr *checkDynamicGet(CallExpr *call, const ReflectGet &get);
  void noteReflectedRead(const Type *type, const Type *base);
  Expr *checkReflectFill(CallExpr *call, MemberExpr *member);
  std::optional<const Type *> fillFailure(const Expr *cls, const Type *type);
  Expr *checkReflectApply(CallExpr *call, MemberExpr *member);
  static const Type *appliedResult(const Type *type);
  void walkAppliedFunction(CallExpr *call);
  void walkAppliedThis(CallExpr *call);
  Expr *checkReflectConstruct(CallExpr *call, MemberExpr *member, const Type *expected);
  static bool hasSpread(const ArrayLitExpr *list);
  Expr *checkLiteralConstruct(CallExpr *call, const NameExpr *name, const ArrayLitExpr *list,
                              const Type *expected);
  Expr *checkDynamicConstruct(CallExpr *call, const Type *type);

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  ReflectCallChecker *reflectCallChecker = nullptr;
};

}
