// Exceptions (docs/errors.md): which functions may throw, `throw`, `try`
// with `catch` and `finally`, the class Error, and the stack check of the
// functions that may recurse.
//
// An error is a value that goes back up the calls, not something that
// unwinds the stack: `throw` puts the Error in the runtime's tz_thrown, and
// control leaves the function as by `return`, releasing its variables on the
// way, until a `try` around it catches the error. After a call to a function
// that may throw, the caller loads tz_thrown: if an error is there, it takes
// the same way out. Which functions may throw is known for the whole program
// (analyzeThrows): those that cannot, and the calls to them, cost nothing.
//
// The way out is shared: each scope gets, once something in it may throw, an
// unwind block that releases its variables and goes on to the scope around
// it, or to the `catch` or the `finally` of a `try` that starts there, or out
// of the function. Each place that may throw only adds its place to the
// stack of the error (a constant TzFrame) and jumps to the innermost one.
//
// Temporaries: an owned value an expression holds while a later part of it
// may throw waits in a managed slot of the scope (CodeGen::hold), so the way
// out releases it too; LLVM keeps it in a register on the normal path.

#include "codegen/codegen.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <functional>
#include <map>
#include <optional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

// What some code may run and throw: the calls it makes, the built-ins of it
// that may throw, and the functions it makes values of.
struct ThrowSummary {
  // A `throw` or a built-in that may throw, not caught in the code itself.
  bool intrinsic = false;
  // Resumes an iterator object, whose code (a generator, the callbacks of
  // helpers) may throw.
  bool resumes = false;
  // The calls not caught in the code, of functions and through values of
  // function types.
  std::vector<const FunctionDecl *> calls;
  std::vector<const Type *> valueCalls;
  // Every call, caught or not, for finding the functions that recurse.
  std::vector<std::pair<const FunctionDecl *, const CallExpr *>> allCalls;
  std::vector<const Type *> allValueCalls;
  // The functions the code makes values of, by the type of the value, and
  // the functions Result.fromThrowable makes, which throw what the function
  // that turns the error throws.
  std::vector<std::pair<const Type *, const FunctionDecl *>> values;
  std::vector<std::pair<const Type *, const Expr *>> fromThrowables;
  // The function values given a type with another result (see
  // CastExpr::adaptsResult): a value of the first type may be one of the
  // second.
  std::vector<std::pair<const Type *, const Type *>> adapters;
  // The functions declared or written in it.
  std::vector<const FunctionDecl *> nested;
};

namespace {

// What a call through the method table of `fn` may run: fn and the methods
// that override it in the classes down the chain (not an abstract one, which
// has no code).
void forEachImplementation(const FunctionDecl *fn,
                           llvm::function_ref<void(const FunctionDecl *)> visit) {
  if (!fn->isAbstract)
    visit(fn);
  if (!fn->cls || fn->slot < 0)
    return;
  std::function<void(const ClassDecl *)> down = [&](const ClassDecl *cls) {
    for (const ClassDecl *child : cls->children) {
      if (size_t(fn->slot) < child->vtable.size())
        if (const FunctionDecl *impl = child->vtable[size_t(fn->slot)]; impl && impl != fn)
          visit(impl);
      down(child);
    }
  };
  down(fn->cls);
}

// Constant integers, as the operands of `/` and `new Array(n)`.
std::optional<int64_t> constantInt(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      return std::nullopt;
    expr = conv->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr))
    return int64_t(lit->value);
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg)
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand))
      return -int64_t(lit->value);
  return std::nullopt;
}

// The function a name refers to directly, as a value (see knownFunction in
// codegen_closure.cpp).
const FunctionDecl *namedFunction(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name)
    return nullptr;
  if (name->func)
    return name->func->decl;
  if (const VarSymbol *var = name->var)
    return var->function ? var->function : var->isConst ? var->lambda : nullptr;
  return nullptr;
}

// Walks code and fills a ThrowSummary. `caught`: the code is in the block of
// a `try` that has a `catch`, which takes what it throws.
class ThrowScanner {
public:
  explicit ThrowScanner(ThrowSummary &out) : out_(out) {}

  void stmt(const Stmt *stmt, bool caught) {
    if (!stmt)
      return;
    switch (stmt->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *s : cast<BlockStmt>(stmt)->body)
        this->stmt(s, caught);
      break;
    case Stmt::Kind::VarDecl:
      for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
        expr(d.init, caught);
      break;
    case Stmt::Kind::Function:
      out_.nested.push_back(cast<FunctionDecl>(stmt));
      break;
    case Stmt::Kind::Expr:
      expr(cast<ExprStmt>(stmt)->expr, caught);
      break;
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      expr(s->cond, caught);
      this->stmt(s->thenStmt, caught);
      this->stmt(s->elseStmt, caught);
      break;
    }
    case Stmt::Kind::While:
      expr(cast<WhileStmt>(stmt)->cond, caught);
      this->stmt(cast<WhileStmt>(stmt)->body, caught);
      break;
    case Stmt::Kind::DoWhile:
      this->stmt(cast<DoWhileStmt>(stmt)->body, caught);
      expr(cast<DoWhileStmt>(stmt)->cond, caught);
      break;
    case Stmt::Kind::For: {
      auto *s = cast<ForStmt>(stmt);
      this->stmt(s->init, caught);
      expr(s->cond, caught);
      expr(s->step, caught);
      this->stmt(s->body, caught);
      break;
    }
    case Stmt::Kind::ForOf: {
      auto *s = cast<ForOfStmt>(stmt);
      expr(s->iterable, caught);
      if (s->iterable->type && s->iterable->type->isIterator())
        resumes(caught);
      this->stmt(s->body, caught);
      break;
    }
    case Stmt::Kind::Switch: {
      auto *s = cast<SwitchStmt>(stmt);
      expr(s->discriminant, caught);
      for (const SwitchCase &c : s->cases) {
        expr(c.test, caught);
        for (const Stmt *body : c.body)
          this->stmt(body, caught);
      }
      break;
    }
    case Stmt::Kind::Labeled:
      this->stmt(cast<LabeledStmt>(stmt)->body, caught);
      break;
    case Stmt::Kind::Return:
      expr(cast<ReturnStmt>(stmt)->value, caught);
      break;
    case Stmt::Kind::Throw:
      expr(cast<ThrowStmt>(stmt)->value, caught);
      intrinsic(caught);
      break;
    case Stmt::Kind::Try: {
      auto *s = cast<TryStmt>(stmt);
      this->stmt(s->block, caught || s->handler);
      this->stmt(s->handler, caught);
      this->stmt(s->finalizer, caught);
      break;
    }
    case Stmt::Kind::Class: {
      // Its static fields get their values where it is declared (those of
      // an instance of a generic class are the generic class's), and its
      // decorators run there.
      auto *cls = cast<ClassDecl>(stmt);
      expr(cls->baseExpr, caught);
      for (const ClassField &f : cls->fields)
        if (f.isStatic && f.var)
          expr(f.init, caught);
      for (const Stmt *s : cls->decorations)
        this->stmt(s, caught);
      break;
    }
    default:
      break;
    }
  }

  void expr(const Expr *expr, bool caught) {
    if (!expr)
      return;
    // An iterator object runs code when it is resumed; whatever consumes one
    // may resume it. That of a dictionary only walks its entries.
    auto *dictIterator = dyn_cast<CallExpr>(expr);
    if (expr->type && expr->type->isIterator() &&
        !(dictIterator && isDictIterator(dictIterator->builtin)))
      resumes(caught);
    switch (expr->kind()) {
    case Expr::Kind::Name: {
      auto *name = cast<NameExpr>(expr);
      if (const FunctionDecl *fn = namedFunction(name); fn && name->type && name->type->isFunction())
        value(name->type, fn);
      break;
    }
    case Expr::Kind::Template:
      for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
        this->expr(e, caught);
      break;
    case Expr::Kind::ArrayLit:
      for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
        this->expr(e.expr, caught);
      break;
    case Expr::Kind::ObjectLit:
      for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
        this->expr(p.key, caught);
        this->expr(p.value, caught);
      }
      break;
    case Expr::Kind::Member: {
      auto *member = cast<MemberExpr>(expr);
      this->expr(member->object, caught);
      if (jsonReadMayThrow(member->object))
        intrinsic(caught); // a key of null or undefined
      // A method as a value: through the method table, any that overrides it.
      if (member->method && member->type) {
        if (member->virtualCall)
          forEachImplementation(member->method->decl,
                                [&](const FunctionDecl *fn) { value(member->type, fn); });
        else
          value(member->type, member->method->decl);
      }
      break;
    }
    case Expr::Kind::Index:
      this->expr(cast<IndexExpr>(expr)->object, caught);
      this->expr(cast<IndexExpr>(expr)->index, caught);
      if (jsonReadMayThrow(cast<IndexExpr>(expr)->object))
        intrinsic(caught);
      break;
    case Expr::Kind::Unary: {
      auto *unary = cast<UnaryExpr>(expr);
      this->expr(unary->operand, caught);
      if (unary->op == UnaryOp::NonNull && unary->operand->type &&
          unary->operand->type->isOptional())
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Binary: {
      auto *bin = cast<BinaryExpr>(expr);
      this->expr(bin->lhs, caught);
      if (bin->op != BinaryOp::InstanceOf)
        this->expr(bin->rhs, caught);
      if (operatorThrows(bin->op, bin->lhs->type, bin->rhs))
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Assign: {
      auto *assign = cast<AssignExpr>(expr);
      target(assign->target, caught);
      this->expr(assign->value, caught);
      if (assign->op && operatorThrows(*assign->op, assign->type, assign->value))
        intrinsic(caught);
      // `a.length = n`: a negative length, or one that grows an array whose
      // elements have no default.
      if (auto *member = dyn_cast<MemberExpr>(assign->target);
          member && member->object->type && member->object->type->isArray())
        intrinsic(caught);
      // A key of a Json that is no object, or an element past the end.
      if (auto *member = dyn_cast<MemberExpr>(assign->target);
          member && member->object->type && member->object->type->nonOptional()->isJson())
        intrinsic(caught);
      if (auto *index = dyn_cast<IndexExpr>(assign->target);
          index && index->object->type && index->object->type->nonOptional()->isJson())
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Update: {
      auto *update = cast<UpdateExpr>(expr);
      target(update->target, caught);
      if (update->type && update->type->isDecimal())
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Call:
      call(cast<CallExpr>(expr), caught);
      break;
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(expr);
      this->expr(create->callee, caught);
      for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
        this->expr(arg, caught);
      // Through the description of the class: its arguments are checked
      // then, and its code may throw.
      if (create->reflect) {
        intrinsic(caught);
        break;
      }
      if (const ClassDecl *cls = create->cls) {
        // The initializers of the classes up the chain run too.
        for (const ClassDecl *c = cls; c; c = c->parent)
          if (c->init)
            callee(c->init, caught, nullptr);
        if (create->ctor && create->ctor->func)
          callee(create->ctor->func->decl, caught, nullptr);
      } else if (create->args.size() == 1) {
        std::optional<int64_t> length = constantInt(create->args[0]);
        if (!length || *length < 0)
          intrinsic(caught);
      }
      break;
    }
    case Expr::Kind::Conditional: {
      auto *cond = cast<ConditionalExpr>(expr);
      this->expr(cond->cond, caught);
      this->expr(cond->thenExpr, caught);
      this->expr(cond->elseExpr, caught);
      break;
    }
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(expr);
      this->expr(conv->operand, caught);
      // An adapter calls the function it wraps.
      if (conv->adaptsResult)
        out_.adapters.push_back({conv->type, conv->operand->type});
      else if (conv->type && conv->type->isFunction() && conv->operand->type &&
               conv->operand->type->isFunction())
        if (const FunctionDecl *fn = namedFunction(conv->operand))
          value(conv->type, fn);
      break;
    }
    case Expr::Kind::Sequence:
      for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
        this->expr(e, caught);
      break;
    case Expr::Kind::OptionalChain:
      this->expr(cast<OptionalChainExpr>(expr)->chain, caught);
      break;
    case Expr::Kind::Function: {
      auto *fe = cast<FunctionExpr>(expr);
      out_.nested.push_back(fe->fn);
      if (fe->type)
        value(fe->type, fe->fn);
      break;
    }
    case Expr::Kind::Yield: {
      auto *yield = cast<YieldExpr>(expr);
      this->expr(yield->value, caught);
      // next() without a value may resume a `yield` that needs one.
      if (!yield->discarded && yield->type && !yield->type->isOptional() &&
          !yield->type->isUndefined() && !yield->type->isVoid())
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Await: {
      // A Promise that is rejected throws its error here.
      const Expr *value = cast<AwaitExpr>(expr)->value;
      this->expr(value, caught);
      if (value->type && value->type->isPromise())
        intrinsic(caught);
      break;
    }
    case Expr::Kind::Class: // as the declaration of its class, where it is
      stmt(cast<ClassExpr>(expr)->decl, caught);
      break;
    default:
      break;
    }
  }

private:
  void intrinsic(bool caught) {
    if (!caught)
      out_.intrinsic = true;
  }
  void resumes(bool caught) {
    if (!caught)
      out_.resumes = true;
  }
  void callee(const FunctionDecl *fn, bool caught, const CallExpr *site) {
    if (!fn)
      return;
    if (!caught)
      out_.calls.push_back(fn);
    out_.allCalls.push_back({fn, site});
  }
  void valueCall(const Type *type, bool caught) {
    if (!type)
      return;
    type = type->nonOptional();
    if (!type->isFunction())
      return;
    if (!caught)
      out_.valueCalls.push_back(type);
    out_.allValueCalls.push_back(type);
  }
  void value(const Type *type, const FunctionDecl *fn) {
    if (type && fn)
      out_.values.push_back({type->nonOptional(), fn});
  }
  // A function a built-in calls during the call.
  void callback(const Expr *arg, bool caught) {
    expr(arg, caught);
    if (auto *fe = dyn_cast<FunctionExpr>(arg))
      callee(fe->fn, caught, nullptr);
    else if (arg->type)
      valueCall(arg->type, caught);
  }
  void target(const Expr *target, bool caught) {
    if (auto *index = dyn_cast<IndexExpr>(target)) {
      expr(index->object, caught);
      expr(index->index, caught);
    } else if (auto *member = dyn_cast<MemberExpr>(target)) {
      expr(member->object, caught);
    }
  }

  // An integer `/` or `%` by what may be zero, an integer `**` (0 ** -1),
  // and the arithmetic of decimals, which may divide by zero or grow too
  // large.
  static bool operatorThrows(BinaryOp op, const Type *type, const Expr *rhs) {
    if (!type)
      return false;
    if (type->isDecimal())
      return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
             op == BinaryOp::Div || op == BinaryOp::Rem || op == BinaryOp::Pow;
    if (!type->isInteger())
      return false;
    if (op == BinaryOp::Pow)
      return true;
    if (op != BinaryOp::Div && op != BinaryOp::Rem)
      return false;
    std::optional<int64_t> divisor = constantInt(rhs);
    return !divisor || *divisor == 0;
  }

  void call(const CallExpr *c, bool caught) {
    auto *member = dyn_cast<MemberExpr>(c->callee);
    Builtin builtin = c->builtin;
    if (builtin == Builtin::None) {
      if (member)
        expr(member->object, caught);
      for (const Expr *arg : c->args)
        expr(arg, caught);
      // A method of a union: that of any member may run.
      for (const CallExpr::UnionMethod &m : c->unionMethods) {
        if (m.method && m.virtualCall)
          forEachImplementation(m.method->decl,
                                [&](const FunctionDecl *fn) { callee(fn, caught, c); });
        else if (m.method)
          callee(m.method->decl, caught, c);
        else
          valueCall(cast<MemberExpr>(c->callee)->type, caught);
      }
      if (!c->unionMethods.empty())
        return;
      if (c->method && c->virtualCall)
        forEachImplementation(c->method->decl,
                              [&](const FunctionDecl *fn) { callee(fn, caught, c); });
      else if (c->method)
        callee(c->method->decl, caught, c);
      else if (c->func)
        callee(c->func->decl, caught, c);
      else {
        expr(c->callee, caught);
        valueCall(c->callee->type, caught);
      }
      return;
    }
    if (member)
      expr(member->object, caught);
    // asyncMap and asyncAndThen of a Result call their function now.
    bool ofResult =
        (builtin == Builtin::ResultAsyncMap || builtin == Builtin::ResultAsyncAndThen) && member &&
        member->object->type && member->object->type->isResult();
    bool callbacks = (builtin >= Builtin::ArrForEach && builtin <= Builtin::ArrSortWith) ||
                     isResultCallback(builtin) || isIteratorSink(builtin) ||
                     (isIteratorStage(builtin) && c->fused) ||
                     builtin == Builtin::StrReplaceRegexFn || builtin == Builtin::MapForEach ||
                     builtin == Builtin::DictGroupBy || ofResult;
    for (size_t i = 0; i < c->args.size(); ++i) {
      const Expr *arg = c->args[i];
      bool function = arg->type && arg->type->nonOptional()->isFunction();
      if ((callbacks && function) || (builtin == Builtin::ArrayFrom && i == 1))
        callback(arg, caught);
      else
        expr(arg, caught);
    }
    switch (builtin) {
    case Builtin::ArrReduce:
    case Builtin::ArrReduceRight:
    case Builtin::IterReduce:
      if (c->args.size() < 2)
        intrinsic(caught); // of nothing, without an initial value
      break;
    case Builtin::IterTake:
    case Builtin::IterDrop: {
      std::optional<int64_t> count = c->args.empty() ? std::nullopt : constantInt(c->args[0]);
      if (!count || *count < 0)
        intrinsic(caught);
      break;
    }
    case Builtin::IterNext:
      resumes(caught);
      break;
    case Builtin::SafeTry:
      // Its generator runs once, now.
      if (!c->args.empty())
        if (auto *fe = dyn_cast<FunctionExpr>(c->args[0]))
          callee(fe->fn, caught, nullptr);
      break;
    case Builtin::ResultFromThrowable:
      out_.fromThrowables.push_back({c->type, c->args.size() > 1 ? c->args[1] : nullptr});
      break;
    case Builtin::ParseInt:
    case Builtin::ParseFloat:
      if (c->type && c->type->isInteger())
        intrinsic(caught); // NaN or a number that does not fit
      break;
    case Builtin::StrRepeat:
    case Builtin::StrPadStart:
    case Builtin::StrPadEnd:
    case Builtin::FromCodePoint:
    case Builtin::UriEncodeComponent: // a lone surrogate
    case Builtin::UriEncode:
    case Builtin::UriDecodeComponent: // an escape that is not UTF-8
    case Builtin::UriDecode:
    case Builtin::ResultUnsafeUnwrap:
    case Builtin::ResultUnsafeUnwrapErr:
      intrinsic(caught);
      break;
    case Builtin::NumToFixed:
      if (!c->args.empty())
        intrinsic(caught);
      break;
    case Builtin::NumToRadix:
      intrinsic(caught);
      break;
    case Builtin::RegExpNew: // a SyntaxError
    case Builtin::JsonParse:
    case Builtin::JsonParseAs:
    case Builtin::FromJson:
    // Reflection checks its values when the program runs, and runs the code
    // of the class.
    case Builtin::ReflectGet:
    case Builtin::ReflectApply:
    case Builtin::ReflectConstruct:
    case Builtin::FromJsonClass:
      intrinsic(caught);
      break;
    case Builtin::StrMatchAll:
      // Without the g flag, a TypeError, where the regex is known only when
      // the program runs.
      if (!c->regex)
        intrinsic(caught);
      break;
    case Builtin::StrReplaceRegex:
    case Builtin::StrReplaceRegexFn:
      if (!c->regex && member && member->name == "replaceAll")
        intrinsic(caught);
      break;
    case Builtin::JsonStringify:
      if (!c->args.empty() && c->args[0]->type && stringifyMayThrow(c->args[0]->type))
        intrinsic(caught); // a value inside itself
      break;
    case Builtin::DateToISOString: // of an invalid date
      intrinsic(caught);
      break;
    case Builtin::DtfFormat:
    case Builtin::DtfFormatToParts:
      if (!c->args.empty()) // a date that may be invalid
        intrinsic(caught);
      break;
    case Builtin::DateToLocale:
      if (dateOptionsMayThrow(llvm::ArrayRef<Expr *>(c->args).drop_front()))
        intrinsic(caught);
      break;
    case Builtin::DtfNew:
      if (dateOptionsMayThrow(c->args))
        intrinsic(caught);
      break;
    case Builtin::MetaDefine:
      // A key `design:*` that the program wrote, which only the compiler
      // writes (runtime/meta.c).
      if (!c->metaKey && !c->designRecord && !c->args.empty() && !isa<StringLitExpr>(c->args[0]))
        intrinsic(caught);
      break;
    case Builtin::ObjectKeys:
    case Builtin::ObjectValues:
    case Builtin::ObjectEntries:
      // Of a Json that is null or undefined, but for `for...in`.
      if (!c->forIn && !c->args.empty() && c->args[0]->type &&
          c->args[0]->type->nonOptional()->isJson())
        intrinsic(caught);
      break;
    default:
      if (isRoundingBuiltin(builtin) && c->type && c->type->isInteger() && !c->args.empty() &&
          c->args[0]->type != c->type)
        intrinsic(caught); // a number that does not fit in the integer
      if (isDecimalBuiltin(builtin))
        intrinsic(caught);
      if (isFsBuiltin(builtin) || isChildBuiltin(builtin)) // an error of the system
        intrinsic(caught);
      if (isCryptoBuiltin(builtin)) // the errors of node:crypto
        intrinsic(caught);
      if (isBytesBuiltin(builtin) && bytesMayThrow(c))
        intrinsic(caught);
      if (isIntlBuiltin(builtin) && intlCallMayThrow(c))
        intrinsic(caught);
      break;
    }
  }

  ThrowSummary &out_;
};

std::u16string utf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

} // namespace

//===----------------------------------------------------------------------===//
// Which functions may throw
//===----------------------------------------------------------------------===//

void CodeGen::analyzeThrows(SourceFile &file) {
  for (Stmt *item : file.items)
    if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->builtin && !cls->parent &&
                                              cls->name == "Error")
      errorClass_ = cls;

  // The functions of the program and what each may run, from top-level code
  // and the functions it declares, through the functions found in them.
  std::vector<const FunctionDecl *> functions;
  std::map<const FunctionDecl *, ThrowSummary> summaries;
  std::vector<const FunctionDecl *> work;
  auto add = [&](const FunctionDecl *fn) {
    if (!fn || (fn->cls && fn->cls->builtin) || summaries.count(fn))
      return;
    summaries[fn];
    functions.push_back(fn);
    work.push_back(fn);
  };
  ThrowSummary topLevel;
  ThrowScanner topScanner(topLevel);
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn))
        add(fn);
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      // Of a generic class, only its static methods are emitted; of an
      // instance, the others; a generic method only as its copies.
      for (const FunctionDecl *fn : cls->methods)
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted()))
          add(fn);
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances)
          add(fn);
        for (const FunctionDecl *fn : cls->constructors)
          add(fn);
        add(cls->init);
        for (const ClassField &f : cls->fields)
          add(f.method);
      }
    }
    topScanner.stmt(item, false);
  }
  for (const FunctionDecl *fn : file.instances)
    add(fn);
  for (const FunctionDecl *fn : topLevel.nested)
    add(fn);
  while (!work.empty()) {
    const FunctionDecl *fn = work.back();
    work.pop_back();
    ThrowSummary summary;
    ThrowScanner scanner(summary);
    for (const Param &param : fn->params)
      scanner.expr(param.defaultValue, false);
    scanner.stmt(fn->body, false);
    // An intrinsic may throw what the runtime throws.
    if (fn->external)
      summary.intrinsic = true;
    // A constructor runs the initializers of its class and of those up the
    // chain (its `super(...)` does).
    if (fn->isConstructor)
      for (const ClassDecl *c = fn->cls; c; c = c->parent)
        if (c->init && c->init != fn) {
          summary.calls.push_back(c->init);
          summary.allCalls.push_back({c->init, nullptr});
        }
    for (const FunctionDecl *nested : summary.nested)
      add(nested);
    summaries[fn] = std::move(summary);
    // The tail calls of a closure that are jumps, which do not recurse.
    if (fn->isClosure() && !fn->isGenerator && fn->sym && fn->sym->ret)
      findTailGroups(const_cast<FunctionDecl *>(fn));
  }

  // A method a decorator may replace calls what its global holds, which
  // may be its adapter, which throws for a `this` of another class.
  for (const FunctionDecl *fn : functions)
    if (fn->impl) {
      summaries[fn].valueCalls.push_back(fn->impl->type);
      summaries[fn].allValueCalls.push_back(fn->impl->type);
      throwingTypes_.insert(fn->impl->type);
    }

  // The functions each type of function value may be.
  llvm::DenseMap<const Type *, std::vector<const FunctionDecl *>> valuesOf;
  std::vector<std::pair<const Type *, const Expr *>> fromThrowables;
  std::vector<std::pair<const Type *, const Type *>> adapters;
  auto collect = [&](const ThrowSummary &summary) {
    for (const auto &[type, fn] : summary.values)
      valuesOf[type].push_back(fn);
    fromThrowables.insert(fromThrowables.end(), summary.fromThrowables.begin(),
                          summary.fromThrowables.end());
    adapters.insert(adapters.end(), summary.adapters.begin(), summary.adapters.end());
  };
  collect(topLevel);
  for (const auto &[fn, summary] : summaries)
    collect(summary);
  // A value that adapts the result of another runs what that one may be,
  // through adapters of adapters too.
  for (bool changed = !adapters.empty(); changed;) {
    changed = false;
    for (const auto &[to, from] : adapters) {
      std::vector<const FunctionDecl *> wrapped = valuesOf.lookup(from);
      std::vector<const FunctionDecl *> &mine = valuesOf[to];
      for (const FunctionDecl *fn : wrapped)
        if (!llvm::is_contained(mine, fn)) {
          mine.push_back(fn);
          changed = true;
        }
    }
  }
  // A call through an interface runs the method of a class that implements
  // it (see emitInterfaceCall): such methods are values of the member's type.
  for (const auto &[iface, info] : interfaces_)
    for (const Type *implementer : info.implementers) {
      const ClassDecl *cls = implementer->classDecl();
      if (!cls || cls->literal)
        continue;
      auto implemented = [&](llvm::StringRef name, const Type *fnType) {
        if (const FunctionDecl *fn = implementingMethod(cls, name, fnType))
          forEachImplementation(fn, [&](const FunctionDecl *m) { valuesOf[fnType].push_back(m); });
      };
      for (const Field &f : iface->fields())
        if (f.type->isFunction())
          implemented(f.name, f.type);
      for (const Overload &overload : iface->overloads())
        for (const Type *signature : overload.signatures)
          implemented(overload.name, signature);
    }

  // The functions that may recurse: those in a cycle of calls, through
  // function values too, but not through tail calls, which are jumps.
  llvm::DenseMap<const FunctionDecl *, size_t> indexOf;
  for (size_t i = 0; i < functions.size(); ++i)
    indexOf[functions[i]] = i;
  std::vector<std::vector<size_t>> edges(functions.size());
  for (size_t i = 0; i < functions.size(); ++i) {
    const ThrowSummary &summary = summaries[functions[i]];
    auto edge = [&](const FunctionDecl *to) {
      if (auto found = indexOf.find(to); found != indexOf.end())
        edges[i].push_back(found->second);
    };
    for (const auto &[to, site] : summary.allCalls)
      if (!site || !tailJumps_.contains(site))
        edge(to);
    for (const Type *type : summary.allValueCalls)
      if (auto found = valuesOf.find(type); found != valuesOf.end())
        for (const FunctionDecl *to : found->second)
          edge(to);
  }
  std::vector<int> index(functions.size(), -1), low(functions.size(), 0);
  std::vector<bool> onStack(functions.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : edges[v]) {
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v])
      return;
    std::vector<size_t> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(w);
    } while (w != v);
    if (cycle.size() == 1 && !llvm::is_contained(edges[v], v))
      return;
    for (size_t member : cycle)
      // A generator runs its body when resumed, where the stack is checked
      // by nothing (see README); calling it only makes it.
      if (!functions[member]->isGenerator)
        recursive_.insert(functions[member]);
  };
  for (size_t v = 0; v < functions.size(); ++v)
    if (index[v] < 0)
      visit(v);

  // The functions that may throw: those with a throw of their own (the stack
  // check is one), and those that call one, until nothing changes.
  // A call of an async function never throws: what its body throws
  // rejects the promise it gives.
  for (const FunctionDecl *fn : functions) {
    const ThrowSummary &summary = summaries[fn];
    if ((summary.intrinsic || summary.resumes || recursive_.contains(fn)) && !fn->isAsync)
      throwing_.insert(fn);
  }
  for (bool changed = true; changed;) {
    changed = false;
    for (const auto &[type, fns] : valuesOf)
      if (!throwingTypes_.contains(type) && llvm::any_of(fns, [&](const FunctionDecl *fn) {
            return throwing_.contains(fn);
          })) {
        throwingTypes_.insert(type);
        changed = true;
      }
    for (const auto &[type, toError] : fromThrowables) {
      if (!toError || throwingTypes_.contains(type))
        continue;
      auto *fe = dyn_cast<FunctionExpr>(toError);
      bool throws = fe ? throwing_.contains(fe->fn) : typeThrows(toError->type);
      if (throws) {
        throwingTypes_.insert(type);
        changed = true;
      }
    }
    for (const FunctionDecl *fn : functions) {
      if (throwing_.contains(fn) || fn->isAsync)
        continue;
      const ThrowSummary &summary = summaries[fn];
      if (llvm::any_of(summary.calls, [&](const FunctionDecl *to) { return throwing_.contains(to); }) ||
          llvm::any_of(summary.valueCalls, [&](const Type *type) { return typeThrows(type); })) {
        throwing_.insert(fn);
        changed = true;
      }
    }
  }
}

bool CodeGen::callMayThrow(const FunctionDecl *fn, bool virtualCall) const {
  if (!virtualCall)
    return mayThrow(fn);
  bool throws = false;
  forEachImplementation(fn, [&](const FunctionDecl *impl) { throws |= mayThrow(impl); });
  return throws;
}

bool CodeGen::summaryThrows(const ThrowSummary &summary) const {
  return summary.intrinsic || summary.resumes ||
         llvm::any_of(summary.calls, [&](const FunctionDecl *fn) { return mayThrow(fn); }) ||
         llvm::any_of(summary.valueCalls, [&](const Type *type) { return typeThrows(type); });
}

bool CodeGen::exprThrows(const Expr *expr) {
  if (!expr)
    return false;
  if (auto found = exprThrows_.find(expr); found != exprThrows_.end())
    return found->second;
  ThrowSummary summary;
  ThrowScanner(summary).expr(expr, false);
  bool throws = summaryThrows(summary);
  exprThrows_[expr] = throws;
  return throws;
}

//===----------------------------------------------------------------------===//
// Places
//===----------------------------------------------------------------------===//

llvm::GlobalVariable *CodeGen::thrownGlobal() {
  if (!thrown_) {
    thrown_ = cast<llvm::GlobalVariable>(module_->getOrInsertGlobal("tz_thrown", builder_.getPtrTy()));
    thrown_->setAlignment(llvm::Align(8));
  }
  return thrown_;
}

// As V8 names them: `f`, `Point.length`, `new Point`; nothing for top-level
// code and a function without a name.
std::string CodeGen::frameFunctionName() const {
  const FunctionDecl *fn = currentFn_;
  if (!fn)
    return "";
  if (fn->cls && !fn->cls->literal) {
    if (fn->isConstructor || fn == fn->cls->init)
      return "new " + fn->cls->name.str();
    return (fn->cls->name + "." + fn->name).str();
  }
  return fn->name.str();
}

// { ptr function, i32 length, i32 line, ptr file, i32 length, i32 column }:
// TzFrame in runtime/tzrt.h.
llvm::Constant *CodeGen::frameDescriptor(llvm::SMLoc loc, const std::string *name) {
  std::string function = name ? *name : frameFunctionName();
  auto key = std::make_pair(function, loc.getPointer());
  if (auto found = frames_.find(key); found != frames_.end())
    return found->second;
  std::string file;
  unsigned line = 0, column = 0;
  if (sourceMgr_ && loc.isValid()) {
    if (unsigned buffer = sourceMgr_->FindBufferContainingLoc(loc)) {
      std::tie(line, column) = sourceMgr_->getLineAndColumn(loc, buffer);
      llvm::StringRef id = sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
      // The full path, as Node shows it.
      auto [it, inserted] = filePaths_.try_emplace(id);
      // A built-in module written in tz is named as node names its own,
      // `tz:http:12:5`.
      if (inserted && id.starts_with("tz:")) {
        it->second = id.str();
      } else if (inserted) {
        llvm::SmallString<256> path(id);
        llvm::sys::fs::make_absolute(path);
        llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
        llvm::sys::path::native(path);
        it->second = std::string(path);
      }
      file = it->second;
    }
  }
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  auto *type = llvm::StructType::get(ctx_, {ptr, i32, i32, ptr, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {utf8Constant(function), builder_.getInt32(uint32_t(function.size())),
             builder_.getInt32(line), utf8Constant(file), builder_.getInt32(uint32_t(file.size())),
             builder_.getInt32(column)});
  auto *global = new llvm::GlobalVariable(*module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.frame");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  frames_[key] = global;
  return global;
}

//===----------------------------------------------------------------------===//
// The way out
//===----------------------------------------------------------------------===//

llvm::BasicBlock *CodeGen::unwindTarget() {
  assert(!scopes_.empty() && "code that may throw runs in a scope");
  return unwindBlock(scopes_.size() - 1);
}

llvm::BasicBlock *CodeGen::unwindBlock(size_t i) {
  Scope &scope = scopes_[i];
  if (!scope.unwind)
    scope.unwind = newBlock("unwind");
  return scope.unwind;
}

// After scope `i`: the `catch` or `finally` of a `try` whose block (or
// `catch`) it is, or the scope around it, or the way out of the function.
llvm::BasicBlock *CodeGen::outerUnwind(size_t i) {
  for (TryContext &t : llvm::reverse(tries_)) {
    if (t.scopeDepth != i)
      continue;
    if (t.phase == TryContext::Phase::Block)
      return t.handler ? t.handler : t.throwEntry;
    if (t.phase == TryContext::Phase::Handler && t.throwEntry)
      return t.throwEntry;
  }
  if (i > 0)
    return unwindBlock(i - 1);
  if (!unwindExit_)
    unwindExit_ = newBlock("unwind.exit");
  return unwindExit_;
}

void CodeGen::finishScopeUnwind(size_t i) {
  Scope &scope = scopes_[i];
  llvm::BasicBlock *unwind = std::exchange(scope.unwind, nullptr);
  if (!unwind)
    return;
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  function_->insert(function_->end(), unwind);
  builder_.SetInsertPoint(unwind);
  emitScopeCleanup(scope);
  builder_.CreateBr(outerUnwind(i));
  builder_.restoreIP(point);
}

// Out of the function: a generator is done, top-level code reports the
// error, and anything else returns zero, which its caller drops.
void CodeGen::finishFunctionUnwind() {
  llvm::BasicBlock *exit = std::exchange(unwindExit_, nullptr);
  if (!exit)
    return;
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  function_->insert(function_->end(), exit);
  builder_.SetInsertPoint(exit);
  llvm::Type *ret = function_->getReturnType();
  if (async_) {
    emitAsyncUnwind();
  } else if (coro_) {
    builder_.CreateBr(coro_->finalBlock);
  } else if (function_->hasExternalLinkage()) { // main
    // Node reports the error of an ES module a few turns of the microtask
    // queue later (runtime/async.c).
    llvm::CallInst *call = builder_.CreateCall(
        processFunction(usesAsync_ && esModule_ ? "tz_async_main_failed" : "tz_uncaught",
                        builder_.getVoidTy(), {}));
    call->setDoesNotReturn();
    builder_.CreateUnreachable();
  } else if (ret->isVoidTy()) {
    builder_.CreateRetVoid();
  } else {
    builder_.CreateRet(llvm::Constant::getNullValue(ret));
  }
  builder_.restoreIP(point);
}

void CodeGen::emitThrowCheck() {
  emitThrowCheckWith([] {});
}

void CodeGen::emitThrowCheckWith(llvm::function_ref<void()> release) {
  llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), thrownGlobal(), "thrown");
  llvm::BasicBlock *throwBlock = newBlock("throw");
  llvm::BasicBlock *goBlock = newBlock("go");
  builder_.CreateCondBr(builder_.CreateIsNotNull(error), throwBlock, goBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(throwBlock);
  release();
  emitRuntimeThrow();
  switchTo(goBlock);
}

void CodeGen::emitRuntimeThrow() {
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(processFunction("tz_error_frame", builder_.getVoidTy(), {ptr}),
                      {frameDescriptor(loc_)});
  builder_.CreateBr(unwindTarget());
}

void CodeGen::emitThrow(llvm::Value *error, llvm::SMLoc loc) {
  llvm::Type *ptr = builder_.getPtrTy();
  builder_.CreateCall(processFunction("tz_error_throw", builder_.getVoidTy(), {ptr, ptr}),
                      {error, frameDescriptor(loc)});
  builder_.CreateBr(unwindTarget());
  startDeadBlock();
}

void CodeGen::emitThrowNew(int kind, llvm::StringRef message) {
  llvm::Value *text = stringLiteral(utf16(message));
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty(), *i32 = builder_.getInt32Ty();
  auto *fn = cast<llvm::Function>(
      processFunction("tz_throw_str", builder_.getVoidTy(), {i32, ptr, i64}).getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  builder_.CreateCall(fn, {builder_.getInt32(uint32_t(kind)), stringObj(text), stringMeta(text)});
  emitRuntimeThrow();
}

void CodeGen::emitThrowStmt(ThrowStmt *stmt) {
  llvm::Value *error = emitExpr(stmt->value);
  if (stmt->value->type->isUnknown()) // an Error, or else a TypeError
    error = builder_.CreateCall(unknownFunction("tz_unknown_error", builder_.getPtrTy(),
                                                {builder_.getPtrTy()}),
                                {error}, "error");
  // `throw new Error(...)` throws from `new`, as V8 says (a NewExpr is at
  // the name of its class).
  llvm::SMLoc loc = stmt->value->loc();
  if (isa<NewExpr>(stmt->value)) {
    const char *start = stmt->loc().getPointer(), *at = loc.getPointer();
    while (at > start && (at[-1] == ' ' || at[-1] == '\t'))
      --at;
    if (at - 3 >= start && llvm::StringRef(at - 3, 3) == "new")
      loc = llvm::SMLoc::getFromPointer(at - 3);
  }
  emitThrow(error, loc);
}

void CodeGen::emitStackCheck(llvm::SMLoc loc) {
  if (!stackLimit_) {
    stackLimit_ =
        cast<llvm::GlobalVariable>(module_->getOrInsertGlobal("tz_stack_limit", builder_.getPtrTy()));
    stackLimit_->setAlignment(llvm::Align(8));
  }
  llvm::Type *ptr = builder_.getPtrTy();
  llvm::Value *top = builder_.CreateIntrinsic(llvm::Intrinsic::stackaddress, {ptr}, {});
  llvm::Value *limit = builder_.CreateLoad(ptr, stackLimit_, "stack.limit");
  llvm::BasicBlock *overflowBlock = newBlock("stack.overflow");
  llvm::BasicBlock *goBlock = newBlock("stack.ok");
  builder_.CreateCondBr(builder_.CreateICmpULT(top, limit), overflowBlock, goBlock,
                        llvm::MDBuilder(ctx_).createUnlikelyBranchWeights());
  switchTo(overflowBlock);
  auto *fn = cast<llvm::Function>(
      processFunction("tz_throw_stack_overflow", builder_.getVoidTy(), {}).getCallee());
  fn->addFnAttr(llvm::Attribute::Cold);
  builder_.CreateCall(fn);
  llvm::SMLoc outer = std::exchange(loc_, loc);
  emitRuntimeThrow();
  loc_ = outer;
  switchTo(goBlock);
}

//===----------------------------------------------------------------------===//
// try, catch and finally
//===----------------------------------------------------------------------===//

CodeGen::TryContext *CodeGen::finallyBetween(size_t target) {
  for (TryContext &t : llvm::reverse(tries_)) {
    if (target != ~size_t(0) && t.targetsDepth <= target)
      return nullptr; // the target is inside this `try`, and so inside the rest
    if (t.finally && t.phase != TryContext::Phase::Finally)
      return &t;
  }
  return nullptr;
}

void CodeGen::emitJumpTo(size_t target, bool isBreak) {
  if (TryContext *t = finallyBetween(target)) {
    size_t k = 0;
    while (k < t->exits.size() && (t->exits[k].target != target || t->exits[k].isBreak != isBreak))
      ++k;
    if (k == t->exits.size())
      t->exits.push_back({target, isBreak});
    builder_.CreateStore(builder_.getInt32(uint32_t(3 + k)), t->completion);
    emitCleanups(t->scopeDepth);
    builder_.CreateBr(t->finally);
    startDeadBlock();
    return;
  }
  const JumpTarget &jt = targets_[target];
  bool inGenerator = !isBreak && jt.continueDepth != ~size_t(0);
  emitCleanups(inGenerator ? jt.continueDepth : jt.scopeDepth);
  builder_.CreateBr(isBreak ? jt.breakTarget : jt.continueTarget);
  startDeadBlock();
}

// What a function returns: the value of its `return`, or for a generator what
// its iterator gives when it is done.
static const Type *returnedType(const FunctionDecl *fn, const Type *returnType) {
  if (fn && fn->isGenerator && fn->sym && fn->sym->ret && fn->sym->ret->isIterator())
    return fn->sym->ret->returned();
  // An async function: what its promise holds.
  if (fn && fn->isAsync && returnType && returnType->isPromise())
    return returnType->promised();
  return returnType;
}

void CodeGen::emitReturnOf(llvm::Value *value) {
  const Type *type = returnedType(currentFn_, returnType_);
  if (TryContext *t = finallyBetween(~size_t(0))) {
    if (value) {
      if (!t->pendingValue) {
        // Empty until a `return` fills it (see emitTry).
        llvm::Type *lowered = lower(type);
        llvm::AllocaInst *slot = createStackSlot(lowered, "pending.value");
        llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
        init.CreateStore(llvm::Constant::getNullValue(lowered), slot);
        t->pendingValue = slot;
      }
      builder_.CreateStore(value, t->pendingValue);
    }
    t->returns = true;
    builder_.CreateStore(builder_.getInt32(2), t->completion);
    emitCleanups(t->scopeDepth);
    builder_.CreateBr(t->finally);
    startDeadBlock();
    return;
  }
  if (coro_ && currentFn_ && currentFn_->isGenerator) {
    emitCoroutineReturn(value, value ? type : nullptr);
    return;
  }
  if (async_) {
    emitAsyncReturn(value);
    return;
  }
  emitCleanups(0);
  if (value)
    builder_.CreateRet(value);
  else if (returnType_ && returnType_->isOptional())
    builder_.CreateRet(undefinedValue(returnType_)); // `return;`
  else
    builder_.CreateRetVoid();
  startDeadBlock();
}

// The block runs; an error from it goes to the `catch`, which takes it from
// tz_thrown, or with only a `finally`, to that. The `finally` runs after the
// block and the `catch`, however they end: `completion` says how to go on.
// While it runs, what is pending (the error to throw again, the value to
// return) waits in slots of its scope, which a way out of the `finally` of
// its own (a return, a jump, an error) releases.
void CodeGen::emitTry(TryStmt *stmt) {
  size_t depth = scopes_.size();
  {
    TryContext t;
    t.stmt = stmt;
    t.scopeDepth = depth;
    t.targetsDepth = targets_.size();
    if (stmt->handler)
      t.handler = newBlock("catch");
    if (stmt->finalizer) {
      t.finally = newBlock("finally");
      t.throwEntry = newBlock("finally.throw");
      t.completion = createStackSlot(builder_.getInt32Ty(), "completion");
      llvm::AllocaInst *slot = createStackSlot(builder_.getPtrTy(), "pending.error");
      llvm::IRBuilder<> init(slot->getParent(), std::next(slot->getIterator()));
      init.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
      t.pendingError = slot;
    }
    tries_.push_back(std::move(t));
  }
  size_t index = tries_.size() - 1;
  llvm::BasicBlock *endBlock = newBlock("try.end");
  // The block or the `catch` ended normally.
  auto leave = [&] {
    if (!blockIsOpen())
      return;
    if (stmt->finalizer) {
      builder_.CreateStore(builder_.getInt32(0), tries_[index].completion);
      builder_.CreateBr(tries_[index].finally);
    } else {
      builder_.CreateBr(endBlock);
    }
  };
  // Takes the error in flight.
  auto takeError = [&] {
    llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), thrownGlobal(), "error");
    builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), thrownGlobal());
    return error;
  };

  emitStmt(stmt->block);
  leave();

  if (stmt->handler) {
    tries_[index].phase = TryContext::Phase::Handler;
    switchTo(tries_[index].handler);
    llvm::Value *error = takeError();
    pushScope();
    if (const VarSymbol *var = stmt->param.sym)
      storeVar(var, createLocal(var, stmt->param.name), error, /*replacing=*/false);
    else
      releaseValue(error, errorClass_->type);
    emitStmt(stmt->handler);
    popScope();
    leave();
  }

  if (stmt->finalizer) {
    TryContext *t = &tries_[index];
    t->phase = TryContext::Phase::Finally;
    switchTo(t->throwEntry);
    builder_.CreateStore(takeError(), t->pendingError);
    builder_.CreateStore(builder_.getInt32(1), t->completion);
    builder_.CreateBr(t->finally);

    switchTo(t->finally);
    pushScope();
    const Type *valueType = returnedType(currentFn_, returnType_);
    scopes_.back().slots.push_back({t->pendingError, errorClass_->type});
    if (t->pendingValue && valueType->isManaged())
      scopes_.back().slots.push_back({t->pendingValue, valueType});
    emitStmt(stmt->finalizer);
    t = &tries_[index];
    if (!blockIsOpen()) {
      popScope();
    } else {
      // Ended normally: go on as the block or the `catch` did.
      llvm::Value *completion = builder_.CreateLoad(builder_.getInt32Ty(), t->completion, "completion");
      llvm::Value *error = builder_.CreateLoad(builder_.getPtrTy(), t->pendingError);
      builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), t->pendingError);
      llvm::Value *value = nullptr;
      if (t->pendingValue) {
        llvm::Type *lowered = lower(valueType);
        value = builder_.CreateLoad(lowered, t->pendingValue);
        builder_.CreateStore(llvm::Constant::getNullValue(lowered), t->pendingValue);
      }
      popScope();
      std::vector<TryContext::Exit> exits = tries_[index].exits;
      bool returns = tries_[index].returns;
      llvm::BasicBlock *rethrowBlock = newBlock("finally.rethrow");
      llvm::BasicBlock *returnBlock = returns ? newBlock("finally.return") : nullptr;
      llvm::SwitchInst *dispatch = builder_.CreateSwitch(completion, endBlock, unsigned(exits.size() + 2));
      dispatch->addCase(builder_.getInt32(1), rethrowBlock);
      if (returns)
        dispatch->addCase(builder_.getInt32(2), returnBlock);
      std::vector<llvm::BasicBlock *> exitBlocks;
      for (size_t k = 0; k < exits.size(); ++k) {
        exitBlocks.push_back(newBlock("finally.exit"));
        dispatch->addCase(builder_.getInt32(uint32_t(3 + k)), exitBlocks.back());
      }
      // The error goes on up, its places kept.
      switchTo(rethrowBlock);
      builder_.CreateStore(error, thrownGlobal());
      builder_.CreateBr(unwindTarget());
      if (returns) {
        switchTo(returnBlock);
        emitReturnOf(value);
        builder_.CreateUnreachable(); // ends the dead block after it
      }
      for (size_t k = 0; k < exits.size(); ++k) {
        switchTo(exitBlocks[k]);
        emitJumpTo(exits[k].target, exits[k].isBreak);
        builder_.CreateUnreachable();
      }
    }
  }
  tries_.pop_back();
  switchTo(endBlock);
}

//===----------------------------------------------------------------------===//
// The class Error
//===----------------------------------------------------------------------===//

// For inheritance (see sema_error.cpp): a class derived from Error calls this
// constructor for `super(message, options)` with its object as `this`; its
// descriptor needs TZ_LAYOUT_ERROR in its flags (as classDescriptor gives
// Error's), and its instances the fields of Error at the offsets that
// emitErrorSupport tells the runtime, which a subclass that keeps its
// parent's fields first has. A value of type Error is released through the
// descriptor of its object (releaseValue), so instances of subclasses are.
//
// constructor(message?: string, options?: { cause?: Error }): the message (or
// ""), the cause and the name "Error"; the places come when it is thrown.
void CodeGen::emitErrorConstructor(FunctionDecl *fn) {
  beginFunction(functions_.lookup(fn->sym), fn);
  const Type *type = fn->cls->type;
  llvm::Value *self = function_->getArg(0);
  llvm::Value *message = function_->getArg(1);
  llvm::Value *options = function_->getArg(2);
  const Type *messageType = fn->sym->params[0], *optionsType = fn->sym->params[1];
  llvm::Value *text = builder_.CreateSelect(optionalHas(message, messageType),
                                            optionalValue(message, messageType), stringLiteral(u""));
  builder_.CreateStore(text, fieldAddress(self, type, "message"));
  builder_.CreateStore(stringLiteral(u"Error"), fieldAddress(self, type, "name"));
  llvm::BasicBlock *causeBlock = newBlock("cause");
  llvm::BasicBlock *doneBlock = newBlock("done");
  builder_.CreateCondBr(optionalHas(options, optionsType), causeBlock, doneBlock);
  switchTo(causeBlock);
  const Type *object = optionsType->inner();
  llvm::Value *cause = builder_.CreateLoad(builder_.getPtrTy(), fieldAddress(options, object, "cause"));
  retainValue(cause, Type::optional(type));
  builder_.CreateStore(cause, fieldAddress(self, type, "cause"));
  // An Error may be a member of a group (codegen_cycles.cpp): the cause
  // joins it, as a store of the program's would.
  groupStore(self, cause, Type::optional(type));
  // The constructor owns its arguments. The options may be a literal of a
  // type of its own, a member of a group: let go of through its descriptor.
  if (hasCycles_)
    builder_.CreateCall(instanceReleaseFn_, {options});
  else
    releaseValue(options, object);
  builder_.CreateBr(doneBlock);
  switchTo(doneBlock);
  builder_.CreateRetVoid();
  function_ = nullptr;
}

// The class of a kind of error (an index of kErrorKinds): Error, or the
// class of the kind that extends it.
const ClassDecl *CodeGen::errorKindClass(int kind) const {
  if (kind > 0)
    for (const ClassDecl *child : errorClass_->children)
      if (child->builtin && child->name == kErrorKinds[kind])
        return child;
  return errorClass_;
}

// The descriptor of Error, and of each kind: the classes' own.
llvm::Constant *CodeGen::errorDescriptor(int kind) {
  return classDescriptor(errorKindClass(kind));
}

// (i32 kind, string message) -> Error: how the runtime makes the errors of
// its own failures (TzErrorInfo::make), and the built-ins of the program.
llvm::Function *CodeGen::errorMaker() {
  if (errorMaker_)
    return errorMaker_;
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty(), *i64 = builder_.getInt64Ty();
  errorMaker_ = llvm::Function::Create(llvm::FunctionType::get(ptr, {i32, ptr, i64}, false),
                                       llvm::GlobalValue::InternalLinkage, "tz.error.new", module_.get());
  setFunctionAttributes(errorMaker_);
  errorMaker_->addFnAttr(llvm::Attribute::Cold);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  llvm::Function *outer = std::exchange(function_, errorMaker_);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", errorMaker_));
  size_t count = std::size(kErrorKinds);
  std::vector<llvm::Constant *> descriptors;
  for (size_t k = 0; k < count; ++k)
    descriptors.push_back(errorDescriptor(int(k)));
  auto *tableType = llvm::ArrayType::get(ptr, count);
  auto *table = new llvm::GlobalVariable(*module_, tableType, /*isConstant=*/true,
                                         llvm::GlobalValue::PrivateLinkage,
                                         llvm::ConstantArray::get(tableType, descriptors), "tz.errors");
  llvm::Value *kind = errorMaker_->getArg(0);
  llvm::Value *descriptor = builder_.CreateLoad(
      ptr, builder_.CreateInBoundsGEP(tableType, table, {builder_.getInt64(0), kind}), "layout");
  const Type *type = errorClass_->type;
  const ObjectLayout &layout = objectLayout(type);
  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "error");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  builder_.CreateStore(descriptor, builder_.CreateStructGEP(layout.type, object, 1));
  builder_.CreateMemSet(builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 16),
                        builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  llvm::Value *message = llvm::UndefValue::get(strTy_);
  message = builder_.CreateInsertValue(message, errorMaker_->getArg(1), 0);
  message = builder_.CreateInsertValue(message, errorMaker_->getArg(2), 1);
  builder_.CreateStore(message, fieldAddress(object, type, "message"));
  // The name of the kind: a literal, one of a table like the descriptors.
  std::vector<llvm::Constant *> names;
  for (size_t k = 0; k < count; ++k)
    names.push_back(stringLiteral(utf16(kErrorKinds[k])));
  auto *namesType = llvm::ArrayType::get(strTy_, count);
  auto *namesTable = new llvm::GlobalVariable(*module_, namesType, /*isConstant=*/true,
                                              llvm::GlobalValue::PrivateLinkage,
                                              llvm::ConstantArray::get(namesType, names), "tz.error.names");
  llvm::Value *name = builder_.CreateLoad(
      strTy_, builder_.CreateInBoundsGEP(namesType, namesTable, {builder_.getInt64(0), kind}));
  builder_.CreateStore(name, fieldAddress(object, type, "name"));
  builder_.CreateRet(object);
  function_ = outer;
  builder_.restoreIP(point);
  return errorMaker_;
}

// What main tells the runtime first: how to make an Error, and where its
// fields are (TzErrorInfo in runtime/tzrt.h).
void CodeGen::emitErrorSupport() {
  if (!errorClass_)
    return;
  const Type *type = errorClass_->type;
  const ObjectLayout &layout = objectLayout(type);
  const llvm::StructLayout *offsets = module_->getDataLayout().getStructLayout(layout.type);
  auto offset = [&](llvm::StringRef field) {
    return builder_.getInt32(
        uint32_t(offsets->getElementOffset(layout.elements[fieldIndex(type, field)])));
  };
  llvm::Type *ptr = builder_.getPtrTy(), *i32 = builder_.getInt32Ty();
  auto *infoType = llvm::StructType::get(ctx_, {ptr, ptr, i32, i32, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      infoType, {errorMaker(), typeDescriptor(Type::longTy()), offset("name"), offset("message"),
                 offset("cause"), offset("#frames")});
  auto *info = new llvm::GlobalVariable(*module_, infoType, /*isConstant=*/true,
                                        llvm::GlobalValue::PrivateLinkage, init, "tz.error.info");
  builder_.CreateCall(processFunction("tz_error_init", builder_.getVoidTy(), {ptr}), {info});
}

// `new Error(message, options)` and the kinds: the constructor of Error, and
// the name of the kind.
llvm::Value *CodeGen::emitNewError(NewExpr *expr) {
  int kind = std::max(errorKindIndex(expr->cls->name), 0);
  const CallExpr *ctor = expr->ctor;
  const FuncSymbol *sym = ctor->func;
  std::vector<llvm::Value *> args = emitHeldValues(ctor->args);
  for (size_t i = args.size(); i < sym->params.size(); ++i)
    args.push_back(undefinedValue(sym->params[i]));
  const Type *type = errorClass_->type;
  const ObjectLayout &layout = objectLayout(type);
  uint64_t size = module_->getDataLayout().getTypeAllocSize(layout.type);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "error");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(layout.type, object, 0));
  builder_.CreateStore(errorDescriptor(kind), builder_.CreateStructGEP(layout.type, object, 1));
  builder_.CreateMemSet(builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 16),
                        builder_.getInt8(0), size - 16, llvm::MaybeAlign(8));
  args.insert(args.begin(), object);
  builder_.CreateCall(methodCode(sym->decl), args);
  if (kind != 0)
    builder_.CreateStore(stringLiteral(utf16(kErrorKinds[kind])), fieldAddress(object, type, "name"));
  return object;
}

// `e.stack` and `e.toString()`.
llvm::Value *CodeGen::emitErrorBuiltin(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  Operand error = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Type *ptr = builder_.getPtrTy();
  const char *name = call->builtin == Builtin::ErrorStack ? "tz_error_stack" : "tz_error_to_string";
  llvm::Value *text =
      callReturningString(processFunction(name, builder_.getVoidTy(), {ptr, ptr}), {error.value});
  dropOperand(error);
  return text;
}

//===----------------------------------------------------------------------===//
// Result.fromThrowable and _unsafeUnwrap
//===----------------------------------------------------------------------===//

// A function value whose environment holds f and toError: its code calls f
// with its arguments and makes a Result of what f returns, or of the error
// f throws, taken from tz_thrown (and given to toError).
llvm::Value *CodeGen::emitFromThrowable(CallExpr *call) {
  const Type *fnType = call->args[0]->type;
  const Type *toErrorType = call->args.size() > 1 ? call->args[1]->type : nullptr;
  const Type *madeType = call->type, *resultType = madeType->ret();
  std::vector<llvm::Value *> values = emitHeldValues(call->args);
  llvm::Type *ptr = builder_.getPtrTy(), *i64 = builder_.getInt64Ty();
  auto *envType = llvm::StructType::get(ctx_, {i64, ptr, fnTy_, fnTy_});

  // The environment, which holds both functions (a null one without
  // toError) and releases them when it is dropped.
  auto *dropType = llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false);
  auto *drop = llvm::Function::Create(dropType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable.env.drop", module_.get());
  drop->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilderBase::InsertPoint point = builder_.saveIP();
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", drop));
  for (unsigned i : {2u, 3u})
    releaseValue(builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, drop->getArg(0), i)),
                 fnType);
  builder_.CreateCall(rt_.objFree, {drop->getArg(0)});
  builder_.CreateRetVoid();

  // The code.
  llvm::FunctionType *codeType = closureFnType(madeType);
  auto *code = llvm::Function::Create(codeType, llvm::GlobalValue::InternalLinkage,
                                      "tz.fromThrowable", module_.get());
  setFunctionAttributes(code);
  llvm::Function *outer = std::exchange(function_, code);
  builder_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "entry", code));
  llvm::Value *env = code->getArg(0);
  std::vector<llvm::Value *> args;
  for (unsigned i = 1; i < code->arg_size(); ++i)
    args.push_back(code->getArg(i));
  llvm::Value *f = builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, env, 2), "f");
  llvm::Value *returned = callFunctionValue(f, fnType, args);
  llvm::Value *thrown = builder_.CreateLoad(ptr, thrownGlobal(), "thrown");
  llvm::BasicBlock *okBlock = newBlock("ok");
  llvm::BasicBlock *errBlock = newBlock("err");
  builder_.CreateCondBr(builder_.CreateIsNull(thrown), okBlock, errBlock);
  switchTo(okBlock);
  const Type *value = fnType->ret();
  builder_.CreateRet(makeResult(resultType, builder_.getTrue(),
                                convertHalf(value->isVoid() ? nullptr : returned, value,
                                            resultType->okType()),
                                nullptr));
  switchTo(errBlock);
  builder_.CreateStore(llvm::ConstantPointerNull::get(ptr), thrownGlobal());
  llvm::Value *error = thrown;
  if (toErrorType) {
    llvm::Value *toError = builder_.CreateLoad(fnTy_, builder_.CreateStructGEP(envType, env, 3), "toError");
    std::vector<llvm::Value *> errorArgs;
    if (!toErrorType->params().empty())
      errorArgs.push_back(thrown);
    else
      releaseValue(thrown, errorClass_->type);
    error = callFunctionValue(toError, toErrorType, errorArgs);
    // toError threw in turn: that goes to the caller.
    if (typeThrows(toErrorType)) {
      llvm::BasicBlock *madeBlock = newBlock("made");
      llvm::BasicBlock *failedBlock = newBlock("failed");
      builder_.CreateCondBr(
          builder_.CreateIsNull(builder_.CreateLoad(ptr, thrownGlobal())), madeBlock, failedBlock);
      switchTo(failedBlock);
      builder_.CreateRet(llvm::Constant::getNullValue(lower(resultType)));
      switchTo(madeBlock);
    }
  }
  builder_.CreateRet(makeResult(resultType, builder_.getFalse(), nullptr, error));
  function_ = outer;
  builder_.restoreIP(point);

  auto *info = new llvm::GlobalVariable(
      *module_, fnInfoTy_, /*isConstant=*/true, llvm::GlobalValue::PrivateLinkage,
      llvm::ConstantStruct::get(fnInfoTy_, {code, stringLiteral(u"")}), "tz.fninfo.fromThrowable");
  uint64_t size = module_->getDataLayout().getTypeAllocSize(envType);
  llvm::Value *object = builder_.CreateCall(rt_.objAlloc, {builder_.getInt64(size)}, "env");
  builder_.CreateStore(builder_.getInt64(1), builder_.CreateStructGEP(envType, object, 0));
  builder_.CreateStore(dropDescriptor(drop), builder_.CreateStructGEP(envType, object, 1));
  builder_.CreateStore(values[0], builder_.CreateStructGEP(envType, object, 2));
  builder_.CreateStore(values.size() > 1 ? values[1] : llvm::Constant::getNullValue(fnTy_),
                       builder_.CreateStructGEP(envType, object, 3));
  return makeFunctionValue(info, object);
}

// The half asked for, or an Error thrown for the other one, as neverthrow's
// messages say.
llvm::Value *CodeGen::emitUnsafeUnwrap(CallExpr *call) {
  auto *member = cast<MemberExpr>(call->callee);
  const Type *type = member->object->type;
  bool wantErr = call->builtin == Builtin::ResultUnsafeUnwrapErr;
  Operand result = emitOperand(member->object, /*canBorrow=*/true);
  llvm::Value *ok = resultIsOk(result.value, type);
  llvm::BasicBlock *goodBlock = newBlock("unwrap");
  llvm::BasicBlock *badBlock = newBlock("unwrap.other");
  builder_.CreateCondBr(wantErr ? builder_.CreateNot(ok) : ok, goodBlock, badBlock,
                        llvm::MDBuilder(ctx_).createLikelyBranchWeights());
  switchTo(badBlock);
  dropOperand(result);
  emitThrowNew(0, wantErr ? "Called `_unsafeUnwrapErr` on an Ok" : "Called `_unsafeUnwrap` on an Err");
  switchTo(goodBlock);
  const Type *half = wantErr ? type->errType() : type->okType();
  llvm::Value *value = nullptr;
  if (!half->isVoid() && !half->isNever()) {
    value = resultHalf(result.value, type, wantErr);
    retainValue(value, half);
  }
  dropOperand(result);
  if (call->type->isUndefined() || !value)
    return value ? value : llvm::Constant::getNullValue(lower(call->type));
  return emitConversion(value, half, call->type);
}

//===----------------------------------------------------------------------===//
// Held values
//===----------------------------------------------------------------------===//

CodeGen::Held CodeGen::hold(llvm::Value *value, const Type *type, bool needed) {
  if (!needed || !value || !type || !type->isManaged() || isa<llvm::Constant>(value))
    return {value, nullptr, type};
  llvm::Value *slot = createManagedSlot(type, "held");
  builder_.CreateStore(value, slot);
  return {value, slot, type};
}

llvm::Value *CodeGen::take(Held &held) {
  if (!held.slot)
    return held.value;
  llvm::Type *lowered = lower(held.type);
  held.value = builder_.CreateLoad(lowered, held.slot);
  builder_.CreateStore(llvm::Constant::getNullValue(lowered), held.slot);
  held.slot = nullptr;
  return held.value;
}

void CodeGen::holdOperand(Operand &operand, bool needed) {
  if (operand.owned)
    operand.heldSlot = hold(operand.value, operand.type, needed).slot;
}

void CodeGen::takeOperand(Operand &operand) {
  if (!operand.heldSlot)
    return;
  Held held{operand.value, operand.heldSlot, operand.type};
  operand.value = take(held);
  operand.heldSlot = nullptr;
}

std::vector<llvm::Value *> CodeGen::emitHeldValues(llvm::ArrayRef<Expr *> exprs) {
  std::vector<bool> laterThrows(exprs.size(), false);
  for (size_t i = exprs.size(); i > 1; --i)
    laterThrows[i - 2] = laterThrows[i - 1] || exprThrows(exprs[i - 1]);
  std::vector<Held> held;
  for (size_t i = 0; i < exprs.size(); ++i)
    held.push_back(hold(emitExpr(exprs[i]), exprs[i]->type, laterThrows[i]));
  std::vector<llvm::Value *> values;
  for (Held &h : held)
    values.push_back(take(h));
  return values;
}

} // namespace tz
