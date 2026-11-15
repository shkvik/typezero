#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/events/library_events_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryEventsRegistry {
public:
  explicit LibraryEventsRegistry(SemanticState &owner);

  void finishSentEmits();
  void walkErrorListeners();
  void afterCreatedInstance();
  void walkDroppedListeners();
  void reportUnlistenedErrors();
  void createdType(const CreatedType &task);
  void completeSite(const CompleteSite &task);
  void eventArgsOf(const EventArgsOf &task);
  void afterEventName(const AfterEventName &task);
  void completeEvent(const CompleteEvent &task);
  void findEventArgs(const FindEventArgs &task);
  void endEventTrial(const EndEventTrial &task);
  void setCheckedType(const SetCheckedType &task);
  void afterEmitArgs(const AfterEmitArgs &task);
  void afterEmitEvent(const AfterEmitEvent &task);
  void nextEmitValue(const NextEmitValue &task);
  void afterEmitValue(const AfterEmitValue &task);
  void beginEventTrial(const BeginEventTrial &task);
  void answerEventArgs(const AnswerEventArgs &task);
  void afterWaitEvent(const AfterWaitEvent &task);
  void afterQueryEvent(const AfterQueryEvent &task);
  void afterWaitErrors(const AfterWaitErrors &task);
  void checkEmitterNew(const CheckEmitterNew &task);
  void afterCreatedArgs(const AfterCreatedArgs &task);
  void tryEventFunction(const TryEventFunction &task);
  void classifyListener(const ClassifyListener &task);
  void reportNotEmitted(const ReportNotEmitted &task);
  void afterListenerArg(const AfterListenerArg &task);
  void checkEmitterCall(const CheckEmitterCall &task);
  void afterSiteEmitKey(const AfterSiteEmitKey &task);
  void finishTargetCall(const FinishTargetCall &task);
  void afterTargetValue(const AfterTargetValue &task);
  void afterEventsObject(const AfterEventsObject &task);
  void afterEventsMethod(const AfterEventsMethod &task);
  void finishEmitterCall(const FinishEmitterCall &task);
  void inferredEventArgs(const InferredEventArgs &task);
  void tryEventStatement(const TryEventStatement &task);
  void noteErrorsHandled(const NoteErrorsHandled &task);
  void afterListenerThis(const AfterListenerThis &task);
  void afterEmitterParent(const AfterEmitterParent &task);
  void afterEmitterEvents(const AfterEmitterEvents &task);
  void afterInferredEvent(const AfterInferredEvent &task);
  void afterEmittedValue(const AfterEmittedValue &task);
  void afterListenerValue(const AfterListenerValue &task);
  void checkEventsFunction(const CheckEventsFunction &task);
  void finishCompleteEvent(const FinishCompleteEvent &task);
  void afterListenerErrors(const AfterListenerErrors &task);
  void checkEventTargetCall(const CheckEventTargetCall &task);
  void afterEmitterInstance(const AfterEmitterInstance &task);
  void afterTargetParamType(const AfterTargetParamType &task);
  void afterTargetObjectType(const AfterTargetObjectType &task);
  void finishListenerLiteral(const FinishListenerLiteral &task);
  void checkEventsLibraryCall(const CheckEventsLibraryCall &task);
  void instantiateEmitterParent(const InstantiateEmitterParent &task);
  void reportEmitterMethodValue(const ReportEmitterMethodValue &task);

private:
  enum class ListenerKind { Plain, Async, This };

  enum class ListenerForm { Free, Receiver, Captures, Unknown };

  struct ListenerShape {
    ListenerForm form;
    const Type *type;
    const VarSymbol *var;
    FunctionDecl *envFn;
    std::vector<const VarSymbol *> captured;
  };

  using ResultListenerThis = Result<const Type *, LibraryEventsRegistryListenerThisError>;
  using ResultListenerEvent = Result<const Type *, LibraryEventsRegistryListenerEventError>;
  using ResultEmitterEvents = Result<const Type *, LibraryEventsRegistryEmitterEventsError>;
  using ResultOneErrorValue = Result<const Type *, LibraryEventsRegistryOneErrorValueError>;

  void walkLooseArgs(CallExpr *call, size_t from);
  void failEmitterNew(NewExpr *newExpr);
  void finishLoosely(CallExpr *call, size_t from);
  void defineMapFields(EventSite *site) const;
  void targetListeners(CallExpr *call, Expr *object);
  void finishRemoveAll(const EmitterDraft &emitter);
  void checkEmit(const EmitterDraft &emitter, bool isLibrary);
  void checkTargetWait(const EventsFunctionDraft &draft);
  void pushFinishEmit(const SentEmit &emit, const Type *args);
  void checkThisValue(Expr **slot, const ListenerWantDraft &want);
  void pushListenerCheck(Expr **slot, const ListenerWantDraft &want);
  void checkEmitterWait(const EventsFunctionDraft &draft, const Type *events);
  void pushTargetFunction(const TargetCallDraft &target, const Type *kind);
  void checkEmitShape(const EmitterDraft &emitter, const EmitShapeDraft &shape, bool isLibrary);
  void finishListeners(const EmitterDraft &emitter, EmitterMethodKind kind, const Type *args);
  void checkEmitterMethod(const EmitterDraft &emitter, EmitterMethodKind kind, bool isLibrary);
  void addModuleCandidates(const EventSite *site, Module *module,
                           std::vector<EmitCandidate> &found);
  void noteListener(MemberExpr *member, const Type *events, llvm::StringRef event,
                    const Type *errors);
  void rememberEventArgs(EventSite *site, EventRecord *record, const Type *args, llvm::SMLoc loc);
  void collectEmitterCall(const SentEmit &emit, EventSite *site, const std::string &event,
                          bool isLibrary);
  void finishCollectedEmit(const AfterEmittedValue &collected,
                           const std::vector<const Type *> &elements);
  void finishEmitShape(const EmitterDraft &emitter, const EmitShapeDraft &shape,
                       const std::vector<bool> &given);
  void startEmitterQuery(const EmitterDraft &emitter, EmitterMethodKind kind, unsigned min,
                         unsigned max);
  void checkListenerArg(const EmitterDraft &emitter, EmitterMethodKind kind, const Type *args,
                        const std::string &event);
  void finishEmitterMethod(const EmitterDraft &emitter, llvm::StringRef name,
                           std::vector<const Type *> bound, bool isTyping);
  void pushListenerLiteral(Expr **slot, FunctionExpr *functionExpr, const Type *wanted,
                           Signature signature);
  void addCandidates(const Stmt *where, Module *module, FunctionDecl *functionDecl, size_t index,
                     std::vector<EmitCandidate> &found) const;
  void addTrial(const EmitCandidate &candidate,
                llvm::SmallPtrSetImpl<const FunctionDecl *> &functions,
                llvm::SmallSet<size_t, 4> &statements, std::vector<CheckTask> &trials);


  bool isEventTarget(const Type *type) const;
  bool canReceive(const Type *type) const;
  bool isClassExpr(const Expr *expr) const;
  bool isMentioningParam(const Type *type) const;
  bool isWaitEnding(const CallExpr *call) const;
  bool isHoldingClassExpr(const Stmt *where) const;
  bool isReceiverKeyword(llvm::StringRef name) const;
  bool isFrozen(const EventRecord *record) const;
  bool hasTimeoutOption(const CallExpr *call) const;
  bool isOwningEmitter(const Expr *emitter) const;
  bool hasSelfListener(const ClassDecl *classDecl) const;
  bool canTryStatement(const EmitCandidate &candidate);
  bool canTryFunction(const FunctionDecl *functionDecl) const;
  bool reportUnknownListener(const Expr *arg, bool timer);
  bool isParamFitting(const Type *param, const Type *wanted) const;
  bool isParamOf(const VarSymbol *variable, const Param &param) const;
  bool isOwnEmit(const EventSite *site, const SentEmit &emit) const;
  bool isSubclassEmit(const EventSite *site, const EmitCandidate &candidate) const;
  bool addListenerReceiver(const ListenerShape &shape, bool topLevel, int &bits);
  bool classifyArg(Expr *arg, const Expr *emitter, int &bits, ListenerUse use);
  bool isListenerTakingThis(const ListenerWantDraft &want, const Type *self, llvm::SMLoc loc);
  bool reportCapturesKept(const Expr *arg, const std::vector<const VarSymbol *> &captured,
                          bool timer);


  Type *newEventMap(llvm::SMLoc loc);
  Type *classEventMap(const ClassDecl *classDecl);
  Type *siteMap(const std::string &key, const std::string &name, Module *owner,
                const ClassDecl *classDecl);


  const Expr *emitterLink(const Expr *expr) const;
  const Expr *emitterRoot(const Expr *emitter) const;


  std::string shortPlaceOf(llvm::SMLoc loc) const;
  std::string emitterPath(const EmitterKey &key) const;
  std::string fileOfPlace(const std::string &place) const;
  std::string eventLabel(const Type *events, const Expr *name) const;


  std::optional<std::string> chainText(const Expr *expr) const;
  std::optional<std::string> rootText(const Expr *expr) const;


  ListenerShape listenerShape(Expr *expr) const;
  ListenerShape closureShape(FunctionDecl *functionDecl) const;
  ListenerShape functionShape(FunctionDecl *functionDecl) const;


  EventRecord *findEvent(EventSite *site, llvm::StringRef name) const;
  EventRecord *addEvent(EventSite *site, llvm::StringRef name);


  std::vector<EmitCandidate> getLateCandidates();
  std::vector<EmitCandidate> getSiteCandidates(const EventSite *site);


  ResultListenerThis checkListenerThis(const Type *self, const Type *receiver) const;
  ResultListenerEvent checkListenerEvent(const Type *type, const ClassDecl *eventClass) const;
  ResultOneErrorValue checkOneErrorValue(const Type *errors, llvm::StringRef functionName) const;
  ResultEmitterEvents checkEmitterEvents(const Type *type, llvm::StringRef functionName,
                                         bool target);


  EmitterKey keyOf(const Expr *root) const;
  Module *findModuleAt(llvm::SMLoc loc) const;
  Signature signatureOf(const Type *functionType) const;
  FunctionDecl *namedFunction(const Expr *expr) const;
  Expr *adaptedListener(Expr *arg, const Type *to);
  EmitterMethodKind emitterMethod(llvm::StringRef name) const;
  std::vector<bool> waitGiven(const CallExpr *call) const;
  std::optional<EmitCandidate> emitCandidate(const Expr *expr) const;
  ListenerKind listenerKindOf(const Expr *original, const Expr *checked) const;
  const std::vector<EmitCandidate> &getModuleCandidates(Module *module);
  std::vector<FunctionDecl *> classMethods(const ClassDecl *classDecl) const;
  const Type *withThis(const ListenerWantDraft &want, const Type *self) const;
  llvm::StringRef waitVariant(const EventsFunctionDraft &draft, bool hasErrors) const;
  const Type *listenedEvent(const Type *type, llvm::SMLoc loc, ClassDecl *eventClass);

  SemanticState &state;
};

}
