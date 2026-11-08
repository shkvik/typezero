#pragma once

namespace tz {

class CheckState;
class Expr;
struct SourceFile;

class AssignedFields {
public:
  explicit AssignedFields(CheckState &owner);

  void rememberAssigned(SourceFile &program);

private:
  void rememberTarget(const Expr *expr);

  CheckState &state;
};

}
