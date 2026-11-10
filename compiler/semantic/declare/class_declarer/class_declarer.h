#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/class_declarer/class_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class Scopes;
class ClassBases;
class TypeResolver;
class ReturnInferrer;
class FunctionChecker;
class GenericClassChecker;

enum class DefaultStepKind {
  VALUE,
  CHECK_EMPTY,
  RESOLVE_FAMILY,
  LEAVE_EMPTY,
  MAKE_NEW,
  FILL_OBJECT
};

struct DefaultFor {
  static constexpr DefaultStepKind kind = DefaultStepKind::VALUE;
  const Type *type;
  const TypeRef *ref;
};

struct CheckEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::CHECK_EMPTY;
  const ClassDecl *cls;
};

struct ResolveFamily {
  static constexpr DefaultStepKind kind = DefaultStepKind::RESOLVE_FAMILY;
  const ClassDecl *cls;
};

struct LeaveEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::LEAVE_EMPTY;
  const ClassDecl *cls;
};

struct MakeNew {
  static constexpr DefaultStepKind kind = DefaultStepKind::MAKE_NEW;
  const ClassDecl *cls;
};

struct FillObject {
  static constexpr DefaultStepKind kind = DefaultStepKind::FILL_OBJECT;
  ObjectLitExpr *object;
  std::vector<const Field *> fields;
};

using DefaultStep = Union<DefaultFor, CheckEmpty, ResolveFamily, LeaveEmpty, MakeNew, FillObject>;

enum class CompletionStepKind { START, FINISH };

struct StartCompletion {
  static constexpr CompletionStepKind kind = CompletionStepKind::START;
  ClassDecl *cls;
};

struct FinishCompletion {
  static constexpr CompletionStepKind kind = CompletionStepKind::FINISH;
  ClassDecl *cls;
};

using CompletionStep = Union<StartCompletion, FinishCompletion>;

struct DefaultRun {
  llvm::SmallPtrSetImpl<const ClassDecl *> &visiting;
  llvm::SMLoc loc;
  std::vector<DefaultStep> steps;
  std::vector<Expr *> values;
};

class ClassDeclarer {
public:
  ClassDeclarer(SemanticState &owner, SemanticParts &all);

  static ClassDecl *classOf(const Type *type);
  static std::vector<FunctionDecl *> methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                  bool isStatic);

  void declareClass(ClassDecl *cls);
  void resolveClass(ClassDecl *cls);
  void checkVariants(ClassDecl *cls);
  void completeClass(ClassDecl *cls);
  void declareMethods(ClassDecl *cls);
  void checkVariantPair(FunctionDecl *a, FunctionDecl *b);
  void declareTyped(FunctionDecl *fn, const Type *declared);

private:
  using ResultStaticTypeFixed = Result<const Type *, ClassDeclarerStaticTypeParamsError>;
  using ResultParamTypePassed = Result<const Type *, ClassDeclarerParamTypeError>;
  using ResultFieldNameUnique = Result<const ClassField *, ClassDeclarerUniqueFieldError>;
  using ResultFieldNotLateSet = Result<const ClassField *, ClassDeclarerSetLaterError>;
  using ResultFieldTypeSource = Result<const ClassField *, ClassDeclarerTypedFieldError>;
  using ResultFieldHasNoValue = Result<const ClassField *, ClassDeclarerFieldValueError>;
  using ResultFieldNameIsFree = Result<const ClassField *, ClassDeclarerFieldOrMethodError>;
  using ResultStaticGetsValue = Result<const ClassField *, ClassDeclarerStaticValueError>;
  using ResultNotParentMethod = Result<const ClassField *, ClassDeclarerParentMethodError>;
  using ResultNarrowingLayout = Result<const ClassField *, ClassDeclarerNarrowedFieldError>;
  using ResultVariantsUnequal = Result<const FunctionDecl *, ClassDeclarerVariantPairError>;
  using ResultGenericIsDirect = Result<const FunctionDecl *, ClassDeclarerGenericMethodError>;
  using ResultAbstractReturns = Result<const FunctionDecl *, ClassDeclarerAbstractReturnError>;

  static ResultFieldNotLateSet setLater(const ClassField &f);
  static ResultFieldHasNoValue fieldValue(const ClassField &f);
  static ResultGenericIsDirect genericMethod(const FunctionDecl *fn);
  static void numberVariants(llvm::ArrayRef<FunctionDecl *> functions);
  static int ambiguousCount(const FuncSymbol *x, const FuncSymbol *y);
  static ResultStaticGetsValue staticValue(const ClassField &f, const Type *type);
  static const ClassField *earlierField(const ClassDecl *cls, size_t index);
  static const Param *propertyParam(const ClassDecl *cls, llvm::StringRef name);
  static ResultNotParentMethod parentMethod(const ClassDecl *cls, const ClassField &f);
  static ResultFieldTypeSource typedField(const ClassDecl *cls, const ClassField &f);
  static ResultVariantsUnequal variantPair(const FunctionDecl *a, const FunctionDecl *b);
  static ResultAbstractReturns abstractReturn(const ClassDecl *cls, const FunctionDecl *fn);
  static ResultParamTypePassed paramType(const Param &param, const Type *type,
                                         const Type *declared);
  static ResultFieldNameIsFree fieldOrMethod(const ClassDecl *cls, const ClassField &f,
                                             const FunctionDecl *fn);
  static ResultFieldNameUnique uniqueField(const ClassDecl *cls, const ClassField &f,
                                           const ClassField *earlier);
  static ResultStaticTypeFixed staticTypeParams(const ClassDecl *cls, const ClassField &f,
                                                const Type *type);
  static ResultNarrowingLayout narrowedField(const ClassDecl *cls, const ClassField &f,
                                             const Field &inherited, const ClassDecl *owner);

  Expr *literalValue(const Type *literal, llvm::SMLoc loc);
  void resolveField(ClassDecl *cls, ClassField &f);
  void checkMemberName(const ClassDecl *cls, size_t index);
  void arrowMethod(ClassDecl *cls, ClassField &f, const Type *type);
  void checkVariantList(llvm::ArrayRef<FunctionDecl *> functions);
  const Type *declaredFieldType(const ClassDecl *cls, ClassField &f);
  static bool checkEmpty(const ClassDecl *cls, DefaultRun &run);
  void startCompletion(ClassDecl *cls, std::vector<CompletionStep> &steps);
  void finishCompletion(ClassDecl *cls);
  static bool isPlainObject(const Type *type);
  static void fillObject(const FillObject &fill, DefaultRun &run);
  bool applyDefaultStep(const DefaultStep &step, DefaultRun &run);
  bool defaultFor(const DefaultFor &wanted, DefaultRun &run);
  Expr *simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc);
  void resolveFamily(const ClassDecl *c, DefaultRun &run);
  void makeEmptyNew(const ClassDecl *cls, DefaultRun &run);
  void declareStaticVar(const ClassDecl *cls, const ClassField &f, StaticField *shared,
                        const Type *type);
  Expr *defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                     llvm::SmallPtrSetImpl<const ClassDecl *> &visiting);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ClassBases *classBases = nullptr;
  TypeResolver *typeResolver = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
