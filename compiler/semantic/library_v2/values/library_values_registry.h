#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/values/library_values_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryValuesRegistry {
public:
  explicit LibraryValuesRegistry(SemanticState &owner);

  void markFused(const MarkFused &task);
  void afterUriArg(const AfterUriArg &task);
  void afterCodeArg(const AfterCodeArg &task);
  void finishAtCall(const FinishAtCall &task);
  void afterConcatArg(const AfterConcatArg &task);
  void afterDigitsArg(const AfterDigitsArg &task);
  void afterNumberArg(const AfterNumberArg &task);
  void afterTakeCount(const AfterTakeCount &task);
  void checkArrayFrom(const CheckArrayFrom &task);
  void checkMapStatic(const CheckMapStatic &task);
  void finishNumberOf(const FinishNumberOf &task);
  void afterListingArg(const AfterListingArg &task);
  void checkDictMethod(const CheckDictMethod &task);
  void checkGlobalCall(const CheckGlobalCall &task);
  void checkObjectCall(const CheckObjectCall &task);
  void afterEnumListing(const AfterEnumListing &task);
  void afterGroupSource(const AfterGroupSource &task);
  void afterListedClass(const AfterListedClass &task);
  void afterListingEnum(const AfterListingEnum &task);
  void checkArrayMethod(const CheckArrayMethod &task);
  void afterEntriesPairs(const AfterEntriesPairs &task);
  void afterListedFields(const AfterListedFields &task);
  void checkIteratorFrom(const CheckIteratorFrom &task);
  void checkNumberMethod(const CheckNumberMethod &task);
  void checkNumberStatic(const CheckNumberStatic &task);
  void checkStringMethod(const CheckStringMethod &task);
  void finishGrowingCall(const FinishGrowingCall &task);
  void afterEntriesSource(const AfterEntriesSource &task);
  void afterGroupCallback(const AfterGroupCallback &task);
  void afterSetCombineArg(const AfterSetCombineArg &task);
  void checkArrayIterator(const CheckArrayIterator &task);
  void finishGlobalResult(const FinishGlobalResult &task);
  void finishStringMethod(const FinishStringMethod &task);
  void afterStringRegexTry(const AfterStringRegexTry &task);
  void checkIteratorMethod(const CheckIteratorMethod &task);
  void afterArrayFromMapper(const AfterArrayFromMapper &task);
  void afterArrayFromSource(const AfterArrayFromSource &task);
  void afterReduceInitValue(const AfterReduceInitValue &task);
  void afterIteratorCallback(const AfterIteratorCallback &task);
  void afterIteratorFromSource(const AfterIteratorFromSource &task);

private:
  using ResultSortableElement = Result<const Type *, LibraryValuesRegistrySortableError>;
  using ResultElementsAreText = Result<const Type *, LibraryValuesRegistryTextElementsError>;
  using ResultFunctionCalled = Result<Expr *, LibraryValuesRegistryCheckGlobalFunctionError>;
  using ResultNumberSource = Result<const Type *, LibraryValuesRegistryCheckGlobalFunctionError>;
  using ResultConversionMade = Result<Expr *, LibraryValuesRegistryCheckConversionError>;
  using ResultStringMethod =
      Result<const StringMethodInfo *, LibraryValuesRegistryStringMethodError>;
  using ResultDictObjectCalled = Result<Expr *, LibraryValuesRegistryCheckDictObjectCallError>;
  using ResultCallbackIsPassed = Result<size_t, LibraryValuesRegistryCallbackGivenError>;
  using ResultNextValueOneType = Result<const Type *, LibraryValuesRegistryNextValueError>;
  using ResultInitialValueType = Result<const Type *, LibraryValuesRegistryInitialValueError>;
  using ResultFlatMapInnerType = Result<const Type *, LibraryValuesRegistryFlatMapInnerError>;
  using ResultCallbackHasValue = Result<const Type *, LibraryValuesRegistryCallbackValueError>;
  using ResultNotThrowOrReturn = Result<llvm::StringRef, LibraryValuesRegistryPlainMethodError>;
  using ResultSourceIsIterable = Result<const Type *, LibraryValuesRegistryIteratedSourceError>;
  using ResultKnownHelperFound = Result<Builtin, LibraryValuesRegistryIteratorHelperError>;
  using ResultArrayStaticKnown = Result<const MemberExpr *, LibraryValuesRegistryArrayStaticError>;
  using ResultEntriesRecord = Result<const Type *, LibraryValuesRegistryCheckFromEntriesError>;
  using ResultGroupKeyType = Result<const Type *, LibraryValuesRegistryGroupKeyError>;
  using ResultListedType = Result<const Type *, LibraryValuesRegistryListedTypeError>;

  struct OwnedField {
    const Type *owner;
    const Field *field;
  };

  void pushLooseArgs(CallExpr *call);
  void walkCodePoint(const Expr *arg);
  void setNumberOfType(CallExpr *call);
  void startAtCall(const ArrayMethodDraft &draft);
  void walkRepeatCount(const CallExpr *call);
  void checkSortCall(const ArrayMethodDraft &draft);
  void startFillCall(const ArrayMethodDraft &draft);
  void startJoinCall(const ArrayMethodDraft &draft);
  void startDictForEach(const DictDraft &dict);
  void startSliceCall(const ArrayMethodDraft &draft);
  void startConcatCall(const ArrayMethodDraft &draft);
  void startSearchCall(const ArrayMethodDraft &draft);
  void startSpliceCall(const ArrayMethodDraft &draft);
  void checkReverseCall(const ArrayMethodDraft &draft);
  void startArrayMethod(const ArrayMethodDraft &draft);
  void startGrowingCall(const ArrayMethodDraft &draft);
  void checkToStringCall(const ArrayMethodDraft &draft);
  void checkShrinkingCall(const ArrayMethodDraft &draft);
  void startFromCodes(CallExpr *call, Builtin builtin);
  void startObjectListing(const ListingDraft &listing);
  void startRegExpCall(CallExpr *call, NameExpr *name);
  void startToFixed(CallExpr *call, MemberExpr *member);
  void startNumberCall(const GlobalFunctionDraft &draft);
  void startStringCall(const GlobalFunctionDraft &draft);
  void startBooleanCall(const GlobalFunctionDraft &draft);
  void startCallbackArgument(IteratorCallbackDraft draft);
  void startStringRaw(CallExpr *call, MemberExpr *member);
  void pushPushedArg(const ArrayMethodDraft &draft, size_t index);
  void shrinkLength(const VarSymbol *self, bool isNotEmpty);
  void startNumberParse(CallExpr *call, MemberExpr *member);
  void startPlainString(CallExpr *call, MemberExpr *member);
  void startRadixString(CallExpr *call, MemberExpr *member);
  void pushElementArg(const ArrayMethodDraft &draft, size_t index);
  void startGlobalFunction(const GlobalFunctionDraft &draft);
  void startNumberStatic(CallExpr *call, MemberExpr *member);
  void startStringStatic(CallExpr *call, MemberExpr *member);
  void pushPositionArgs(const ArrayMethodDraft &draft, size_t from);
  void startEntriesSource(CallExpr *call, const Type *record);
  void finishCallbackResult(const IteratorCallbackDraft &draft);
  void pushStringArgs(CallExpr *call, llvm::StringRef argKinds);
  void setDictCallType(const DictDraft &dict, const Type *type);
  void walkArrayFromSends(const CallExpr *call, const Type *type);
  void setArrayCallType(const ArrayMethodDraft &draft, const Type *type);
  void setListingType(const ListingDraft &listing, const Type *value);
  void startListedFields(const ListingDraft &listing, const Type *type);
  void startToArray(CallExpr *call, MemberExpr *member, Builtin builtin);
  void startTakeDrop(CallExpr *call, MemberExpr *member, Builtin builtin);
  void startDictMethod(const DictDraft &dict, const DictMethodInfo &found);
  void startGroupBy(CallExpr *call, MemberExpr *member, GroupByOwner owner);
  void finishEntries(CallExpr *call, const Type *record, const Type *element);
  void startIteratorStep(CallExpr *call, MemberExpr *member, Builtin builtin);
  void finishMapResult(const IteratorCallbackDraft &draft, const Type *result);
  void addOwnedFields(const Type *owner, std::vector<OwnedField> &fields) const;
  void startNumberPredicate(CallExpr *call, MemberExpr *member, Builtin builtin);
  void startFromEntries(CallExpr *call, MemberExpr *member, const Type *expected);
  void finishFlatMapResult(const IteratorCallbackDraft &draft, const Type *result);
  void reportNoProperty(CallExpr *call, MemberExpr *member, llvm::StringRef owner);
  void walkMethodRange(CallExpr *call, const MemberExpr *member, StringMethod method);
  void reportAfterLooseArgs(CallExpr *call, llvm::SMLoc loc, const std::string &message);
  void startParseCall(const GlobalFunctionDraft &draft, std::optional<GlobalName> global);
  void startIteratorCallback(CallExpr *call, MemberExpr *member, IteratorMethod method,
                             Builtin builtin);
  void startUriCall(const GlobalFunctionDraft &draft, std::optional<GlobalName> global,
                    Builtin builtin);


  bool isHoldingJson(const Type *type) const;
  bool isHoldingUnknown(const Type *type) const;
  bool isCallbackMethod(ArrayMethod method) const;
  bool isGlobalFunction(const NameExpr *name) const;
  bool hasTextArity(const ArrayMethodDraft &draft, unsigned max);
  bool areAllTextual(llvm::ArrayRef<const Type *> members) const;
  bool hasDictArity(const DictDraft &dict, unsigned min, unsigned max);
  bool hasArrayArity(const ArrayMethodDraft &draft, unsigned min, unsigned max);
  bool isLengthFact(const VarSymbol *self, const Facts::Fact &fact) const;
  bool isDictMethodOn(const DictMethodInfo &method, const Type *type) const;
  bool hasGlobalArity(const GlobalFunctionDraft &draft, unsigned min, unsigned max);
  bool hasIteratorArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);


  std::string shownNumber(const Expr *object) const;
  std::string sortMessage(LibraryValuesRegistrySortableErrorCode code, const Type *element) const;
  std::string recordMethodMessage(const CallExpr *call, const MemberExpr *member,
                                  const std::string &text) const;


  const Type *resultType(StringResult result) const;
  const Type *stepResultType(const Type *value) const;
  const Type *conversionTarget(llvm::StringRef name) const;
  const Type *iteratedDictType(const DictDraft &dict, DictMethod method) const;
  const Type *callbackReturn(IteratorMethod method, const Type *accumulator) const;
  const Type *arrayIteratorType(const ArrayMethodInfo *method, const Type *element) const;
  const Type *objectCallType(std::optional<GlobalMember> member, const Type *value) const;


  Expr *markStageFused(CallExpr *call) const;
  Expr *enumListElement(const CallExpr *call, const EnumDecl *enumDecl,
                        const EnumMember &enumMember, GlobalMember listing);


  std::optional<Builtin> helperBuiltin(const MemberExpr *member);
  std::optional<Builtin> uriBuiltin(std::optional<GlobalName> global) const;


  ResultConversionMade checkConversion(CallExpr *call, MemberExpr *member);
  ResultConversionMade checkConversionAllowed(const MemberExpr *member, const Type *to) const;


  Builtin arrayIteratorBuiltin(const ArrayMethodInfo *method) const;
  Builtin objectCallBuiltin(std::optional<GlobalMember> member) const;


  CallExpr *soleCallArg(const CallExpr *call) const;
  const VarSymbol *arrayVariable(const Expr *object) const;
  ResultNumberSource checkNumberSource(const Type *type) const;
  ResultSortableElement checkSortable(const Type *element) const;
  ResultStringMethod checkMethodName(llvm::StringRef name) const;
  ResultEntriesRecord checkEntriesRecord(const Type *element) const;
  ResultGroupKeyType checkGroupKey(const GroupByDraft &draft) const;
  ResultNextValueOneType checkNextValue(const Type *iterator) const;
  ResultFlatMapInnerType checkFlatMapInner(const Type *result) const;
  ResultNotThrowOrReturn checkPlainMethod(llvm::StringRef name) const;
  ResultArrayStaticKnown checkArrayStatic(const MemberExpr *member) const;
  ResultInitialValueType checkInitialValue(const Type *accumulator) const;
  ResultDictObjectCalled checkDictObjectCall(CallExpr *call, llvm::StringRef functionName);
  ResultElementsAreText checkTextElements(const Type *element, llvm::StringRef name) const;
  ResultFunctionCalled checkNotLegacyEscape(CallExpr *call, std::optional<GlobalName> global);
  ResultKnownHelperFound checkIteratorHelper(llvm::StringRef name, const Type *iterator) const;
  std::optional<LibraryValuesRegistrySortableErrorCode> sortProblem(const Type *element) const;
  ResultSourceIsIterable checkIteratedSource(const Type *type, llvm::StringRef functionName) const;
  ResultCallbackHasValue checkCallbackValue(const Type *result, llvm::StringRef functionName) const;
  ResultCallbackIsPassed checkCallbackGiven(size_t count, llvm::StringRef name,
                                            bool isReduce) const;
  Signature callbackSignature(IteratorMethod method, const Type *element,
                              const Type *accumulator) const;
  ResultListedType checkListedElement(const Type *type, llvm::StringRef functionName,
                                      const std::vector<OwnedField> &fields) const;

  SemanticState &state;
};

}
