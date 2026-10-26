#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

class FlowCalls {
public:
  explicit FlowCalls(Flows &flows);

  bool mayCall(const Type *type, const FunctionDecl *fn) const;
  void functionValue(const FunctionDecl *fn);
  Refs receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                          const Refs &refs, const VarSymbol *initializes, std::string role);
  void passArguments(const CallExpr *call, const FunctionDecl *target,
                     const std::vector<Refs> &args);
  void passThis(const Refs &receiver, const FunctionDecl *method);
  std::vector<const FunctionDecl *> overridersOf(const FunctionDecl *fn);
  Refs call(CallExpr *c);
  void callback(const Expr *arg, const Refs &values);

private:
  Flows &flows_;
};

}
