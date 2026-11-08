#pragma once

namespace tz {

class SemanticState;
class Expr;
struct SourceFile;

class AssignedFields {
public:
  explicit AssignedFields(SemanticState &owner);

  void rememberAssigned(SourceFile &program);

private:
  void rememberTarget(const Expr *expr);

  SemanticState &state;
};

}
