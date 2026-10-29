#pragma once

#include "llvm/Support/SMLoc.h"
#include <cstddef>

namespace tz {

class Expr;
class KnowledgePass;
class Stmt;
struct ClassDecl;
struct FunctionDecl;
struct Param;
struct TypeRef;
struct VarDeclarator;

class KnowledgeWalk {
public:
  KnowledgeWalk(KnowledgePass &pass, size_t module, size_t from);

  void stmt(const Stmt *stmt);
  void expr(const Expr *expr);
  void typeRef(const TypeRef &ref);
  void function(const FunctionDecl *fn);
  void classDecl(const ClassDecl *cls);
  void declarator(const VarDeclarator &declarator);

private:
  bool use(const Expr *expr, bool call);
  void param(const Param &param);

  KnowledgePass &pass_;
  size_t module_;
  size_t from_;
};

}
