#pragma once

#include "compiler/semantic/library/events/emitter_checker/emitter_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class EmitterChecker {
public:
  explicit EmitterChecker(SemanticState &owner);

  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super);
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> *given,
                          bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  Expr *collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site);
  void reportUnlistenedErrors();
  void checkErrorListeners();
  void checkDroppedListeners();
  void noteErrorsHandled(const Expr *emitter);
  bool classifyListener(Expr *arg, const Expr *emitter, int &bits, bool timer = false);
  bool ownsEmitter(const Expr *emitter) const;
  static bool canReceive(const Type *type);

private:
  enum class EmitterMethod {
    None,
    Listen,
    Remove,
    Emit,
    Listeners,
    RawListeners,
    Count,
    RemoveAll
  };

  static EmitterMethod emitterMethod(llvm::StringRef name);
  static std::string chainText(const Expr *expr);
  static bool isServiceEvent(llvm::StringRef name);
  static std::string fileOfPlace(const std::string &place);
  static std::string shortPlace(const std::string &place);

  void noteListener(MemberExpr *member, const Type *events, llvm::StringRef event);
  EmitterKey keyOf(const Expr *root) const;
  std::string emitterPath(const EmitterKey &key) const;

  SemanticState &state;
};

}
