#include "compiler/lower/iterators/iter_lower_helpers/iter_lower_helpers.h"
#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

bool hasTry(const Stmt *stmt) {
  bool found = false;
  std::function<void(const Stmt *)> visit = [&](const Stmt *s) {
    if (!s || found) {
      return;
    }
    switch (s->kind()) {
    case Stmt::Kind::Try:
      found = true;
      break;
    case Stmt::Kind::Block:
      for (const Stmt *inner : cast<BlockStmt>(s)->body) {
        visit(inner);
      }
      break;
    case Stmt::Kind::If:
      visit(cast<IfStmt>(s)->thenStmt);
      visit(cast<IfStmt>(s)->elseStmt);
      break;
    case Stmt::Kind::While:
      visit(cast<WhileStmt>(s)->body);
      break;
    case Stmt::Kind::DoWhile:
      visit(cast<DoWhileStmt>(s)->body);
      break;
    case Stmt::Kind::For:
      visit(cast<ForStmt>(s)->body);
      break;
    case Stmt::Kind::ForOf:
      visit(cast<ForOfStmt>(s)->body);
      break;
    case Stmt::Kind::Switch:
      for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
        for (const Stmt *inner : c.body) {
          visit(inner);
        }
      }
      break;
    case Stmt::Kind::Labeled:
      visit(cast<LabeledStmt>(s)->body);
      break;
    default:
      break;
    }
  };
  visit(stmt);

  return found;
}

}
