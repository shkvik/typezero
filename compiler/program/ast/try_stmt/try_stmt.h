#pragma once

#include "compiler/program/ast/block_stmt/block_stmt.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

// `try { block } catch (e) { handler } finally { finalizer }`. Either clause
// may be left out, not both; `catch { ... }` has no variable (`param.name` is
// empty). The variable is an Error, whatever was thrown.
struct TryStmt : Stmt {
  explicit TryStmt(llvm::SMLoc loc) : Stmt(Kind::Try, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Try; }

  BlockStmt *block = nullptr;
  BlockStmt *handler = nullptr;   // null without `catch`
  BlockStmt *finalizer = nullptr; // null without `finally`
  VarDeclarator param;            // `catch (e)`, or `catch (e: unknown)`; sym set by Checker
};

} // namespace tz
