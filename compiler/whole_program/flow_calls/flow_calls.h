#pragma once

#include "compiler/whole_program/flows/flows.h"

namespace tz {

// The flows of calls: arguments, this and what they return.
class FlowCalls {
public:
  explicit FlowCalls(Flows &flows);

  // Whether a value of the function type `type` may be `fn`: the types of
  // what it passes go to its parameters. Types with type parameters, and
  // unknown, may be anything.
  bool mayCall(const Type *type, const FunctionDecl *fn) const;
  void functionValue(const FunctionDecl *fn);
  // A conversion a call makes of `this`: an object of a class that narrows
  // fields, given to a method of a class up its chain.
  Refs receiverConversion(const Expr *at, const Type *from, const FunctionDecl *method,
                          const Refs &refs, const VarSymbol *initializes, std::string role);
  // The parameters a call gives `args` to.
  void passArguments(const CallExpr *call, const FunctionDecl *target,
                     const std::vector<Refs> &args);
  // What a call of the method gives `this`: the method, and the ones that
  // override it, which a call through the method table may run.
  void passThis(const Refs &receiver, const FunctionDecl *method);
  std::vector<const FunctionDecl *> overridersOf(const FunctionDecl *fn);
  Refs call(CallExpr *c);
  // A function given as an argument: its parameters get `values`, as the
  // callee may call it with them; what it returns flows back.
  void callback(const Expr *arg, const Refs &values);

private:
  Flows &flows_;
};

} // namespace tz
