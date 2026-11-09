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

  SemanticState &state;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
