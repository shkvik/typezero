#pragma once

#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/program/library/reflect/builtin_constructor_table/builtin_constructor_table.h"
#include "compiler/program/library/reflect/design_key_table/design_key_table.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/reflect/library_reflect_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryReflectRegistry {
public:
  explicit LibraryReflectRegistry(SemanticState &owner);

  void reflectOk();
  void finishReflection();
  void rememberWriters(SourceFile &program);
  void finishFill(const FinishFill &task);
  void afterFillArgs(const AfterFillArgs &task);
  void finishApply(const FinishApply &task);
  void afterApplyThis(const AfterApplyThis &task);
  void reflectArgsSlot(const ReflectArgsSlot &task);
  void checkReflectCall(const CheckReflectCall &task);
  void finishReflectOk(const FinishReflectOk &task);
  void afterReflectArgs(const AfterReflectArgs &task);
  void continueKnownGet(const ContinueKnownGet &task);
  void finishDynamicGet(const FinishDynamicGet &task);
  void makeVoidSequence(const MakeVoidSequence &task);
  void afterDirectMember(const AfterDirectMember &task);
  void finishApplyResult(const FinishApplyResult &task);
  void afterApplyFunction(const AfterApplyFunction &task);
  void afterFieldComplete(const AfterFieldComplete &task);
  void afterKnownMetadata(const AfterKnownMetadata &task);
  void afterMetadataValue(const AfterMetadataValue &task);
  void finishMetadataCall(const FinishMetadataCall &task);
  void nextMetadataWriter(const NextMetadataWriter &task);
  void writeDesignRecords(const WriteDesignRecords &task);
  void afterConstructClass(const AfterConstructClass &task);
  void afterMetadataKeyArg(const AfterMetadataKeyArg &task);
  void afterMetadataTarget(const AfterMetadataTarget &task);
  void noteConstructorFlow(const NoteConstructorFlow &task);
  void afterDirectArguments(const AfterDirectArguments &task);
  void metadataValueUnknown(const MetadataValueUnknown &task);
  void afterReflectGetObject(const AfterReflectGetObject &task);
  void finishDynamicConstruct(const FinishDynamicConstruct &task);
  void noteReflectedConversion(const NoteReflectedConversion &task);

private:
  struct NamedKey {
    const void *key;
    std::string label;
  };

  struct KnownMethod {
    FunctionDecl *method;
    bool hasSeveral;
  };

  struct MetadataFunction {
    Builtin builtin;
    unsigned min;
    unsigned max;
  };

  struct ConstructorEntry {
    ClassDecl *classDecl;
    const Type *constructed;
  };

  struct SlotConversion {
    Expr **slot;
    const Type *to;
  };

  struct DesignRecord {
    DesignKey key;
    Expr *value;
    llvm::StringRef member;
    llvm::SMLoc loc;
  };

  using TypeSet = llvm::SmallPtrSet<const Type *, 8>;
  using ReachMap = llvm::DenseMap<const Type *, TypeSet>;
  using ResultKeyOfRecords = Result<const Type *, LibraryReflectRegistryKeyOfRecordsError>;
  using ResultTargetObject = Result<const Type *, LibraryReflectRegistryTargetObjectError>;
  using ResultArgumentList = Result<const Type *, LibraryReflectRegistryArgumentListError>;
  using ResultMetadataTyped = Result<const Type *, LibraryReflectRegistryMetadataTypeError>;
  using ResultConstructible = Result<ClassDecl *, LibraryReflectRegistryConstructibleError>;
  using ResultFillableClass = Result<ClassDecl *, LibraryReflectRegistryFillableClassError>;

  void walkReflectNews();
  void startDynamicGet(const ReflectGetDraft &get);
  void startKnownGet(const ReflectGetDraft &get);
  void walkAppliedFunction(CallExpr *call);
  void walkBodyEarly(FunctionDecl *functionDecl);
  void startReflectGet(CallExpr *call, MemberExpr *member);
  void startReflectFill(CallExpr *call, MemberExpr *member);
  void startReflectApply(CallExpr *call, MemberExpr *member);
  void noteWidening(const Type *source, const Type *target);
  void noteReflectedRead(const Type *type, const Type *base);
  void markReflectMethods(ClassDecl *classDecl) const;
  void rememberWritersIn(FunctionDecl *functionDecl);
  void finishUnrecorded(const MetadataCallDraft &draft);
  void startMetadataValue(const MetadataCallDraft &draft);
  void continueMetadataCall(const MetadataCallDraft &draft);
  void startDynamicConstruct(CallExpr *call, const Type *type);
  void fillReachable(const Type *start, TypeSet &reached) const;
  void rememberWriter(FunctionDecl *functionDecl, const Expr *expr);
  void pushConversions(const std::vector<SlotConversion> &conversions);
  void startDirectApply(CallExpr *call, const FunctionDecl *functionDecl);
  void rememberMetadataType(const Expr *value, const MetadataKeyDraft &key);
  void startReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) const;
  void reportReflectName(CallExpr *call, MemberExpr *member, ReflectFunction function);
  void failReflectName(CallExpr *call, MemberExpr *member, const std::string &message);
  void startReflectConstruct(CallExpr *call, MemberExpr *member, const Type *expected);
  void constructorsIn(const Type *type, llvm::SmallVectorImpl<const Type *> &out) const;
  void writeMethodRecords(ClassDecl *classDecl, const FunctionDecl *method, llvm::SMLoc loc);
  void readClassField(Expr *seen, const StringLitExpr *key, const std::string &name);
  void pushKnownMetadataCoerce(Expr **slot, const MetadataType *known, const std::string &label);
  void readNamedField(const NameExpr *object, const StringLitExpr *key, const std::string &name);
  void walkReflectedNew(const ReflectedNew &reflected, llvm::ArrayRef<ConstructorEntry> entries,
                        ReachMap &reach);
  void startLiteralConstruct(CallExpr *call, const NameExpr *name, const ArrayLitExpr *list,
                             const Type *expected);
  void readReflectMethod(const ReflectGetDraft &get, Expr *seen, ClassDecl *classDecl,
                         const std::string &name);
  void writeDesignRecord(ClassDecl *classDecl, const DesignRecord &record,
                         std::vector<SlotConversion> &conversions);
  void startMetadataCall(CallExpr *call, MemberExpr *member, const Type *expected,
                         MetadataFunction function);


  bool isDesignKey(const Expr *key) const;
  bool hasSpread(const ArrayLitExpr *list) const;
  bool isInstanceOf(const Type *made, const Type *of) const;
  bool isDefiningMetadata(const CallExpr *call) const;
  bool isGenericMethod(const FunctionDecl *functionDecl) const;
  bool isReflectAccepting(const Type *arg, const Type *param) const;
  bool isPlainAccepting(const Type *arg, const Type *param) const;
  bool isObjectAccepting(const Type *arg, const Type *param) const;
  bool isClassRefAccepting(const Type *arg, const Type *param) const;
  bool hasOwnMethodNamed(const ClassDecl *owner, llvm::StringRef name) const;
  bool isDeclaredBelow(const ClassDecl *classDecl, llvm::StringRef name) const;
  bool isReflectCallable(ClassDecl *classDecl, FunctionDecl *functionDecl) const;
  bool canApplyDirectly(const CallExpr *call, const FunctionDecl *functionDecl) const;
  bool hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);


  Expr *stripImplicit(Expr *expr) const;
  Expr *designClass(const Type *type, llvm::SMLoc loc);
  Expr *classValueAt(ClassDecl *classDecl, llvm::SMLoc loc);
  Expr *memberProperty(llvm::StringRef member, llvm::SMLoc loc);


  const Type *designPlain(const Type *type) const;
  const Type *designType(const Expr *key) const;
  const Type *appliedResult(const Type *type) const;


  ClassDecl *seenClass(const Type *seenType) const;
  ClassDecl *plainConstructor(const Type *type);
  ClassDecl *designConstructor(const Type *type);


  std::vector<FunctionDecl *> functionsOf(Stmt *item) const;
  std::vector<FunctionDecl *> constructorsOf(const ClassDecl *classDecl) const;


  NamedKey metadataKey(const Expr *key) const;
  const void *entryKey(const ScopeEntry &entry) const;
  MetadataKeyDraft metadataKeyOf(CallExpr *call);
  const FunctionDecl *heldReflectMethod(const Expr *expr);
  const TypeSet &reachableFrom(const Type *start, ReachMap &reach) const;
  std::vector<ConstructorEntry> constructorEntries() const;
  std::optional<std::string> asciiName(const std::u16string &text) const;
  std::optional<BuiltinConstructor> builtinDesign(const Type *type) const;
  std::optional<PreludeType> fillFailure(const Expr *cls, const Type *type);
  std::optional<KnownMethod> knownReflectMethod(ClassDecl *classDecl, llvm::StringRef name);
  std::vector<ClassDecl *> reachingClasses(const Type *made,
                                           llvm::ArrayRef<ConstructorEntry> entries,
                                           ReachMap &reach) const;
  ArrayLitExpr *paramClasses(const FunctionDecl *functionDecl, llvm::SMLoc loc,
                             std::vector<SlotConversion> &conversions);
  ResultKeyOfRecords checkKeyOfRecords(const Type *type) const;
  ResultTargetObject checkTargetObject(const Type *type) const;
  ResultArgumentList checkArgumentList(const Type *type) const;
  ResultFillableClass checkFillableClass(ClassDecl *made) const;
  ResultMetadataTyped checkWrittenMetadata(const Type *expected, llvm::StringRef label) const;
  ResultConstructible checkConstructible(ClassDecl *classDecl, llvm::ArrayRef<const Type *> args);

  SemanticState &state;
};

}
