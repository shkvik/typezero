#include "compiler/check/flow/loop_scanner/loop_scanner.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/objects/keyed_field_checker/keyed_field_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// Built-in functions called by name; they change no variables and arrays.
const llvm::StringRef kGlobalFunctions[] = {
    "String",   "Number",  "parseInt",           "parseFloat", "isNaN",
    "isFinite", "Boolean", "encodeURIComponent", "encodeURI",  "decodeURIComponent",
    "decodeURI"};

} // namespace

LoopScanner::LoopScanner(Sema &sema) : sema_(sema) {}

Sema::LoopScan LoopScanner::scanLoop(llvm::ArrayRef<const Stmt *> stmts,
                              llvm::ArrayRef<const Expr *> exprs) const {
  Sema::LoopScan scan;
  for (const Stmt *stmt : stmts) {
    if (stmt) {
      scanStmt(stmt, scan, 0);
    }
  }
  for (const Expr *expr : exprs) {
    if (expr) {
      scanExpr(expr, scan, 0);
    }
  }

  return scan;
}

// Whether `expr` names a variable that holds an iterator: iterating it runs
// the code of a generator or the callbacks of helpers.
bool LoopScanner::namesIterator(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? sema_.scopes().lookup(name->name).var : nullptr;

  return var && var->type && var->type->nonOptional()->isIterator();
}

// `depth` counts the loops nested inside the scanned one.
void LoopScanner::scanStmt(const Stmt *stmt, Sema::LoopScan &scan, unsigned depth) const {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      scanStmt(s, scan, depth);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls) {
      if (d.init) {
        scanExpr(d.init, scan, depth);
      }
      if (d.destructured && d.init && namesIterator(d.init)) {
        scan.callsFunctions = true;
      }
    }
    break;
  case Stmt::Kind::Function:
    // What a function declared in the loop assigns to may change in the
    // loop, when it is called.
    scanStmt(cast<FunctionDecl>(stmt)->body, scan, depth + 1);
    break;
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  case Stmt::Kind::Expr:
    scanExpr(cast<ExprStmt>(stmt)->expr, scan, depth);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    scanExpr(s->cond, scan, depth);
    scanStmt(s->thenStmt, scan, depth);
    if (s->elseStmt) {
      scanStmt(s->elseStmt, scan, depth);
    }
    break;
  }
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    scanExpr(s->cond, scan, depth + 1);
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    scanStmt(s->body, scan, depth + 1);
    scanExpr(s->cond, scan, depth + 1);
    break;
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    if (s->init) {
      scanStmt(s->init, scan, depth + 1);
    }
    if (s->cond) {
      scanExpr(s->cond, scan, depth + 1);
    }
    if (s->step) {
      scanExpr(s->step, scan, depth + 1);
    }
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    scanExpr(s->iterable, scan, depth);
    if (namesIterator(s->iterable)) {
      scan.callsFunctions = true;
    }
    scanStmt(s->body, scan, depth + 1);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    scanExpr(s->discriminant, scan, depth);
    for (const SwitchCase &c : s->cases) {
      if (c.test) {
        scanExpr(c.test, scan, depth);
      }
    }
    ++scan.switches; // an unlabeled `break` in it leaves the switch
    for (const SwitchCase &c : s->cases) {
      for (const Stmt *body : c.body) {
        scanStmt(body, scan, depth);
      }
    }
    --scan.switches;
    break;
  }
  case Stmt::Kind::Labeled: {
    auto *s = cast<LabeledStmt>(stmt);
    scan.innerLabels.push_back(s->label);
    scanStmt(s->body, scan, depth);
    scan.innerLabels.pop_back();
    break;
  }
  case Stmt::Kind::Return:
    if (auto *value = cast<ReturnStmt>(stmt)->value) {
      scanExpr(value, scan, depth);
    }
    break;
  case Stmt::Kind::Throw:
    scanExpr(cast<ThrowStmt>(stmt)->value, scan, depth);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    scanStmt(s->block, scan, depth);
    if (s->handler) {
      scanStmt(s->handler, scan, depth);
    }
    if (s->finalizer) {
      scanStmt(s->finalizer, scan, depth);
    }
    break;
  }
  case Stmt::Kind::Break: {
    // `break label` leaves the loop unless the label is inside it.
    llvm::StringRef label = cast<BreakStmt>(stmt)->label;
    if (label.empty() ? depth == 0 && scan.switches == 0
                      : !llvm::is_contained(scan.innerLabels, label)) {
      scan.breaks = true;
    }
    break;
  }
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Class:
    break;
  }
}

void LoopScanner::scanExpr(const Expr *expr, Sema::LoopScan &scan, unsigned depth) const {
  auto change = [&](const Expr *target, int64_t delta) {
    auto *name = dyn_cast<NameExpr>(target);
    if (!name) {
      return;
    }
    Sema::LoopScan::Change &c = scan.changes[name->name];
    c.delta = ++c.count == 1 ? delta : 0;
    c.nested |= depth > 0;
  };
  // A constant step: an int literal from 1 to kMaxLength, or 0.
  auto step = [](const Expr *e) -> int64_t {
    auto *lit = dyn_cast<IntLitExpr>(e);

    return lit && lit->value >= 1 && lit->value <= uint64_t(kMaxLength) ? int64_t(lit->value) : 0;
  };
  auto isName = [](const Expr *e, const Expr *target) {
    auto *a = dyn_cast<NameExpr>(e), *b = dyn_cast<NameExpr>(target);

    return a && b && a->name == b->name;
  };
  auto scanTarget = [&](const Expr *target) {
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      scanExpr(index->object, scan, depth);
      scanExpr(index->index, scan, depth);
      scan.growsArrays = true; // `a[a.length] = x` appends
      scan.releasesElements = true;
      // `o[key] = x` assigns the field the key names.
      sema_.keyedFieldChecker().keyNames(index->index, [&](llvm::StringRef name) { scan.fieldWrites.insert(name); });
    } else if (auto *member = dyn_cast<MemberExpr>(target)) {
      scanExpr(member->object, scan, depth);
      scan.fieldWrites.insert(member->name);
      // `a.length = n`; the field of an object changes no array.
      if (member->name == "length") {
        scan.growsArrays = scan.shrinksArrays = true;
        scan.releasesElements = true;
      }
    }
  };

  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::Name:
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      scanExpr(e, scan, depth);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      scanExpr(e.expr, scan, depth);
      if (e.spread && namesIterator(e.expr)) {
        scan.callsFunctions = true;
      }
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      if (p.key) {
        scanExpr(p.key, scan, depth);
      }
      scanExpr(p.value, scan, depth);
    }
    break;
  case Expr::Kind::Member:
    scanExpr(cast<MemberExpr>(expr)->object, scan, depth);
    break;
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    scanExpr(index->object, scan, depth);
    scanExpr(index->index, scan, depth);
    break;
  }
  case Expr::Kind::Unary:
    scanExpr(cast<UnaryExpr>(expr)->operand, scan, depth);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    scanExpr(bin->lhs, scan, depth);
    scanExpr(bin->rhs, scan, depth);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    scanTarget(assign->target);
    scanExpr(assign->value, scan, depth);
    int64_t delta = 0;
    if (assign->op && (*assign->op == BinaryOp::Add || *assign->op == BinaryOp::Sub)) {
      delta = step(assign->value); // i += k
      if (*assign->op == BinaryOp::Sub) {
        delta = -delta;
      }
    } else if (auto *bin = dyn_cast<BinaryExpr>(assign->value); !assign->op && bin) {
      if (bin->op == BinaryOp::Add && isName(bin->lhs, assign->target)) {
        delta = step(bin->rhs); // i = i + k
      } else if (bin->op == BinaryOp::Add && isName(bin->rhs, assign->target)) {
        delta = step(bin->lhs); // i = k + i
      } else if (bin->op == BinaryOp::Sub && isName(bin->lhs, assign->target)) {
        delta = -step(bin->rhs); // i = i - k
      }
    }
    change(assign->target, delta);
    break;
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    scanTarget(update->target);
    change(update->target, update->increment ? 1 : -1);
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    for (const Expr *arg : call->args) {
      scanExpr(arg, scan, depth);
    }
    if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
      if (sema_.callChecker().isGlobalName(member->object, "console") || sema_.callChecker().isGlobalName(member->object, "Math")) {
        break;
      }
      scanExpr(member->object, scan, depth);
      llvm::StringRef method = member->name;
      // `o.run()` calls the function in a field: any code.
      if (sema_.file_->fieldNames.contains(method)) {
        scan.callsFunctions = true;
      }
      if (method == "push" || method == "unshift") {
        scan.growsArrays = true;
      } else if (method == "pop" || method == "shift") {
        scan.shrinksArrays = scan.releasesElements = true;
      } else if (method == "splice") {
        scan.growsArrays = scan.shrinksArrays = scan.releasesElements = true;
      } else if (method == "fill") {
        scan.releasesElements = true;
      }
      // The methods that call back, of arrays and of Results.
      static const char *const callbacks[] = {
          "map",     "filter", "forEach", "reduce",   "reduceRight",   "find",      "findIndex",
          "some",    "every",  "flatMap", "findLast", "findLastIndex", "sort",      "mapErr",
          "andThen", "orElse", "match",   "andTee",   "orTee",         "andThrough"};
      if (llvm::is_contained(callbacks, method) && !call->args.empty()) {
        scan.callsFunctions = true;
      }
      // An iterator runs a generator's code or the callbacks of helpers.
      if (method == "next" || method == "toArray" ||
          (method == "from" &&
           (sema_.callChecker().isGlobalName(member->object, "Array") || sema_.callChecker().isGlobalName(member->object, "Iterator")))) {
        scan.callsFunctions = true;
      }
    } else if (!llvm::any_of(kGlobalFunctions, [&](llvm::StringRef name) {
                 return sema_.callChecker().isGlobalName(call->callee, name);
               })) {
      scan.callsFunctions = true;
    }
    break;
  }
  case Expr::Kind::New:
    if (const Expr *callee = cast<NewExpr>(expr)->callee) {
      scanExpr(callee, scan, depth);
    }
    for (const Expr *arg : cast<NewExpr>(expr)->args) {
      scanExpr(arg, scan, depth);
    }
    // A constructor runs code.
    if (cast<NewExpr>(expr)->name != "Array") {
      scan.callsFunctions = true;
    }
    break;
  case Expr::Kind::Conditional: {
    auto *cond = cast<ConditionalExpr>(expr);
    scanExpr(cond->cond, scan, depth);
    scanExpr(cond->thenExpr, scan, depth);
    scanExpr(cond->elseExpr, scan, depth);
    break;
  }
  case Expr::Kind::Cast:
    scanExpr(cast<CastExpr>(expr)->operand, scan, depth);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs) {
      scanExpr(e, scan, depth);
    }
    break;
  case Expr::Kind::OptionalChain:
    scanExpr(cast<OptionalChainExpr>(expr)->chain, scan, depth);
    break;
  case Expr::Kind::Function:
    // The same for an arrow created in the loop.
    scanStmt(cast<FunctionExpr>(expr)->fn->body, scan, depth + 1);
    break;
  case Expr::Kind::Yield:
    // Whoever iterates the generator runs any code before it goes on.
    if (const Expr *value = cast<YieldExpr>(expr)->value) {
      scanExpr(value, scan, depth);
    }
    scan.callsFunctions = true;
    break;
  case Expr::Kind::Await:
    // So does any other task while this one waits.
    scanExpr(cast<AwaitExpr>(expr)->value, scan, depth);
    scan.callsFunctions = true;
    break;
  case Expr::Kind::Class:
    // Its static initializers run, and its base may be a call.
    scan.callsFunctions = true;
    break;
  }
}

} // namespace tz
