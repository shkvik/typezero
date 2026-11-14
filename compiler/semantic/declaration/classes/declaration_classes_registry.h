#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/classes/declaration_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class TypeResolver;
class SemanticParts;
class EventMapInference;
class GenericClassChecker;
class GenericClassInstantiator;
class DeclarationModulesRegistry;
class DeclarationFunctionsRegistry;

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

class DeclarationClassesRegistry {
public:
  DeclarationClassesRegistry(SemanticState &owner, SemanticParts &all);

  void declareClass(ClassDecl *classDecl);
  void resolveClass(ClassDecl *classDecl);
  void walkVariants(ClassDecl *classDecl);
  void completeClass(ClassDecl *classDecl);
  void declareMethods(ClassDecl *classDecl);
  void addBase(const Type *type, const Type *base);
  void reportVariantPair(FunctionDecl *first, FunctionDecl *second);
  void declareTyped(FunctionDecl *functionDecl, const Type *declared);
  void declareItemMethods(Stmt *item, Module *module, std::vector<FunctionDecl *> &functions);


  std::vector<Stmt *> fieldSets(const ClassDecl *classDecl);
  FunctionDecl *makeInitDecl(ClassDecl *classDecl, std::vector<Stmt *> sets);
  std::vector<Field> interfaceFields(TypeAliasDecl *alias, std::vector<const Type *> &bases,
                                     std::vector<Overload> &overloads);

private:
  struct ParentLookup {
    ClassDecl *parent;
    Stmt *found;
    bool isInstance;
  };
  struct InterfaceShape {
    std::vector<Field> fields;
    std::vector<std::string> origins;
  };

  using ResultParamTypePassed = Result<const Type *, DeclarationClassesRegistryParamTypeError>;
  using ResultParentClassFound = Result<ClassDecl *, DeclarationClassesRegistryBaseClassError>;
  using ResultUnusedMemberName = Result<const Field *, DeclarationClassesRegistryNewFieldError>;
  using ResultFieldNarrowsDown = Result<const Field *, DeclarationClassesRegistryNarrowsFieldError>;
  using ResultFieldNotLateSet = Result<const ClassField *, DeclarationClassesRegistrySetLaterError>;
  using ResultFieldTypeIsSeen = Result<const ClassField *, DeclarationClassesRegistryTypeSeenError>;
  using ResultObjectTypeAsBase = Result<const Type *, DeclarationClassesRegistryInterfaceBaseError>;
  using ResultFieldTypesAreOne =
      Result<const Field *, DeclarationClassesRegistrySameFieldTypeError>;
  using ResultFieldHasNoValue =
      Result<const ClassField *, DeclarationClassesRegistryFieldValueError>;
  using ResultFieldTypeSource =
      Result<const ClassField *, DeclarationClassesRegistryTypedFieldError>;
  using ResultImplementedShape =
      Result<const Type *, DeclarationClassesRegistryImplementedTypeError>;
  using ResultStaticTypeFixed =
      Result<const Type *, DeclarationClassesRegistryStaticTypeParamsError>;
  using ResultUnusedMethodName =
      Result<const Overload *, DeclarationClassesRegistryNewOverloadError>;
  using ResultAccessIsNotLower =
      Result<const ClassField *, DeclarationClassesRegistryAccessKeptError>;
  using ResultFieldNameUnique =
      Result<const ClassField *, DeclarationClassesRegistryUniqueFieldError>;
  using ResultInterfaceNotSelf =
      Result<const TypeAliasDecl *, DeclarationClassesRegistryNotSelfError>;
  using ResultStaticGetsValue =
      Result<const ClassField *, DeclarationClassesRegistryStaticValueError>;
  using ResultValueOnRedeclare =
      Result<const ClassField *, DeclarationClassesRegistryValueGivenError>;
  using ResultNotParentMethod =
      Result<const ClassField *, DeclarationClassesRegistryParentMethodError>;
  using ResultFieldNameIsFree =
      Result<const ClassField *, DeclarationClassesRegistryFieldOrMethodError>;
  using ResultMethodSignatures =
      Result<const Overload *, DeclarationClassesRegistrySameSignaturesError>;
  using ResultNarrowingLayout =
      Result<const ClassField *, DeclarationClassesRegistryNarrowedFieldError>;
  using ResultVariantsUnequal =
      Result<const FunctionDecl *, DeclarationClassesRegistryVariantPairError>;
  using ResultGenericIsDirect =
      Result<const FunctionDecl *, DeclarationClassesRegistryGenericMethodError>;
  using ResultOverrideTargeted =
      Result<const ClassField *, DeclarationClassesRegistryOverrideTargetError>;
  using ResultAbstractReturns =
      Result<const FunctionDecl *, DeclarationClassesRegistryAbstractReturnError>;
  using ResultInheritedVisible =
      Result<const ClassField *, DeclarationClassesRegistryInheritedVisibleError>;

  void makeFieldInit(ClassDecl *classDecl);
  void finishCompletion(ClassDecl *classDecl);
  void fillDefaultValues(ClassDecl *classDecl);
  void resolveClassBases(ClassDecl *classDecl);
  void linkParent(ClassDecl *classDecl, ClassDecl *parent);
  void planObjectDefault(const Type *type, DefaultRun &run);
  void resolveFamily(ClassDecl *classDecl, DefaultRun &run);
  void defaultFor(const DefaultFor &wanted, DefaultRun &run);
  void resolveField(ClassDecl *classDecl, ClassField &field);
  void addImplemented(ClassDecl *classDecl, const TypeRef &ref);
  void walkMemberName(const ClassDecl *classDecl, size_t index);
  void fillObject(const FillObject &fill, DefaultRun &run) const;
  void makeEmptyNew(const ClassDecl *classDecl, DefaultRun &run);
  void walkVariantList(llvm::ArrayRef<FunctionDecl *> functions);
  void applyDefaultStep(const DefaultStep &step, DefaultRun &run);
  void addHiddenParam(FunctionDecl *functionDecl, const Type *type);
  void enterEmptyClass(ClassDecl *classDecl, DefaultRun &run) const;
  void resolveParentClass(ClassDecl *classDecl, const TypeRef &ref);
  void walkRedeclaredField(ClassDecl *classDecl, ClassField &field);
  void planClassDefault(ClassDecl *classDecl, DefaultRun &run) const;
  void numberVariants(llvm::ArrayRef<FunctionDecl *> functions) const;
  void declareFieldMethod(ClassDecl *classDecl, const ClassField &field);
  void leaveEmptyClass(const ClassDecl *classDecl, DefaultRun &run) const;
  void declareClassMethod(const ClassDecl *classDecl, FunctionDecl *method);
  void resolveInstanceFields(ClassDecl *instance, const ClassDecl *generic);
  void arrowMethod(ClassDecl *classDecl, ClassField &field, const Type *type);
  void startCompletion(ClassDecl *classDecl, std::vector<CompletionStep> &steps);
  void completeField(const ClassDecl *classDecl, ClassField &field, std::vector<Field> &fields);
  void addOwnField(const ClassDecl *classDecl, const ClassField &field, std::vector<Field> &fields);
  void narrowInheritedField(const ClassDecl *classDecl, const ClassField &field,
                            std::vector<Field> &fields);
  void mergeBaseFields(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                       InterfaceShape &shape);
  void declareStaticVar(const ClassDecl *classDecl, const ClassField &field, StaticField *shared,
                        const Type *type);
  void walkSameSignatures(const TypeAliasDecl *alias, const TypeRef &ref, const Overload &known,
                          const Overload &overload);
  void addOwnField(const TypeAliasDecl *alias, const Field &field,
                   llvm::ArrayRef<Overload> overloads, InterfaceShape &shape);
  void mergeBaseOverloads(const TypeAliasDecl *alias, const TypeRef &ref, const Type *base,
                          std::vector<Overload> &overloads);
  void mergeBaseField(const TypeAliasDecl *alias, const TypeRef &ref, const Field &field,
                      const std::string &baseName, InterfaceShape &shape);


  bool isPlainObject(const Type *type) const;
  bool isLiteralUnion(const Type *type) const;
  bool hasInternalField(const Type *type) const;
  bool isErrorClass(llvm::StringRef name) const;
  bool isMethodField(const ClassField &field) const;
  bool isFullyTyped(const FuncSymbol *function) const;
  bool isNeedingDefault(const ClassField &field) const;
  bool isMadeByConstructor(const ClassDecl *classDecl) const;
  bool isUninstantiatedGeneric(const FunctionDecl *functionDecl) const;
  bool isWrittenCollection(const Type *type, const TypeRef *ref) const;
  bool hasOtherReturn(const FuncSymbol *first, const FuncSymbol *second) const;
  bool hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isVariantOf(const FunctionDecl *earlier, const FunctionDecl *later) const;


  Expr *literalValue(const Type *literal, llvm::SMLoc loc);
  Expr *sizeLiteral(const Type *base, double size, llvm::SMLoc loc);
  Expr *simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc);
  Expr *defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                     llvm::SmallPtrSetImpl<const ClassDecl *> &visiting);


  ClassDecl *classOf(const Type *type) const;
  ClassDecl *defaultedClass(const ClassField &field) const;


  ParentLookup parentOf(ClassDecl *classDecl, const TypeRef &ref, Stmt *found);
  ParentLookup emitterParent(ClassDecl *classDecl, const TypeRef &ref, ClassDecl *emitter,
                             Stmt *found);


  const Type *writtenFieldType(const ClassDecl *classDecl, ClassField &field);
  const Type *declaredFieldType(const ClassDecl *classDecl, ClassField &field);
  const Type *resolvedFieldType(const ClassDecl *classDecl, ClassField &field);
  const Type *instanceFieldType(const ClassDecl *instance, const Type *original);
  const Type *typedParam(const Param &param, const Type *given, const Type *declared);
  const Type *interfaceBaseType(const TypeAliasDecl *alias, const TypeRef &ref,
                                llvm::ArrayRef<const Type *> bases);


  ResultParentClassFound checkUsableParent(const ClassDecl *classDecl, ClassDecl *parent) const;
  ResultParentClassFound checkMissingParent(const ClassDecl *classDecl, const TypeRef &ref,
                                            const Stmt *found) const;
  ResultParentClassFound checkBaseClass(const ClassDecl *classDecl, const TypeRef &ref,
                                        bool isNamed, const ParentLookup &lookup) const;


  Stmt *fieldSet(const ClassField &field);
  ResultFieldNotLateSet setLater(const ClassField &field) const;
  ResultFieldTypeIsSeen typeSeen(const ClassField &field) const;
  ResultObjectTypeAsBase checkInterfaceBase(const Type *base) const;
  std::vector<const Field *> requiredFields(const Type *type) const;
  std::vector<Field> inheritedFields(const ClassDecl *classDecl) const;
  ResultGenericIsDirect genericMethod(const FunctionDecl *method) const;
  Field *fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const;
  size_t fieldIndex(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  const ClassField *earlierField(const ClassDecl *classDecl, size_t index) const;
  llvm::SMLoc ownFieldLoc(const TypeAliasDecl *alias, llvm::StringRef name) const;
  ResultStaticGetsValue staticValue(const ClassField &field, const Type *type) const;
  const Param *propertyParam(const ClassDecl *classDecl, llvm::StringRef name) const;
  std::optional<int> ambiguousCount(const FuncSymbol *first, const FuncSymbol *second) const;
  ResultFieldHasNoValue fieldValue(const ClassDecl *classDecl, const ClassField &field) const;
  ResultFieldTypeSource typedField(const ClassDecl *classDecl, const ClassField &field) const;
  ResultNotParentMethod parentMethod(const ClassDecl *classDecl, const ClassField &field) const;
  const Overload *overloadNamed(llvm::ArrayRef<Overload> overloads, llvm::StringRef name) const;
  ResultVariantsUnequal variantPair(const FunctionDecl *first, const FunctionDecl *second) const;
  ResultImplementedShape checkImplementedType(const Type *type, const ClassDecl *classDecl) const;
  ResultInterfaceNotSelf checkNotSelf(const TypeAliasDecl *alias, const TypeAliasDecl *other) const;
  ResultParamTypePassed paramType(const Param &param, const Type *type, const Type *declared) const;
  ResultAbstractReturns abstractReturn(const ClassDecl *classDecl,
                                       const FunctionDecl *method) const;
  ResultOverrideTargeted checkOverrideTarget(const ClassDecl *classDecl,
                                             const ClassField &field) const;
  ResultStaticTypeFixed staticTypeParams(const ClassDecl *classDecl, const ClassField &field,
                                         const Type *type) const;
  ResultFieldNameUnique uniqueField(const ClassDecl *classDecl, const ClassField &field,
                                    const ClassField *earlier) const;
  ResultValueOnRedeclare checkValueGiven(const ClassDecl *classDecl, const ClassField &field,
                                         const ClassDecl *owner) const;
  ResultFieldNameIsFree fieldOrMethod(const ClassDecl *classDecl, const ClassField &field,
                                      const FunctionDecl *method) const;
  ResultMethodSignatures checkSameSignatures(const TypeAliasDecl *alias, const Overload &known,
                                             const Overload &other) const;
  ResultUnusedMemberName checkNewField(const TypeAliasDecl *alias, const Field &field,
                                       llvm::ArrayRef<Overload> overloads) const;
  ResultInheritedVisible checkInheritedVisible(const ClassField &field, const ClassField *inherited,
                                               const ClassDecl *owner) const;
  ResultFieldNarrowsDown checkNarrowsField(const TypeAliasDecl *alias, const Field &field,
                                           const Field &known, llvm::StringRef origin) const;
  ResultNarrowingLayout narrowedField(const ClassDecl *classDecl, const ClassField &field,
                                      const Field &inherited, const ClassDecl *owner) const;
  ResultAccessIsNotLower checkAccessKept(const ClassField &field, const ClassField *inherited,
                                         const ClassDecl *owner, const ClassDecl *classDecl) const;
  ResultFieldTypesAreOne checkSameFieldType(const TypeAliasDecl *alias, const Field &known,
                                            const Field &field, llvm::StringRef origin,
                                            llvm::StringRef baseName) const;
  ResultUnusedMethodName checkNewOverload(const TypeAliasDecl *alias, const Overload &overload,
                                          llvm::ArrayRef<Field> fields,
                                          llvm::ArrayRef<Overload> overloads) const;

  SemanticState &state;
  TypeResolver *typeResolver = nullptr;
  DeclarationModulesRegistry *modules = nullptr;
  EventMapInference *eventMapInference = nullptr;
  DeclarationFunctionsRegistry *functions = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
