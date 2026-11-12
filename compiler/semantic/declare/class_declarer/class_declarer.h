#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declare/class_declarer/class_declarer_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class ClassLinks;
class SemanticParts;
class Scopes;
class ClassBases;
class TypeResolver;
class FunctionChecker;
class GenericClassChecker;

enum class DefaultStepKind {
  VALUE,
  ENTER_EMPTY,
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

struct EnterEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::ENTER_EMPTY;
  ClassDecl *classDecl;
};

struct ResolveFamily {
  static constexpr DefaultStepKind kind = DefaultStepKind::RESOLVE_FAMILY;
  ClassDecl *classDecl;
};

struct LeaveEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::LEAVE_EMPTY;
  ClassDecl *classDecl;
};

struct MakeNew {
  static constexpr DefaultStepKind kind = DefaultStepKind::MAKE_NEW;
  ClassDecl *classDecl;
};

struct FillObject {
  static constexpr DefaultStepKind kind = DefaultStepKind::FILL_OBJECT;
  ObjectLitExpr *object;
  std::vector<const Field *> fields;
};

using DefaultStep = Union<DefaultFor, EnterEmpty, ResolveFamily, LeaveEmpty, MakeNew, FillObject>;

enum class CompletionStepKind { START, FINISH };

struct StartCompletion {
  static constexpr CompletionStepKind kind = CompletionStepKind::START;
  ClassDecl *classDecl;
};

struct FinishCompletion {
  static constexpr CompletionStepKind kind = CompletionStepKind::FINISH;
  ClassDecl *classDecl;
};

using CompletionStep = Union<StartCompletion, FinishCompletion>;

struct DefaultRun {
  llvm::SmallPtrSetImpl<const ClassDecl *> &visiting;
  llvm::SMLoc loc;
  std::vector<DefaultStep> steps;
  std::vector<Expr *> values;
  bool hasNoDefault;
};

class ClassDeclarer {
public:
  ClassDeclarer(SemanticState &owner, SemanticParts &all);

  ClassDecl *classOf(const Type *type) const;
  std::vector<FunctionDecl *> methodsNamed(const ClassDecl *classDecl, llvm::StringRef name) const;
  std::vector<FunctionDecl *> staticMethodsNamed(const ClassDecl *classDecl,
                                                 llvm::StringRef name) const;
  std::vector<FunctionDecl *> memberFunctions(const ClassDecl *classDecl) const;

  void declareClass(ClassDecl *classDecl);
  void resolveClass(ClassDecl *classDecl);
  void walkVariants(ClassDecl *classDecl);
  void completeClass(ClassDecl *classDecl);
  void declareMethods(ClassDecl *classDecl);
  std::vector<Stmt *> fieldSets(const ClassDecl *classDecl);
  void reportVariantPair(FunctionDecl *first, FunctionDecl *second);
  void declareTyped(FunctionDecl *functionDecl, const Type *declared);
  FunctionDecl *makeInitDecl(ClassDecl *classDecl, std::vector<Stmt *> sets);

private:
  using ResultStaticTypeFixed = Result<const Type *, ClassDeclarerStaticTypeParamsError>;
  using ResultParamTypePassed = Result<const Type *, ClassDeclarerParamTypeError>;
  using ResultFieldNameUnique = Result<const ClassField *, ClassDeclarerUniqueFieldError>;
  using ResultFieldNotLateSet = Result<const ClassField *, ClassDeclarerSetLaterError>;
  using ResultFieldTypeSource = Result<const ClassField *, ClassDeclarerTypedFieldError>;
  using ResultFieldHasNoValue = Result<const ClassField *, ClassDeclarerFieldValueError>;
  using ResultFieldTypeIsSeen = Result<const ClassField *, ClassDeclarerTypeSeenError>;
  using ResultFieldNameIsFree = Result<const ClassField *, ClassDeclarerFieldOrMethodError>;
  using ResultStaticGetsValue = Result<const ClassField *, ClassDeclarerStaticValueError>;
  using ResultNotParentMethod = Result<const ClassField *, ClassDeclarerParentMethodError>;
  using ResultNarrowingLayout = Result<const ClassField *, ClassDeclarerNarrowedFieldError>;
  using ResultVariantsUnequal = Result<const FunctionDecl *, ClassDeclarerVariantPairError>;
  using ResultGenericIsDirect = Result<const FunctionDecl *, ClassDeclarerGenericMethodError>;
  using ResultAbstractReturns = Result<const FunctionDecl *, ClassDeclarerAbstractReturnError>;

  ResultFieldNotLateSet setLater(const ClassField &field) const;
  ResultFieldTypeIsSeen typeSeen(const ClassField &field) const;
  ResultGenericIsDirect genericMethod(const FunctionDecl *method) const;
  ResultStaticGetsValue staticValue(const ClassField &field, const Type *type) const;
  ResultNotParentMethod parentMethod(const ClassDecl *classDecl, const ClassField &field) const;
  ResultFieldTypeSource typedField(const ClassDecl *classDecl, const ClassField &field) const;
  ResultFieldHasNoValue fieldValue(const ClassDecl *classDecl, const ClassField &field) const;
  ResultVariantsUnequal variantPair(const FunctionDecl *first, const FunctionDecl *second) const;
  ResultAbstractReturns abstractReturn(const ClassDecl *classDecl,
                                       const FunctionDecl *method) const;
  ResultParamTypePassed paramType(const Param &param, const Type *type, const Type *declared) const;
  ResultFieldNameIsFree fieldOrMethod(const ClassDecl *classDecl, const ClassField &field,
                                      const FunctionDecl *method) const;
  ResultFieldNameUnique uniqueField(const ClassDecl *classDecl, const ClassField &field,
                                    const ClassField *earlier) const;
  ResultStaticTypeFixed staticTypeParams(const ClassDecl *classDecl, const ClassField &field,
                                         const Type *type) const;
  ResultNarrowingLayout narrowedField(const ClassDecl *classDecl, const ClassField &field,
                                      const Field &inherited, const ClassDecl *owner) const;

  Stmt *fieldSet(const ClassField &field);
  void makeFieldInit(ClassDecl *classDecl);
  void fillDefaultValues(ClassDecl *classDecl);
  void finishCompletion(ClassDecl *classDecl);
  bool isPlainObject(const Type *type) const;
  bool isLiteralUnion(const Type *type) const;
  bool hasInternalField(const Type *type) const;
  bool isMethodField(const ClassField &field) const;
  bool isFullyTyped(const FuncSymbol *function) const;
  bool isNeedingDefault(const ClassField &field) const;
  Expr *literalValue(const Type *literal, llvm::SMLoc loc);
  ClassDecl *defaultedClass(const ClassField &field) const;
  void resolveFamily(ClassDecl *classDecl, DefaultRun &run);
  void resolveField(ClassDecl *classDecl, ClassField &field);
  bool isMadeByConstructor(const ClassDecl *classDecl) const;
  void defaultFor(const DefaultFor &wanted, DefaultRun &run);
  void planObjectDefault(const Type *type, DefaultRun &run);
  void makeEmptyNew(const ClassDecl *classDecl, DefaultRun &run);
  void walkMemberName(const ClassDecl *classDecl, size_t index);
  void walkVariantList(llvm::ArrayRef<FunctionDecl *> functions);
  void fillObject(const FillObject &fill, DefaultRun &run) const;
  void applyDefaultStep(const DefaultStep &step, DefaultRun &run);
  Expr *sizeLiteral(const Type *base, double size, llvm::SMLoc loc);
  void addHiddenParam(FunctionDecl *functionDecl, const Type *type);
  void enterEmptyClass(ClassDecl *classDecl, DefaultRun &run) const;
  void planClassDefault(ClassDecl *classDecl, DefaultRun &run) const;
  void numberVariants(llvm::ArrayRef<FunctionDecl *> functions) const;
  std::vector<const Field *> requiredFields(const Type *type) const;
  std::vector<Field> inheritedFields(const ClassDecl *classDecl) const;
  bool isWrittenCollection(const Type *type, const TypeRef *ref) const;
  void leaveEmptyClass(const ClassDecl *classDecl, DefaultRun &run) const;
  Field *fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const;
  Expr *simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc);
  void declareFieldMethod(ClassDecl *classDecl, const ClassField &field);
  void declareClassMethod(const ClassDecl *classDecl, FunctionDecl *method);
  void resolveInstanceFields(ClassDecl *instance, const ClassDecl *generic);
  void arrowMethod(ClassDecl *classDecl, ClassField &field, const Type *type);
  const Type *writtenFieldType(const ClassDecl *classDecl, ClassField &field);
  const Type *resolvedFieldType(const ClassDecl *classDecl, ClassField &field);
  const Type *declaredFieldType(const ClassDecl *classDecl, ClassField &field);
  bool hasOtherReturn(const FuncSymbol *first, const FuncSymbol *second) const;
  const Type *instanceFieldType(const ClassDecl *instance, const Type *original);
  bool hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isVariantOf(const FunctionDecl *earlier, const FunctionDecl *later) const;
  void startCompletion(ClassDecl *classDecl, std::vector<CompletionStep> &steps);
  const ClassField *earlierField(const ClassDecl *classDecl, size_t index) const;
  const Type *typedParam(const Param &param, const Type *given, const Type *declared);
  const Param *propertyParam(const ClassDecl *classDecl, llvm::StringRef name) const;
  std::optional<int> ambiguousCount(const FuncSymbol *first, const FuncSymbol *second) const;
  std::vector<FunctionDecl *> ownMethodsNamed(const ClassDecl *classDecl,
                                              llvm::StringRef name) const;
  bool isHiddenByAny(llvm::ArrayRef<FunctionDecl *> found, const FunctionDecl *method) const;
  bool isOverriddenByAny(llvm::ArrayRef<FunctionDecl *> found, const FunctionDecl *method) const;
  void addOwnField(const ClassDecl *classDecl, const ClassField &field, std::vector<Field> &fields);
  std::vector<FunctionDecl *> ownStaticMethodsNamed(const ClassDecl *classDecl,
                                                    llvm::StringRef name) const;
  void completeField(const ClassDecl *classDecl, ClassField &field, std::vector<Field> &fields);
  void narrowInheritedField(const ClassDecl *classDecl, const ClassField &field,
                            std::vector<Field> &fields);
  void declareStaticVar(const ClassDecl *classDecl, const ClassField &field, StaticField *shared,
                        const Type *type);
  Expr *defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                     llvm::SmallPtrSetImpl<const ClassDecl *> &visiting);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ClassBases *classBases = nullptr;
  ClassLinks *classLinks = nullptr;
  TypeResolver *typeResolver = nullptr;
  FunctionChecker *functionChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
