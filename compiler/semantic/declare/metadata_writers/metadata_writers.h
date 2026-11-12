#pragma once

#include <vector>

namespace tz {

class SemanticState;
class Expr;
class Stmt;
class CallExpr;
struct FunctionDecl;
struct SourceFile;

class MetadataWriters {
public:
  explicit MetadataWriters(SemanticState &owner);

  void rememberWriters(SourceFile &program);

private:
  void rememberWritersIn(FunctionDecl *functionDecl);
  bool isDefiningMetadata(const CallExpr *call) const;
  std::vector<FunctionDecl *> functionsOf(Stmt *item) const;
  void rememberWriter(FunctionDecl *functionDecl, const Expr *expr);

  SemanticState &state;
};

}
