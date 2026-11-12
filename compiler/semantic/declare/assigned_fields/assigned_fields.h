#pragma once

namespace tz {

class SemanticParts;
class KeyedFieldChecker;

class SemanticState;
class Expr;
struct SourceFile;

class AssignedFields {
public:
  AssignedFields(SemanticState &owner, SemanticParts &all);

  void rememberAssigned(SourceFile &program);

private:
  void rememberTarget(const Expr *expr);
  const Expr *changedTarget(const Expr *expr) const;

  SemanticState &state;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
