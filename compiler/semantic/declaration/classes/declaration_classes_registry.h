#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/classes/declaration_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class DeclarationClassesRegistry {
public:
  explicit DeclarationClassesRegistry(SemanticState &owner);

  void closeClassScope();
  void walkVariants(ClassDecl *classDecl);
  void resolveClass(const ResolveClass &task);
  void completeClass(const CompleteClass &task);
  void finishMethods(const FinishMethods &task);
  void makeFieldInit(const MakeFieldInit &task);
  void declareMethods(const DeclareMethods &task);
  void finishVariants(const FinishVariants &task);
  void openClassScope(const OpenClassScope &task);
  void addBase(const Type *type, const Type *base);
  void finishFieldInit(const FinishFieldInit &task);
  void buildInstanceInit(const BuildInstanceInit &task);
  void startClassBases(const StartClassBases &task);
  void startClassField(const StartClassField &task);
  void declareItemMethods(Stmt *item, Module *module);
  void finishClassBases(const FinishClassBases &task);
  void finishClassField(const FinishClassField &task);
  void finishCompletion(const FinishCompletion &task);
  void finishLinkParent(const FinishLinkParent &task);
  void finishTypedParam(const FinishTypedParam &task);
  void nextFieldDefault(const NextFieldDefault &task);
  void startFieldMethod(const StartFieldMethod &task);
  void startTypedMethod(const StartTypedMethod &task);
  void finishClassMethod(const FinishClassMethod &task);
  void finishConstructor(const FinishConstructor &task);
  void finishFieldMethod(const FinishFieldMethod &task);
  void finishImplemented(const FinishImplemented &task);
  void finishItemMethods(const FinishItemMethods &task);
  void finishParentClass(const FinishParentClass &task);
  void finishTypedMethod(const FinishTypedMethod &task);
  void startInterfaceBase(const StartInterfaceBase &task);
  void finishInterfaceBase(const FinishInterfaceBase &task);
  void resolveClassMembers(const ResolveClassMembers &task);
  void continueFieldDefault(const ContinueFieldDefault &task);
  void startInterfaceFields(const StartInterfaceFields &task);
  void finishInterfaceFields(const FinishInterfaceFields &task);
  void reportVariantPair(FunctionDecl *first, FunctionDecl *second);


  std::vector<Stmt *> fieldSets(const ClassDecl *classDecl);
  FunctionDecl *makeInitDecl(ClassDecl *classDecl, std::vector<Stmt *> sets);

private:
  struct ParentLookup {
    ClassDecl *parent;
    Stmt *found;
    bool isInstance;
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

  void startParentClass(ClassDecl *classDecl);
  void pushSignature(FunctionDecl *functionDecl);
  void linkParent(ClassDecl *classDecl, ClassDecl *parent);
  void planObjectDefault(const Type *type, DefaultRun &run);
  void resolveFamily(ClassDecl *classDecl, DefaultRun &run);
  void defaultFor(const DefaultFor &wanted, DefaultRun &run);
  void addOwnField(const Field &field, InterfaceDraft &draft);
  void walkMemberName(const ClassDecl *classDecl, size_t index);
  void fillObject(const FillObject &fill, DefaultRun &run) const;
  void makeEmptyNew(const ClassDecl *classDecl, DefaultRun &run);
  void walkVariantList(llvm::ArrayRef<FunctionDecl *> functions);
  void applyDefaultStep(const DefaultStep &step, DefaultRun &run);
  void addHiddenParam(FunctionDecl *functionDecl, const Type *type);
  void enterEmptyClass(ClassDecl *classDecl, DefaultRun &run) const;
  void pushInterfaceBase(const InterfaceDraft &draft, size_t index);
  void walkRedeclaredField(ClassDecl *classDecl, ClassField &field);
  void planClassDefault(ClassDecl *classDecl, DefaultRun &run) const;
  void numberVariants(llvm::ArrayRef<FunctionDecl *> functions) const;
  void linkFoundParent(ClassDecl *classDecl, const ParentLookup &lookup);
  void leaveEmptyClass(const ClassDecl *classDecl, DefaultRun &run) const;
  void runFieldDefault(ClassDecl *classDecl, size_t index, DefaultRun run);
  void arrowMethod(ClassDecl *classDecl, ClassField &field, const Type *type);
  void placeClassField(ClassDecl *classDecl, ClassField &field, const Type *type);
  void mergeBaseFields(const TypeRef &ref, const Type *base, InterfaceDraft &draft);
  void mergeBaseOverloads(const TypeRef &ref, const Type *base, InterfaceDraft &draft);
  void completeField(const ClassDecl *classDecl, ClassField &field, std::vector<Field> &fields);
  void addOwnField(const ClassDecl *classDecl, const ClassField &field, std::vector<Field> &fields);
  void narrowInheritedField(const ClassDecl *classDecl, const ClassField &field,
                            std::vector<Field> &fields);
  void mergeBaseField(const TypeRef &ref, const Field &field, const std::string &baseName,
                      InterfaceDraft &draft);
  void declareStaticVar(const ClassDecl *classDecl, const ClassField &field, StaticField *shared,
                        const Type *type);
  void walkSameSignatures(const TypeAliasDecl *alias, const TypeRef &ref, const Overload &known,
                          const Overload &overload);


  bool isNamedRef(const TypeRef &ref) const;
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
  bool isEmitterParent(const ClassDecl *parent, const TypeRef &ref) const;
  bool isGenericParent(const ClassDecl *parent, const TypeRef &ref) const;
  bool hasOtherReturn(const FuncSymbol *first, const FuncSymbol *second) const;
  bool hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isVariantOf(const FunctionDecl *earlier, const FunctionDecl *later) const;


  Expr *literalValue(const Type *literal, llvm::SMLoc loc);
  Expr *sizeLiteral(const Type *base, double size, llvm::SMLoc loc);
  Expr *simpleDefault(const Type *type, const TypeRef *ref, llvm::SMLoc loc);


  ClassDecl *classOf(const Type *type) const;
  ClassDecl *defaultedClass(const ClassField &field) const;
  ClassDecl *unresolvedFamily(const DefaultStep &step) const;


  const Type *unwrittenFieldType(const ClassField &field) const;
  const Type *optionalFieldType(const ClassField &field, const Type *written) const;
  const Type *typedParam(const Param &param, const Type *given, const Type *declared);
  const Type *acceptedInterfaceBase(const TypeRef &ref, llvm::ArrayRef<const Type *> bases);


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
  TypeRef *writtenFieldRef(ClassField &field, Param *property) const;
  std::vector<Field> inheritedFields(const ClassDecl *classDecl) const;
  ResultGenericIsDirect genericMethod(const FunctionDecl *method) const;
  Param *propertyParam(ClassDecl *classDecl, llvm::StringRef name) const;
  Field *fieldNamed(std::vector<Field> &fields, llvm::StringRef name) const;
  size_t fieldIndex(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  const ClassField *earlierField(const ClassDecl *classDecl, size_t index) const;
  llvm::SMLoc ownFieldLoc(const TypeAliasDecl *alias, llvm::StringRef name) const;
  ResultStaticGetsValue staticValue(const ClassField &field, const Type *type) const;
  DefaultRun fieldDefaultRun(const ClassDecl *classDecl, const ClassField &field) const;
  ResultFieldTypeSource typedField(const ClassField &field, const Param *property) const;
  std::optional<int> ambiguousCount(const FuncSymbol *first, const FuncSymbol *second) const;
  ResultFieldHasNoValue fieldValue(const ClassDecl *classDecl, const ClassField &field) const;
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
};

}
