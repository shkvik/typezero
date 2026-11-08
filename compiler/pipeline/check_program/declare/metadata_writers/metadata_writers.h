#pragma once

namespace tz {

class CheckState;
class Expr;
struct FunctionDecl;
struct SourceFile;

class MetadataWriters {
public:
  explicit MetadataWriters(CheckState &owner);

  void rememberWriters(SourceFile &program);

private:
  void rememberWriter(FunctionDecl *fn, const Expr *expr);

  CheckState &state;
};

}
