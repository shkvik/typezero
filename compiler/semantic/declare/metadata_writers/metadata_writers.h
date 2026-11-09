#pragma once

namespace tz {

class SemanticState;
class Expr;
struct FunctionDecl;
struct SourceFile;

class MetadataWriters {
public:
  explicit MetadataWriters(SemanticState &owner);

  void rememberWriters(SourceFile &program);

private:
  void rememberWritersIn(FunctionDecl *fn);
  void rememberWriter(FunctionDecl *fn, const Expr *expr);

  SemanticState &state;
};

}
