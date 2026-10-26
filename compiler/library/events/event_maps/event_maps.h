#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class EventMaps {
public:
  explicit EventMaps(Checker &checker);

  bool isEmitterClass(const ClassDecl *cls) const;
  const Type *eventMapOf(const Type *type) const;
  bool checkEventMap(const Type *events, llvm::SMLoc loc);
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  static const Type *listenerType(const Type *args, const Type *ret);
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      Checker::ListenerKind &kind);
  const Type *createdType(const NewExpr *create);

private:
  Checker &checker_;
};

}
