#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"

#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

// The names a statement mentions, to know which globals functions may use.
// With `inFunction` unset, only the names inside the functions it contains
// count.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction) {
  if (!stmt)
    return;
  auto expr = [&](const Expr *e) { collectNames(e, names, inFunction); };
  auto sub = [&](const Stmt *s) { collectNames(s, names, inFunction); };
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body)
      sub(s);
    break;
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : cast<VarDeclStmt>(stmt)->decls)
      if (d.init)
        expr(d.init);
    break;
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    for (const Param &param : fn->params)
      if (param.defaultValue)
        collectNames(param.defaultValue, names, true);
    collectNames(fn->body, names, true);
    break;
  }
  case Stmt::Kind::Expr:
    expr(cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    expr(s->cond);
    sub(s->thenStmt);
    sub(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    expr(cast<WhileStmt>(stmt)->cond);
    sub(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    expr(cast<DoWhileStmt>(stmt)->cond);
    sub(cast<DoWhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    sub(s->init);
    if (s->cond)
      expr(s->cond);
    if (s->step)
      expr(s->step);
    sub(s->body);
    break;
  }
  case Stmt::Kind::ForOf:
    expr(cast<ForOfStmt>(stmt)->iterable);
    sub(cast<ForOfStmt>(stmt)->body);
    break;
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      if (c.test)
        expr(c.test);
      for (const Stmt *body : c.body)
        sub(body);
    }
    break;
  }
  case Stmt::Kind::Labeled:
    sub(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value)
      expr(value);
    break;
  case Stmt::Kind::Throw:
    expr(cast<ThrowStmt>(stmt)->value);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    sub(s->block);
    sub(s->handler);
    sub(s->finalizer);
    break;
  }
  case Stmt::Kind::Class: {
    // The methods and the initializers of the fields run as functions; the
    // static initializers where the class is declared.
    auto *cls = cast<ClassDecl>(stmt);
    if (cls->baseExpr)
      collectNames(cls->baseExpr, names, inFunction);
    for (const FunctionDecl *fn : cls->methods)
      collectNames(fn, names, true);
    for (const FunctionDecl *fn : cls->constructors)
      collectNames(fn, names, true);
    for (const ClassField &f : cls->fields)
      if (f.init)
        collectNames(f.init, names, f.isStatic ? inFunction : true);
    break;
  }
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  }
}

void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction) {
  auto sub = [&](const Expr *e) { collectNames(e, names, inFunction); };
  switch (expr->kind()) {
  case Expr::Kind::Name:
    if (inFunction)
      names.insert(cast<NameExpr>(expr)->name);
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements)
      sub(e.expr);
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      if (p.key)
        sub(p.key);
      sub(p.value);
    }
    break;
  case Expr::Kind::Member:
    sub(cast<MemberExpr>(expr)->object);
    break;
  case Expr::Kind::Index:
    sub(cast<IndexExpr>(expr)->object);
    sub(cast<IndexExpr>(expr)->index);
    break;
  case Expr::Kind::Unary:
    sub(cast<UnaryExpr>(expr)->operand);
    break;
  case Expr::Kind::Binary:
    sub(cast<BinaryExpr>(expr)->lhs);
    sub(cast<BinaryExpr>(expr)->rhs);
    break;
  case Expr::Kind::Assign:
    sub(cast<AssignExpr>(expr)->target);
    sub(cast<AssignExpr>(expr)->value);
    break;
  case Expr::Kind::Update:
    sub(cast<UpdateExpr>(expr)->target);
    break;
  case Expr::Kind::Call:
    sub(cast<CallExpr>(expr)->callee);
    for (const Expr *arg : cast<CallExpr>(expr)->args)
      sub(arg);
    break;
  case Expr::Kind::New: {
    // Once checked, the arguments of a class's constructor are in its call.
    // `new C()` may name a variable that holds a class.
    auto *create = cast<NewExpr>(expr);
    if (inFunction && !create->name.empty())
      names.insert(create->name);
    if (create->callee)
      sub(create->callee);
    for (const Expr *arg : create->ctor ? create->ctor->args : create->args)
      sub(arg);
    break;
  }
  case Expr::Kind::Conditional:
    sub(cast<ConditionalExpr>(expr)->cond);
    sub(cast<ConditionalExpr>(expr)->thenExpr);
    sub(cast<ConditionalExpr>(expr)->elseExpr);
    break;
  case Expr::Kind::Cast:
    sub(cast<CastExpr>(expr)->operand);
    break;
  case Expr::Kind::Sequence:
    for (const Expr *e : cast<SequenceExpr>(expr)->exprs)
      sub(e);
    break;
  case Expr::Kind::OptionalChain:
    sub(cast<OptionalChainExpr>(expr)->chain);
    break;
  case Expr::Kind::Function:
    collectNames(cast<FunctionExpr>(expr)->fn, names, true);
    break;
  case Expr::Kind::Yield:
    if (const Expr *value = cast<YieldExpr>(expr)->value)
      sub(value);
    break;
  case Expr::Kind::Await:
    sub(cast<AwaitExpr>(expr)->value);
    break;
  case Expr::Kind::Class: // as a declaration of the class
    collectNames(cast<ClassExpr>(expr)->decl, names, inFunction);
    break;
  default:
    break;
  }
}

std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i) {
  std::vector<const Stmt *> following;
  for (size_t j = 0; j < i; ++j)
    if (isa<FunctionDecl>(stmts[j]))
      following.push_back(stmts[j]);
  following.insert(following.end(), stmts.begin() + i + 1, stmts.end());
  return following;
}

void Sema::check(SourceFile &file) {
  file_ = &file;
  if (file.modules.empty()) {
    // A program of one file that did not come from the loader.
    Module *module = ctx_.newModule();
    module->isEntry = true;
    module->file.items = file.items;
    module->file.imports = std::move(file.imports);
    module->file.exports = std::move(file.exports);
    module->loadPath = {module};
    file.modules.push_back(module);
  }
  pushScope(); // the functions and globals of a file (see useModule)
  prepareLibraries(file);
  declareErrorClass();
  declareFsClasses();
  declareExecSyncError();
  declarePrelude();

  // Only the fields assigned somewhere can change. `o[key] = v` assigns the
  // field the key names (see keyNames), and an element of a tuple by its
  // number. An arrow in a field of a class that nothing assigns is a method
  // (see resolveClass).
  for (Stmt *item : file.items)
    visitExprs(item, [&](const Expr *e) {
      const Expr *target = nullptr;
      if (auto *assign = dyn_cast<AssignExpr>(e))
        target = assign->target;
      else if (auto *update = dyn_cast<UpdateExpr>(e))
        target = update->target;
      if (auto *member = llvm::dyn_cast_or_null<MemberExpr>(target))
        assignedFields_.insert(member->name);
      else if (auto *index = llvm::dyn_cast_or_null<IndexExpr>(target))
        keyNames(index->index, [&](llvm::StringRef name) { assignedFields_.insert(name); });
    });

  // Types and functions are visible in the whole file, as in JavaScript, and
  // those of every file exist before any code runs.
  std::vector<FunctionDecl *> functions = declareModules(file);
  findCycles(functions,
             [&](FunctionDecl *fn, llvm::StringRef name) { return cycleCallee(fn, name); });
  findMetadataWriters(file);

  // A global no function mentions cannot change during a call, and can live in
  // main's stack frame. That includes the arrows of top-level code.
  for (Stmt *item : file.items)
    collectNames(item, namesInFunctions_, /*inFunction=*/isa<FunctionDecl>(item));
  noteImportsInFunctions(file);

  // Top-level code runs in order, file by file in the order they load; its
  // `let` and `const` become globals. A file sees the variables of the files
  // that load before it.
  flow_ = Flow();
  for (Module *module : file.modules)
    checkTopLevel(module);
  topLevelDone_ = true;
  for (Module *module : file.modules) {
    useModule(module);
    bindImports();
  }

  // Bodies come last so that functions can use globals declared after them,
  // except those whose return type top-level code needed first. Nothing is
  // known about the globals when a function starts.
  for (Module *module : file.modules) {
    useModule(module);
    for (Stmt *item : module->file.items) {
      // A class whose base is a value is checked with those made late.
      if (auto *cls = dyn_cast<ClassDecl>(item); cls && cls->type) {
        if (!cls->baseExpr)
          checkClassBodies(cls);
        continue;
      }
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        // The body of a template is checked in its copies only.
        if (isTemplate(fn->sym))
          continue;
        if (!fn->sym->ret)
          inferReturnType(fn->sym);
        if (fn->checked)
          continue;
        flow_ = Flow();
        checkFunctionBody(fn);
      }
    }
  }
  for (FunctionDecl *fn : lifted_) {
    if (isTemplate(fn->sym))
      continue;
    if (!fn->sym->ret)
      inferReturnType(fn->sym);
    if (!fn->checked) {
      flow_ = Flow();
      checkFunctionBody(fn);
    }
  }
  // The copies of generic functions that calls need, now that every generic
  // body is checked, and the classes that class expressions made, which may
  // need more of each other.
  do {
    checkLateClasses();
    checkInstances();
  } while (lateChecked_ < lateClasses_.size());
  checkConstructorCalls();

  checkLoadOrder(file);
  findUnused(file);
  finishReflection();
  // The globals without a type, file by file.
  for (Module *module : file.modules) {
    useModule(module);
    if (!untyped_.empty())
      reportUntyped();
  }
  popScope();
  // Error and its kinds are classes of the program from here on, for CodeGen,
  // and so are the instances of generic classes, numbered with the others.
  for (ClassDecl *kind : errorKinds_)
    file.items.push_back(kind);
  file.items.push_back(systemErrorClass_);
  file.items.push_back(execSyncErrorClass_);
  numberClasses();
  for (ClassDecl *cls : classes_)
    if ((cls->generic || (cls->expression && cls->type)) && cls->isEmitted())
      file.items.push_back(cls);
  // Writes through a wider type, now that every write is known.
  if (!diags_.hasErrors())
    checkWidenings(file);
}

//===----------------------------------------------------------------------===//
// Scopes and types
//===----------------------------------------------------------------------===//

void Sema::findCycles(llvm::ArrayRef<FunctionDecl *> functions,
                      llvm::function_ref<FunctionDecl *(FunctionDecl *, llvm::StringRef)> resolve) {
  // Tarjan's algorithm over the calls between functions without a return
  // type. A name that only looks like such a call (a variable with the same
  // name) costs a few more checks, nothing else.
  std::vector<FunctionDecl *> nodes;
  llvm::StringMap<size_t> byName;
  llvm::DenseMap<const FunctionDecl *, size_t> byDecl;
  for (FunctionDecl *fn : functions)
    if (fn->sym && !fn->sym->ret && !inheritsReturn_.count(fn) && !returnSources_.count(fn)) {
      byName[fn->name] = nodes.size();
      byDecl[fn] = nodes.size();
      nodes.push_back(fn);
    }
  std::vector<std::vector<size_t>> calls(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    llvm::StringSet<> names;
    for (const Param &param : nodes[i]->params)
      if (param.defaultValue)
        collectNames(param.defaultValue, names);
    collectNames(nodes[i]->body, names);
    // `o.m(...)` may call any method named m.
    llvm::StringSet<> members;
    collectMemberCalls(nodes[i]->body, members);
    for (const auto &member : members)
      for (size_t j = 0; j < nodes.size(); ++j)
        if (nodes[j]->cls && nodes[j]->name == member.getKey())
          calls[i].push_back(j);
    for (const auto &name : names) {
      if (!resolve) {
        if (auto found = byName.find(name.getKey()); found != byName.end())
          calls[i].push_back(found->second);
      } else if (FunctionDecl *callee = resolve(nodes[i], name.getKey())) {
        if (auto found = byDecl.find(callee); found != byDecl.end())
          calls[i].push_back(found->second);
      }
    }
  }

  std::vector<int> index(nodes.size(), -1), low(nodes.size(), 0);
  std::vector<bool> onStack(nodes.size(), false);
  std::vector<size_t> stack;
  int next = 0;
  std::function<void(size_t)> visit = [&](size_t v) {
    index[v] = low[v] = next++;
    stack.push_back(v);
    onStack[v] = true;
    for (size_t w : calls[v]) {
      if (index[w] < 0) {
        visit(w);
        low[v] = std::min(low[v], low[w]);
      } else if (onStack[w]) {
        low[v] = std::min(low[v], index[w]);
      }
    }
    if (low[v] != index[v])
      return;
    std::vector<FuncSymbol *> cycle;
    size_t w;
    do {
      w = stack.back();
      stack.pop_back();
      onStack[w] = false;
      cycle.push_back(nodes[w]->sym);
    } while (w != v);
    if (cycle.size() == 1 && !llvm::is_contained(calls[v], v))
      return; // not recursive
    for (FuncSymbol *fn : cycle)
      cycleOf_[fn] = static_cast<unsigned>(cycles_.size());
    cycles_.push_back(std::move(cycle));
  };
  for (size_t v = 0; v < nodes.size(); ++v)
    if (index[v] < 0)
      visit(v);
}

void Sema::declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry) {
  if (name == "undefined") {
    diags_.error(loc, "'undefined' cannot be used as a name");
    return;
  }
  auto [it, inserted] = scopes_.back().try_emplace(name, entry);
  if (inserted)
    return;
  const ScopeEntry &previous = it->second;
  // Two functions with one name are not overloads: plain functions have none
  // (docs/overloads.md).
  diags_.error(loc, "duplicate declaration of '" + name + "'" +
                        (entry.func && previous.func
                             ? "; plain functions are not overloaded: give each variant its own name"
                             : ""));
  diags_.note(previous.var        ? previous.var->loc
              : previous.func     ? previous.func->loc
              : previous.enumDecl ? previous.enumDecl->loc()
                                  : previous.cls->loc(),
              "previously declared here");
}

Sema::ScopeEntry Sema::lookup(llvm::StringRef name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end())
      return found->second;
  }
  // Error and its kinds, which the program's own names hide.
  if (ClassDecl *kind = errorKindClass(name))
    return {nullptr, nullptr, nullptr, kind};
  return {};
}

VarSymbol *Sema::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst) {
  VarSymbol *var = ctx_.newVar();
  var->name = name;
  var->loc = loc;
  var->type = type;
  var->isConst = isConst;
  var->isGlobal = scopes_.size() == 1;
  var->usedInFunction = var->isGlobal && namesInFunctions_.contains(name);
  var->owner = var->isGlobal ? nullptr : currentDecl();
  var->declSeq = ++seq_;
  declare(name, loc, {var, nullptr});
  return var;
}

const Type *Sema::resolveType(const TypeRef &ref, bool allowVoid) {
  const Type *type = resolveTypeInner(ref);
  if (type->isVoid() && !allowVoid) {
    diags_.error(ref.loc, "'void' can only be used as a return type");
    return Type::error();
  }
  if (type->isUndefined()) {
    diags_.error(ref.loc, "the type 'undefined' alone is not supported; use 'T | undefined'");
    return Type::error();
  }
  return type;
}

const Type *Sema::resolveTypeInner(const TypeRef &ref) {
  switch (ref.kind) {
  case TypeRef::Kind::Name: {
    if (ref.name.empty())
      return Type::error(); // the parser already reported it
    if (const Type *type = Type::fromName(ref.name))
      return type;
    // What cannot happen: the value after every member of a union is ruled
    // out, the type of a function that never returns.
    if (ref.name == "never" && !lookupType(ref.name))
      return Type::never();
    if (Stmt *decl = lookupType(ref.name)) {
      if (auto *alias = dyn_cast<TypeAliasDecl>(decl))
        return alias->typeParams.empty() ? resolveAlias(alias) : instantiateAlias(alias, {}, ref.loc);
      if (auto *cls = dyn_cast<ClassDecl>(decl)) {
        // A generic class names a type with its type arguments, unless
        // they all have defaults.
        if (cls->isGeneric()) {
          ClassDecl *inst = instantiateClass(cls, {}, ref.loc);
          return inst ? inst->type : Type::error();
        }
        return finalClass(cls)->type;
      }
      auto *e = cast<EnumDecl>(decl);
      resolveEnum(e);
      return e->isString ? Type::string() : Type::intTy();
    }
    if (ref.name.starts_with("Intl.")) {
      if (const Type *intl = intlTypeNamed(ref.name.drop_front(5)))
        return intl;
      diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
      return Type::error();
    }
    // What TypeScript calls the texts a tag gets from its template: here a
    // plain array of strings, without `raw` (see checkMember).
    if (ref.name == "TemplateStringsArray")
      return Type::array(Type::string());
    if (ref.name == "RegExp")
      return Type::regExp();
    if (ref.name == "Date")
      return Type::date();
    // What setTimeout and setImmediate give, also as @types/node names them.
    if (ref.name == "Timeout" || ref.name == "NodeJS.Timeout")
      return Type::timeout();
    if (ref.name == "Immediate" || ref.name == "NodeJS.Immediate")
      return Type::immediate();
    if (const Type *bytes = bytesTypeNamed(ref.name))
      return bytes;
    // The Interface of node:readline, by the name imported.
    if (const NodeName *n = nodeName(ref.name);
        n && (n->module == NodeModule::Readline || n->module == NodeModule::ReadlinePromises) &&
        n->name == "Interface")
      return Type::runtimeObject(n->module == NodeModule::Readline
                                     ? Type::Runtime::Readline
                                     : Type::Runtime::ReadlinePromises);
    if (const Type *crypto = cryptoTypeNamed(ref.name))
      return crypto;
    // A match of some regex; one of a known regex converts to it.
    if (ref.name == "RegExpMatchArray" || ref.name == "RegExpExecArray")
      return Type::match(-1, {}, nullptr, false);
    if (const Type *dict = resolveDictType(ref))
      return dict; // an error: it needs type arguments
    // Any class, and the parameters of the constructor of the class extended
    // (docs/decorators.md, section 5).
    if (ref.name == "Constructor")
      return Type::constructorOf(Type::anyObject());
    if (ref.name == "ConstructorArgs")
      return constructorArgs(ref.loc);
    if (ref.name == "Array")
      diags_.error(ref.loc, "'Array' needs an element type, e.g. 'Array<int>' or 'int[]'");
    else if (llvm::StringRef result = neverthrowTypeName(ref.name); !result.empty())
      diags_.error(ref.loc, "'" + ref.name +
                                "' needs the types of its value and its error, e.g. '" +
                                ref.name + "<int, string>'");
    else if (ref.name == "Generator" || ref.name == "IterableIterator")
      diags_.error(ref.loc, "'" + ref.name + "' needs the type of the values it gives, e.g. '" +
                                ref.name + "<int>'");
    else if (ref.name == "Promise")
      diags_.error(ref.loc, "'Promise' needs the type of its value, e.g. 'Promise<int>' or "
                            "'Promise<void>'");
    else if (ref.name == "null")
      diags_.error(ref.loc, "'null' is not supported: the only value for nothing is 'undefined'; "
                            "write 'T | undefined'");
    else if (ref.name == "bigint")
      reportBigInt(ref.loc, "the type 'bigint'");
    else if (ref.name == "BigInt64Array" || ref.name == "BigUint64Array")
      reportBigInt(ref.loc, "'" + ref.name.str() + "'");
    else if (ref.name == "SharedArrayBuffer")
      diags_.error(ref.loc, "'SharedArrayBuffer' is not supported yet: it comes with the shared "
                            "memory of async (docs/bytes.md, section 7); use 'ArrayBuffer'");
    else
      diags_.error(ref.loc, "cannot find type '" + ref.name + "'");
    return Type::error();
  }
  case TypeRef::Kind::Generic:
    // `Uint8Array<ArrayBuffer>` of TypeScript 5.7: the buffer is always an
    // ArrayBuffer here.
    if (const Type *bytes = bytesTypeNamed(ref.name); bytes && bytes->isTypedArray() &&
                                                      !lookupType(ref.name)) {
      if (ref.args.size() != 1 || ref.args[0].kind != TypeRef::Kind::Name ||
          (ref.args[0].name != "ArrayBuffer" && ref.args[0].name != "ArrayBufferLike")) {
        diags_.error(ref.loc, "the type argument of '" + ref.name +
                                  "' can only be 'ArrayBuffer'; or leave it out");
        return Type::error();
      }
      return bytes;
    }
    if (llvm::StringRef result = neverthrowTypeName(ref.name); !result.empty())
      return resolveResultType(ref, result);
    // ReadonlyArray<T> is for tsc; changes to it are not stopped here.
    if (ref.name == "Array" || ref.name == "ReadonlyArray") {
      if (ref.args.size() != 1) {
        diags_.error(ref.loc, "'" + ref.name + "' takes one type argument");
        return Type::error();
      }
      TypeRef array;
      array.kind = TypeRef::Kind::Array;
      array.loc = ref.loc;
      array.args = ref.args;
      return resolveTypeInner(array);
    }
    // `Box<int>`: an instance of a generic class.
    if (auto *cls = llvm::dyn_cast_or_null<ClassDecl>(lookupType(ref.name))) {
      std::vector<const Type *> args;
      for (const TypeRef &arg : ref.args) {
        args.push_back(resolveType(arg, /*allowVoid=*/false));
        if (args.back()->isError())
          return args.back();
      }
      if (!cls->isGeneric()) {
        diags_.error(ref.loc, "class '" + ref.name + "' is not generic; remove the type arguments");
        return Type::error();
      }
      ClassDecl *inst = instantiateClass(cls, std::move(args), ref.loc);
      return inst ? inst->type : Type::error();
    }
    // `Pair<int>`
    if (auto *alias = llvm::dyn_cast_or_null<TypeAliasDecl>(lookupType(ref.name))) {
      std::vector<const Type *> args;
      for (const TypeRef &arg : ref.args) {
        args.push_back(resolveTypeArgument(arg));
        if (args.back()->isError())
          return args.back();
      }
      if (alias->typeParams.empty()) {
        diags_.error(ref.loc, "type '" + ref.name + "' is not generic; remove the type arguments");
        return Type::error();
      }
      return instantiateAlias(alias, std::move(args), ref.loc);
    }
    if (const Type *iterator = resolveIteratorType(ref))
      return iterator;
    if (const Type *dict = resolveDictType(ref))
      return dict;
    if (const Type *promise = resolvePromiseType(ref))
      return promise;
    if (ref.name == "MethodDescriptor") {
      // What a decorator of a method gets (docs/decorators.md, section 2).
      if (ref.args.size() != 2) {
        diags_.error(ref.loc, "'MethodDescriptor' takes two type arguments: the types of the "
                              "method's parameters and what it returns, e.g. "
                              "'MethodDescriptor<[int, int], int>'");
        return Type::error();
      }
      const Type *args = resolveType(ref.args[0], /*allowVoid=*/false);
      const Type *ret = resolveType(ref.args[1], /*allowVoid=*/true);
      if (args->isError() || ret->isError())
        return Type::error();
      if (!args->isTuple() && !args->isListParam()) {
        diags_.error(ref.args[0].loc, "the first type argument of 'MethodDescriptor' is the list of "
                                      "the method's parameter types, a tuple such as '[int, "
                                      "string]', or 'A' of '<A extends unknown[]>'");
        return Type::error();
      }
      return methodDescriptorType(args, ret);
    }
    if (const Type *utility = resolveUtilityType(ref))
      return utility;
    if (ref.name == "Constructor")
      return resolveConstructorType(ref);
    // The type of the objects a class makes: `InstanceType<typeof Point>`.
    if (ref.name == "InstanceType" && ref.args.size() == 1) {
      const Type *cls = resolveType(ref.args[0], /*allowVoid=*/false);
      if (cls->isError())
        return cls;
      const Type *made = cls->isClassRef() ? constructedBy(cls) : nullptr;
      if (!made) {
        diags_.error(ref.args[0].loc, "'InstanceType' takes a class as a value, e.g. "
                                      "'InstanceType<typeof Point>', found '" + cls->name() + "'");
        return Type::error();
      }
      return made;
    }
    diags_.error(ref.loc, "generic type '" + ref.name + "' is not supported");
    return Type::error();
  case TypeRef::Kind::Array: {
    const Type *element = resolveTypeInner(ref.args[0]);
    if (element->isVoid() || element->isUndefined()) {
      diags_.error(ref.loc, "arrays of '" + element->name() + "' are not supported");
      return Type::error();
    }
    return Type::array(element);
  }
  case TypeRef::Kind::Union: {
    bool withNull = false;
    std::vector<const Type *> members;
    for (const TypeRef &member : ref.args) {
      // `Json | null` is a Json, which holds null already.
      if (member.kind == TypeRef::Kind::Name && member.name == "null") {
        withNull = true;
        continue;
      }
      const Type *type = resolveTypeInner(member);
      if (type->isError())
        return type;
      if (type->isVoid()) {
        diags_.error(ref.loc, "'void' cannot be part of a union");
        return Type::error();
      }
      members.push_back(type);
    }
    const Type *type = Type::unionOf(members);
    if (withNull && !type->nonOptional()->isJson()) {
      diags_.error(ref.loc, "'null' is not supported: the only value for nothing is 'undefined'; "
                            "write 'T | undefined' (null exists only in Json values)");
      return Type::error();
    }
    if (std::string problem = unionProblem(type); !problem.empty()) {
      diags_.error(ref.loc, "union '" + type->name() + "' is not supported: " + problem);
      return Type::error();
    }
    return type;
  }
  case TypeRef::Kind::Literal: {
    if (ref.name == "string")
      return Type::stringLiteral(ref.text);
    if (ref.name == "boolean")
      return Type::boolLiteral(ref.number != 0);
    // A whole number that fits is an int, as the literal `1` is.
    bool whole = ref.number == std::trunc(ref.number) && ref.number >= -2147483648.0 &&
                 ref.number <= 2147483647.0 && !(ref.number == 0 && std::signbit(ref.number));
    return Type::numberLiteral(ref.number, whole ? Type::intTy() : Type::doubleTy());
  }
  case TypeRef::Kind::Predicate:
    diags_.error(ref.loc, "a type predicate, '" + ref.name +
                              " is T', can only be the return type of a function");
    return Type::error();
  case TypeRef::Kind::KeyOf:
  case TypeRef::Kind::Indexed:
  case TypeRef::Kind::TypeOf:
  case TypeRef::Kind::Mapped:
    return resolveTypeOp(ref);
  case TypeRef::Kind::Conditional:
    return resolveConditional(ref);
  case TypeRef::Kind::Template:
    return resolveTemplate(ref);
  case TypeRef::Kind::Intersection:
    return resolveIntersection(ref);
  case TypeRef::Kind::Infer: {
    // The type parameter resolveConditional declared for it.
    auto *binding = llvm::dyn_cast_or_null<TypeAliasDecl>(lookupType(ref.name));
    if (!extendsClause_ || !binding || !binding->resolved) {
      diags_.error(ref.loc, "'infer' declares a type only in the 'extends' clause of a conditional "
                            "type, e.g. 'T extends Array<infer U> ? U : never'");
      return Type::error();
    }
    return binding->resolved;
  }
  case TypeRef::Kind::Function: {
    // `(a: int, b?: int, ...rest: int[]) => T`
    std::vector<const Type *> params;
    size_t minArgs = 0;
    bool optional = false, ok = true;
    size_t count = ref.args.size() - 1;
    // `(this: unknown, ...args: A) => R` (see Type::hasThis).
    bool hasThis = count > 0 && !ref.names.empty() && ref.names[0] == "this" && !ref.optional[0] &&
                   !(ref.rest && count == 1);
    for (size_t i = 0; i < count; ++i) {
      bool rest = ref.rest && i + 1 == count;
      bool outerList = std::exchange(listContext_, rest);
      const Type *type = resolveType(ref.args[i], /*allowVoid=*/false);
      listContext_ = outerList;
      ok &= !type->isError();
      if (rest) {
        // Or a list of the types of parameters: `(...args: A) => R`.
        if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
          diags_.error(ref.args[i].loc, "a rest parameter must be an array, e.g. '...xs: int[]'");
          ok = false;
        }
      } else if (ref.optional[i]) {
        type = Type::optional(type);
        optional = true;
      } else {
        if (optional) {
          diags_.error(ref.args[i].loc, "a required parameter cannot follow an optional one");
          ok = false;
        }
        minArgs = i + 1;
      }
      params.push_back(type);
    }
    // A predicate in a function type is what the function returns.
    const TypeRef &returned = ref.args.back();
    const Type *ret = returned.kind != TypeRef::Kind::Predicate
                          ? resolveType(returned, /*allowVoid=*/true)
                      : returned.asserts ? Type::voidTy()
                                         : Type::boolTy();
    if (!ok || ret->isError())
      return Type::error();
    return Type::function(params, minArgs, ref.rest, ret, hasThis);
  }
  case TypeRef::Kind::Object:
    return Type::object(resolveFields(ref));
  case TypeRef::Kind::Tuple: {
    std::vector<const Type *> elements;
    for (const TypeRef &element : ref.args)
      elements.push_back(resolveType(element, /*allowVoid=*/false));
    // `[int, ...A]`: lists of the types of parameters spread into it
    // (docs/generics.md, section 9).
    if (llvm::is_contained(ref.spread, true)) {
      // `[...Tuple<A>, ...Tuple<B>]`: what a computation that waits gives is
      // spread once it is known.
      bool waiting = false;
      for (size_t i = 0; i < elements.size(); ++i)
        waiting |=
            ref.spread[i] && elements[i]->typeOp() != TypeOp::None && !elements[i]->isListParam();
      if (waiting && llvm::none_of(elements, [](const Type *t) { return t->isError(); })) {
        std::vector<const Type *> operands;
        for (size_t i = 0; i < elements.size(); ++i) {
          operands.push_back(Type::boolLiteral(ref.spread[i]));
          operands.push_back(elements[i]);
        }
        return evalTypeOp(Type::typeOp(TypeOp::Spread, operands), operands);
      }
      std::string why;
      const Type *list = spreadTuple(elements, ref.spread, why);
      if (!list) {
        diags_.error(ref.loc, why);
        return Type::error();
      }
      // A rest element makes a list of parameters, which only the type of a
      // rest parameter is, and a pattern of a conditional type may match.
      if (list->isTuple() && list->tupleRest() && !listContext_ && !extendsClause_) {
        diags_.error(ref.loc, "a rest element in a tuple type is not supported: a tuple has a fixed "
                              "length; use an array, e.g. 'int[]'");
        return Type::error();
      }
      return list;
    }
    for (size_t i = 0; i < elements.size(); ++i)
      if (elements[i]->isListParam()) {
        diags_.error(ref.args[i].loc, "'" + elements[i]->name() + "' is a list of types; spread it "
                                      "into the tuple, '..." + elements[i]->name() + "'");
        return Type::error();
      }
    return Type::tuple(elements, !ref.readonly.empty() && ref.readonly[0]);
  }
  }
  llvm_unreachable("unknown type reference");
}

std::vector<Field> Sema::resolveFields(const TypeRef &ref, std::vector<Overload> *overloads) {
  std::vector<Field> fields;
  auto isMethod = [&](size_t i) { return i < ref.methods.size() && ref.methods[i]; };
  for (size_t i = 0; i < ref.args.size(); ++i) {
    llvm::StringRef name = ref.names[i];
    // A method with several signatures: all of them are methods.
    bool several = false, others = false;
    for (size_t j = 0; j < ref.args.size(); ++j)
      if (j != i && ref.names[j] == name) {
        several = true;
        others |= !isMethod(j);
      }
    if (several && isMethod(i) && !others) {
      if (!overloads) {
        diags_.error(ref.locs[i], "several signatures of method '" + name +
                                      "' are supported only in an interface; declare one, "
                                      "'interface Name { ... }'");
        continue;
      }
      const Type *type = resolveType(ref.args[i], /*allowVoid=*/false);
      auto it = llvm::find_if(*overloads, [&](const Overload &o) { return o.name == name; });
      if (it == overloads->end())
        it = overloads->insert(overloads->end(), Overload{name.str(), {}});
      if (llvm::is_contained(it->signatures, type))
        diags_.error(ref.locs[i], "duplicate signature '" + type->name() + "' of method '" + name +
                                      "': the signatures must differ in their parameters");
      else if (!type->isError())
        it->signatures.push_back(type);
      continue;
    }
    if (llvm::any_of(fields, [&](const Field &f) { return f.name == name; })) {
      diags_.error(ref.locs[i], "duplicate field '" + name + "'");
      continue;
    }
    const Type *type = resolveType(ref.args[i], /*allowVoid=*/false);
    if (ref.optional[i])
      type = Type::optional(type);
    fields.push_back({name.str(), type, ref.readonly[i]});
  }
  return fields;
}

bool Sema::isImplicitlyConvertible(const Type *from, const Type *to) {
  if (from == to)
    return true;
  // A value that cannot exist goes anywhere: `ok(1)` is a `Result<int, never>`,
  // which fits where a `Result<int, string>` is expected.
  if (from->isNever())
    return true;
  // Widening, as in C#. A long beyond 2^53 loses precision as a double, as
  // every such number does in JavaScript.
  if (from == Type::intTy() && (to == Type::longTy() || to == Type::doubleTy()))
    return true;
  if (from == Type::longTy() && to == Type::doubleTy())
    return true;
  // An integer becomes a decimal exactly; a double, inexact already, does not.
  if (from->isInteger() && to->isDecimal())
    return true;
  // Any value is an unknown, and any object of the language an object
  // (docs/decorators.md): the same reference, or a box (see sema_unknown.cpp).
  if (to->isUnknown())
    return !from->isVoid();
  if (to->isAnyObject())
    return isObjectReference(from) || (from->isParam() && from->constraint() &&
                                       isImplicitlyConvertible(from->constraint(), to));
  // A class as a value is a Constructor of what its objects are, and a
  // Constructor of a narrower type one of a wider type (docs/decorators.md,
  // section 5): the same reference to the object of the class.
  if (to->isConstructor() && from->isClassRef()) {
    // Not a generic class: each list of its type arguments makes a class of
    // its own, and `new` of the value could make none of them.
    if (from->classRef() && from->classRef()->isGeneric())
      return false;
    const Type *made = constructedBy(from);
    return made ? isImplicitlyConvertible(made, to->constructed())
                : to->constructed()->isAnyObject();
  }
  // A string, a number and a boolean are Json values, and so is a Json[],
  // which stays the same array (docs/modules.md, section 5).
  if (to->isJson())
    return from->isString() || from->isNumeric() || from->isBool() ||
           from->isLiteral() ||
           (from->isArray() && from->inner()->isJson());
  if (to->isOptional())
    return from->isUndefined() || isImplicitlyConvertible(from->nonOptional(), to->inner());
  // A member goes into its union, and a union into one that has each of its
  // members (docs/objects.md, section 7).
  if (to->isUnion()) {
    if (from->isUnion())
      return llvm::all_of(from->members(), [&](const Type *m) { return unionMemberFor(m, to); });
    return unionMemberFor(from, to) != nullptr;
  }
  // A union goes where each of its members goes, if its value needs no tag:
  // `"up" | "down"` is a string, `Circle | Square` a Shape they implement.
  if (from->isUnion())
    return !from->isTagged() && !hasParamMember(from) &&
           llvm::all_of(from->members(),
                        [&](const Type *m) { return isImplicitlyConvertible(m, to); });
  // A string of a pattern, `` `on${string}` ``, is a string, and a string
  // literal is one of a pattern it matches (docs/generics.md, section 10).
  if (to->isPattern())
    return from->isStringLiteral() && matchesPattern(from->literalText(), to);
  if (from->isPattern())
    return to == Type::string();
  // A literal is a value of its base type: `"circle"` is a string.
  if (from->isLiteral())
    return !to->isLiteral() && isImplicitlyConvertible(from->literalBase(), to);
  // A value of `T extends C` is a C.
  if (from->isParam() && from->constraint())
    return isImplicitlyConvertible(from->constraint(), to);
  // An array of Dog is an array of Animal: the same array. Code that puts
  // another Animal into it is found by the check of the whole program
  // (sema_widen.cpp, docs/objects.md, section 2).
  if (from->isArray() && to->isArray())
    return sameReference(from->inner(), to->inner());
  // An iterator may be seen through a type that drops what its generator
  // returns, or that sends nothing to it; the values it gives are the same.
  if (from->isIterator() && to->isIterator())
    return from->yielded() == to->yielded() &&
           (from->returned() == to->returned() || to->returned()->isVoid()) &&
           (from->sent() == to->sent() || to->sent()->isVoid());
  // A Promise that is never fulfilled, as Promise.reject() gives, is a
  // Promise of anything.
  if (from->isPromise() || to->isPromise())
    return from->isPromise() && to->isPromise() && from->promised()->isNever();
  // A Buffer is a Uint8Array, as in node; the other objects of binary data
  // are only themselves.
  if (from->isBytes() || to->isBytes())
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8;
  // A match of a known regex is a match of some regex: every match without
  // indices has the same layout.
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() ||
      to->isCrypto() || from->isWeakRef() || to->isWeakRef())
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 &&
           !from->field("indices");
  // An object is one of the types up its declared chain: the class it
  // extends, the interfaces it implements (docs/objects.md, section 2). The
  // reference stays as it is.
  if (from->isObject() && to->isObject() && from->isSubtypeOf(to))
    return true;
  // An object may be seen through a type that makes some of its fields
  // read-only, or lets code change those that are; the fields are the same,
  // so nothing changes in memory (see Sema::convert for where they lie).
  // Code that changes a read-only one is found by the check of the whole
  // program (sema_widen.cpp). A class is only itself.
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      from->isTuple() == to->isTuple() && from->fields().size() == to->fields().size() &&
      !from->isAnyObject() && !to->isAnyObject() && from->overloads().empty() &&
      to->overloads().empty()) {
    for (const Field &f : from->fields()) {
      const Field *t = to->field(f.name);
      if (!t || t->type != f.type)
        return false;
    }
    return true;
  }
  // A Result is a value: each half converts, `Result<int, never>` to
  // `Result<double, string>`.
  if (from->isResult() && to->isResult()) {
    auto half = [](const Type *a, const Type *b) {
      return a == b || a->isNever() ||
             (!a->isVoid() && !b->isVoid() && isImplicitlyConvertible(a, b));
    };
    return half(from->okType(), to->okType()) && half(from->errType(), to->errType());
  }
  return false;
}

const Type *Sema::commonNumericType(const Type *a, const Type *b) {
  if (isImplicitlyConvertible(a, b))
    return b;
  if (isImplicitlyConvertible(b, a))
    return a;
  return nullptr;
}

const Type *Sema::unify(const Type *a, const Type *b) {
  if (a == b)
    return a;
  if (a->isError() || b->isError())
    return Type::error();
  if (a->isNever())
    return b;
  if (b->isNever())
    return a;
  if (a->isVoid() || b->isVoid())
    return nullptr;
  if (a->isUndefined())
    return Type::optional(b);
  if (b->isUndefined())
    return Type::optional(a);
  // Anything goes into an unknown.
  if (a->isUnknown() || b->isUnknown())
    return Type::unknown();
  const Type *x = a->nonOptional(), *y = b->nonOptional();
  // A literal and its base, a member and its union: the wider one.
  if (x->isLiteral() || y->isLiteral() || x->isUnion() || y->isUnion()) {
    const Type *wider = isImplicitlyConvertible(x, y)   ? y
                        : isImplicitlyConvertible(y, x) ? x
                                                        : nullptr;
    if (!wider)
      return nullptr;
    return a->isOptional() || b->isOptional() ? Type::optional(wider) : wider;
  }
  // Strings of patterns, and a string: a string.
  if (x->isString() && y->isString())
    return a->isOptional() || b->isOptional() ? Type::optional(Type::string()) : Type::string();
  // An object and the same object with read-only fields give the read-only
  // one.
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool related = (number(x) && number(y)) || (x->isObject() && y->isObject()) ||
                 (x->isIterator() && y->isIterator());
  const Type *base = x == y ? x : related ? commonNumericType(x, y) : nullptr;
  // A Json and a value that is one: `j.port ?? 8080`.
  if (!base && (x->isJson() || y->isJson()))
    base = isImplicitlyConvertible(y, x) ? x : isImplicitlyConvertible(x, y) ? y : nullptr;
  // A Dog and a Cat are both Animals.
  if (!base && x->isClass() && y->isClass())
    base = commonClass(x, y);
  // An array of Dog and an array of Animal: the wider, which the check of
  // writes through it follows (sema_widen.cpp).
  if (!base && x->isArray() && y->isArray())
    base = isImplicitlyConvertible(x, y) ? y : isImplicitlyConvertible(y, x) ? x : nullptr;
  if (!base && x->isPromise() && y->isPromise())
    base = x->promised()->isNever() ? y : y->promised()->isNever() ? x : nullptr;
  // Two classes as values make a Constructor of what both make: `[Dog,
  // Cat]` is a `Constructor<Animal>[]`.
  if (!base && x->isClassRef() && y->isClassRef()) {
    const Type *p = constructedBy(x), *q = constructedBy(y);
    const Type *made = !p || !q                        ? nullptr
                       : isImplicitlyConvertible(p, q) ? q
                       : isImplicitlyConvertible(q, p) ? p
                                                       : commonClass(p, q);
    base = Type::constructorOf(made ? made : Type::anyObject());
  }
  // A class as a value and an object are objects.
  if (!base && (x->isAnyObject() || y->isAnyObject()) && isObjectReference(x) &&
      isObjectReference(y))
    base = Type::anyObject();
  // `ok(1)` and `err("empty")` make a `Result<int, string>`.
  if (x->isResult() && y->isResult() && x != y) {
    const Type *value = unifyHalf(x->okType(), y->okType());
    const Type *error = unifyHalf(x->errType(), y->errType());
    base = value && error ? Type::result(value, error) : nullptr;
  }
  if (!base)
    return nullptr;
  return a->isOptional() || b->isOptional() ? Type::optional(base) : base;
}

Expr *Sema::convert(Expr *expr, const Type *to) {
  if (expr->type == to || expr->type->isError() || to->isError())
    return expr;
  // What reflection may reach (see sema_reflect.cpp).
  if (const Type *target = to->nonOptional();
      target->isConstructor() || target->isAnyObject() || target->isUnknown() ||
      (target->isArray() && target->inner()->isUnknown()))
    noteReflectedConversion(expr->type, to);
  // Generic code converts a value of type T here; instances will too.
  if (expr->type->isParam() && !mentionsParam(to))
    noteForced(expr, to);
  // An integer literal becomes a literal of the wider type, so that `-0` in
  // `[1.5, -0]` stays a negative zero (and a decimal one is a constant).
  if ((to->isNumeric() || to->isDecimal()) && expr->type->isInteger() &&
      isImplicitlyConvertible(expr->type, to)) {
    auto *unary = dyn_cast<UnaryExpr>(expr);
    auto *lit = dyn_cast<IntLitExpr>(unary && unary->op == UnaryOp::Neg ? unary->operand : expr);
    if (lit) {
      lit->type = to;
      if (unary)
        unary->type = to;
      return expr;
    }
  }
  // An object seen through a type with read-only fields: CodeGen reads them
  // as through an interface that the object's type implements, since the
  // types may lie out differently (see SourceFile::views). A Buffer is a
  // Uint8Array as it is.
  const Type *from = expr->type->nonOptional(), *target = to->nonOptional();
  // So are the elements of an array seen as an array of such objects.
  while (from->isArray() && target->isArray()) {
    from = from->inner()->nonOptional();
    target = target->inner()->nonOptional();
  }
  if (from->isObject() && target->isObject() && from != target && !from->isClass() &&
      !target->isClass() && !from->isAnyObject() && !target->isAnyObject() && !from->isBytes() &&
      !from->isSubtypeOf(target) &&
      !llvm::is_contained(file_->views, std::pair<const Type *, const Type *>(from, target)))
    file_->views.push_back({from, target});
  auto *cast = ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  // A conversion that lets code write what the value's type does not, as
  // `Dog[]` to `Animal[]`: checked against the writes of the program once it
  // is all checked (sema_widen.cpp).
  if (widensWrites(expr->type, to))
    widenings_.insert(cast);
  return cast;
}

// Why an int's `x >>> n` may not go back into an int (see checkBinary).
static const char kUnsignedShift[] =
    "'>>>' is unsigned in JavaScript, and with a count of 0 a negative int becomes a number above "
    "int: keep the result in a long";

Expr *Sema::coerce(Expr *expr, const Type *to, llvm::StringRef what) {
  const Type *from = expr->type;
  if (from == to || from->isError() || to->isError())
    return expr;
  if (isImplicitlyConvertible(from, to))
    return convert(expr, to);
  // Generic code may give a value of either branch where a conditional type
  // that waits is expected; each copy checks the branch it chooses.
  if (fitsConditional(from, to))
    return convert(expr, to);
  // An iterator written by hand (an object with next()), or an object with
  // `[Symbol.iterator]()`, where an iterator is expected.
  if (to->nonOptional()->isIterator() && from->isObject()) {
    Expr *iterator = iteratorOf(expr);
    if (!iterator)
      iterator = userIterator(expr);
    if (iterator && iterator->type->isIterator() && isImplicitlyConvertible(iterator->type, to))
      return convert(iterator, to);
  }
  // An object with `[Symbol.asyncIterator]()` where an async iterator is
  // expected, as an `AsyncIterable<T>` parameter.
  if (to->nonOptional()->isAsyncIterator() && from->isObject() && !from->isAsyncIterator())
    if (Expr *iterator = asyncUserIterator(expr);
        iterator && iterator->type->isAsyncIterator() && isImplicitlyConvertible(iterator->type, to))
      return convert(iterator, to);
  // A function named here can be given a type that takes more or fewer
  // arguments.
  const Type *function = to->isOptional() && to->inner()->isFunction() ? to->inner() : to;
  if (from->isFunction() && function->isFunction()) {
    if (Expr *adapted = adaptFunction(expr, function))
      return function == to ? adapted : convert(adapted, to);
    if (Expr *adapted = adaptResult(expr, function))
      return function == to ? adapted : convert(adapted, to);
  }

  std::string message =
      ("type '" + from->name() + "' is not assignable to type '" + to->name() + "'").str();
  if (!what.empty())
    message += (" of " + what).str();
  // Objects, or arrays of them: the fields that differ.
  const Type *fromObject = from->nonOptional(), *toObject = to->nonOptional();
  while (fromObject->isArray() && toObject->isArray()) {
    fromObject = fromObject->inner()->nonOptional();
    toObject = toObject->inner()->nonOptional();
  }
  if (from->isFunction() && function->isFunction())
    message += "; the types of functions must match exactly: wrap it in an arrow that converts "
               "what it needs";
  // A literal that goes to places whose types disagree (see
  // literalDestination).
  const std::vector<std::pair<const Type *, llvm::SMLoc>> *places = nullptr;
  const Type *otherPlace = nullptr;
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var)
    if (auto found = destinations_.find(name->var); found != destinations_.end()) {
      places = &found->second;
      for (auto [type, loc] : *places)
        if (type->nonOptional() != toObject && !otherPlace)
          otherPlace = type->nonOptional();
    }
  const ClassDecl *fromClass = fromObject->classDecl();
  if (from->classRef() && from->classRef()->isGeneric() && toObject->isConstructor()) {
    // An emitter of the library extends with its map inferred.
    const ClassDecl *generic = from->classRef();
    std::string name = generic->name.str();
    std::string example = generic->library ? "class Bus extends " + name + " {}"
                                           : "class Int" + name + " extends " + name + "<int> {}";
    message += "; each list of type arguments of '" + name +
               "' makes a class of its own, and 'new' of this value could make none of them: "
               "give a class that extends it, e.g. '" + example +
               "', or take the class as an 'object'";
  }
  else if (from->isUnknown() || from->isAnyObject())
    message += unknownHint(expr, to);
  else if (fromObject->isObject() && toObject->isClass() && (!fromClass || fromClass->literal))
    message += "; an object literal is never an instance of a class: create one with 'new " +
               toObject->name().str() + "(...)'";
  else if (fromClass && fromClass->literal && toObject->isObject())
    message += "; an object literal with methods has a type of its own; to give it this type, "
               "write its functions as fields, 'f: () => ...'";
  else if (fromObject->isClass() && toObject->isObject() && fromObject != toObject)
    message += toObject->isClass()
                   ? "; classes are told apart by their names, whatever their fields, and '" +
                         fromObject->name().str() + "' does not extend '" +
                         toObject->name().str() + "'"
                   : "; an instance of a class is only of its class's type and of those it "
                     "extends or implements: write 'implements " + toObject->name().str() +
                         "' in the class";
  else if (fromObject->isObject() && !fromObject->isClass() && toObject->isUnion() &&
           llvm::any_of(toObject->members(), [](const Type *m) { return m->isObject(); })) {
    // A literal goes into a union as one of its members.
    message += "; an object literal goes into a union as one of its members: it needs the "
               "fields of one of them";
    const Type *front = toObject->members().front();
    for (const Field &f : front->isObject() ? front->fields() : llvm::ArrayRef<Field>())
      if (llvm::all_of(toObject->members(), [&](const Type *m) {
            return m->isObject() && m->field(f.name) && m->field(f.name)->type->isLiteral();
          })) {
        message += ", with its value of '" + f.name + "'";
        break;
      }
  } else if (fromObject->isTuple() && toObject->isArray())
    message +=
        "; a tuple is not an array, though it is written like one: copy it into an array with "
        "'[...t]'";
  else if (fromObject->isObject() && toObject->isObject() &&
           !isImplicitlyConvertible(fromObject, toObject))
    message += objectMismatch(fromObject, toObject, /*conflict=*/otherPlace != nullptr) +
               (otherPlace ? "; the same object also goes where '" + otherPlace->name().str() +
                                 "' is expected, and one object cannot be both"
                           : "");
  else if (from->isOptional() && isImplicitlyConvertible(from->inner(), to))
    message += "; the value may be undefined: check it first (e.g. 'x != undefined') or give a "
               "default with '??'";
  else if (from->nonOptional()->isJson())
    message += "; a Json may hold any JSON value: check what it holds first (e.g. 'typeof x == "
               "\"string\"' or 'Array.isArray(x)'), or convert it with 'fromJson<" +
               to->nonOptional()->name().str() + ">(x)'";
  else if (to->nonOptional()->isAnyObject())
    message += "; only objects of the language are 'object's (instances of classes, object "
               "literals, dates, dictionaries...); for any value at all there is 'unknown'";
  else if (to->nonOptional()->isJson() &&
           (fromObject->isObject() || from->nonOptional()->isArray()))
    message += "; an array or an object becomes a Json only as a literal written where a Json is "
               "expected ('[...]', '{ ... }'), or as a 'Json[]', which stays the same array";
  else if (to->nonOptional()->isJson() && from->nonOptional()->isDecimal())
    message += "; JSON has no decimals: write it as text with 'String(x)', as JSON.stringify does";
  else if (from->isFloat() && to->isDecimal())
    message += "; a double is inexact already, so it becomes a decimal only explicitly: "
               "'decimal(x)'";
  else if (from->isDecimal() && (to->isNumeric() || to->nonOptional()->isNumeric()))
    message += "; a decimal becomes a number only explicitly: 'Number(x)' gives the nearest double";
  else if (from->isFloat() && to->isInt())
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, "
               "Math.ceil or Math.trunc";
  else if (from->isFloat() && to->isInteger())
    message += "; round it with Math.round, Math.floor, Math.ceil or Math.trunc";
  else if (auto *bin = dyn_cast<BinaryExpr>(expr);
           bin && bin->op == BinaryOp::UShr && bin->lhs->type->isInt() && to->isInt())
    message += "; " + std::string(kUnsignedShift) +
               (bin->source.empty() ? "" : ", e.g. 'const u = " + bin->source.str() + "'");
  else if (from->isInteger() && to->isInteger())
    message += "; Math.trunc(x) converts it and checks that it fits";
  diags_.error(expr->loc(), message);
  if (otherPlace)
    for (auto [type, loc] : *places)
      if (type->nonOptional() == otherPlace) {
        diags_.note(loc, "the object also goes here, as '" + otherPlace->name() + "'");
        break;
      }
  return expr;
}

void Sema::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined())
    message += "; a value may be undefined: check it first or give a default with '??'";
  diags_.error(loc, message);
}

const Type *Sema::comparisonType(const Type *a, const Type *b) {
  const Type *common = a->isVoid() || b->isVoid() ? nullptr : unify(a, b);
  if (common && common->isError())
    return common;
  const Type *base = common ? common->nonOptional() : nullptr;
  if (base && (base->isNumeric() || base->isDecimal() || base->isBool() || base->isString() ||
               base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
               base->isJson() || base->isDict() || base->isUnknown() || base->isClassRef() ||
               base->isLiteral() || (base->isUnion() && !hasParamMember(base))))
    return common;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Declarations and statements
//===----------------------------------------------------------------------===//

// The type of a parameter without an annotation, from a default value that
// is a literal, as `let x = 5` would give it; null for other values.
const Type *literalType(const Expr *value) {
  if (auto *unary = dyn_cast<UnaryExpr>(value); unary && unary->op == UnaryOp::Neg)
    value = unary->operand;
  if (auto *lit = dyn_cast<IntLitExpr>(value))
    return lit->value < (uint64_t(1) << 31)   ? Type::intTy()
           : lit->value < (uint64_t(1) << 63) ? Type::longTy()
                                               : Type::doubleTy();
  if (isa<FloatLitExpr>(value))
    return Type::doubleTy();
  if (isa<StringLitExpr>(value) || isa<TemplateExpr>(value))
    return Type::string();
  if (isa<BoolLitExpr>(value))
    return Type::boolTy();
  return nullptr;
}

void Sema::declareFunction(FunctionDecl *fn, VarSymbol *var, bool scoped) {
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  // A generic function: its type parameters are visible in its signature;
  // so are those of the generic class of a method (not of a static one,
  // which every instance of the class shares).
  // A parameter `xs: Iterable<int>` makes it generic too (see iterableParam).
  bool generic = !fn->typeParams.empty() ||
                 llvm::any_of(fn->params, [&](const Param &p) { return p.type && isIterableRef(*p.type); });
  bool classParams = fn->cls && !fn->isStatic && !fn->cls->typeParams.empty();
  if (generic || classParams) {
    typeScopes_.emplace_back();
    if (classParams)
      bindClassTypeParams(fn->cls);
  }
  if (generic) {
    if (var) {
      // A parameter `Iterable<T>` alone is reported with it (see iterableParam).
      if (!fn->typeParams.empty())
        diags_.error(fn->loc(), "a generic function can be declared only at the top level for now");
      fn->typeArgs.assign(fn->typeParams.size(), Type::error());
      bindTypeParams(fn->typeParams, fn->typeArgs);
    } else {
      fn->typeArgs = sym->typeParams = declareTypeParams(fn->typeParams, &sym->typeDefaults);
    }
  }
  bool afterOptional = false;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    Param &param = fn->params[i];
    // Only the parameters of the methods and constructors of classes have
    // decorators (docs/decorators.md, section 1).
    if (!param.decorators.empty() && (!fn->cls || fn->cls->literal) && !fn->isInstance)
      diags_.error(param.decorators[0].loc, "a decorator of a parameter can decorate only a "
                                            "parameter of a method or a constructor of a class");
    const Type *type = nullptr;
    if (param.type) {
      type = iterableParam(fn, sym, *param.type);
      bool outerList = std::exchange(listContext_, param.rest);
      if (!type)
        type = resolveType(*param.type, /*allowVoid=*/false);
      listContext_ = outerList;
    } else if (param.defaultValue && (type = literalType(param.defaultValue))) {
      // `function f(n = 10)`: n is an int.
    } else if (param.defaultValue && !param.rest && !fn->source.empty()) {
      // `function f(n = limit * 2)`: the type of the value, found when
      // needed (see resolveParams).
      param.declaredType = nullptr;
      sym->params.push_back(nullptr);
      sym->pendingParams = true;
      continue;
    } else {
      diags_.error(param.loc, "parameter '" + param.label() + "' needs a type annotation");
      type = Type::error();
    }
    if (param.optional)
      type = Type::optional(type);
    param.declaredType = type;
    // `function (this: unknown, ...args: A)`: the `this` the function gets,
    // which `f.call(obj, ...)` passes first (docs/decorators.md, section 2).
    if (i == 0 && param.name == "this" && !param.rest && param.type) {
      if (fn->isArrow)
        diags_.error(param.loc, "an arrow function cannot have a 'this' parameter: it sees the "
                                "'this' of the function around it; write a 'function'");
      else if (fn->cls)
        diags_.error(param.loc, "a method cannot have a 'this' parameter: its 'this' is its object");
      else
        sym->hasThis = !param.optional && !param.defaultValue;
    }
    if (param.rest) {
      // The arguments from here on, as an array (empty without any); or a
      // list of the types of parameters, `...args: A`.
      if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
        diags_.error(param.loc, "a rest parameter must be an array, e.g. '..." + param.name +
                                    ": int[]'");
        param.declaredType = Type::error();
      }
      sym->hasRest = true;
      sym->params.push_back(param.declaredType);
      continue;
    }
    // A caller may leave out an optional parameter or one with a default
    // value, or pass undefined: then the default applies.
    bool mayOmit = param.optional || param.defaultValue;
    sym->params.push_back(mayOmit ? Type::optional(type) : type);
    if (!mayOmit) {
      if (afterOptional)
        diags_.error(param.loc, "a required parameter cannot follow an optional one");
      sym->minArgs = i + 1;
    }
    afterOptional |= param.optional;
  }
  // Without an annotation: void if no `return` gives a value, else what the
  // body returns, found when needed (see returnType). A generator returns an
  // iterator, whose types its body gives.
  if (fn->returnType)
    sym->ret = asyncReturnType(fn, generatorReturnType(fn, resolveReturnType(fn, sym)));
  else if (!returnsValue(fn->body) && fn->isAsync && !fn->isGenerator)
    sym->ret = Type::promise(Type::voidTy());
  else if (!returnsValue(fn->body) && !fn->isGenerator)
    sym->ret = Type::voidTy();
  if (generic || classParams)
    typeScopes_.pop_back();
  // `...args: [int, int]`: those parameters (see expandArgumentList).
  expandArgumentList(fn, sym);
  fn->sym = sym;
  if (!fn->name.empty() && scoped)
    declare(fn->name, fn->loc(), {var, sym});
}

void Sema::checkFunctionBody(FunctionDecl *fn) {
  if (fn->external) {
    checkExternal(fn);
    return;
  }
  FuncSymbol *sym = fn->sym;
  resolveParams(sym);
  fn->checked = true;
  FuncSymbol *outer = currentFunction_;
  currentFunction_ = sym;
  functions_.push_back(fn);
  unsigned errors = diags_.errorCount();
  // A method may use the private members of its class; a closure those of
  // the class around it.
  ClassDecl *outerClass = currentClass_;
  if (!fn->isClosure())
    currentClass_ = fn->cls;
  FunctionDecl *outerInit = initFn_;
  const VarSymbol *outerThis = initThis_;
  bool outerReady = closureThisReady_;
  pushScope();
  // The type parameters of a generic function, or the types an instance has
  // for them; and those of the class of a method.
  if (fn->cls && !fn->isStatic)
    bindClassTypeParams(fn->cls);
  // A class that a class expression in a copy of a generic function made
  // sees the types of the copy.
  if (fn->cls)
    bindOuterTypeParams(fn->cls);
  if (!fn->typeArgs.empty())
    bindTypeParams(fn->typeParams, fn->typeArgs);
  if (fn->takesThis()) {
    VarSymbol *self = declareThis(fn);
    if (fn->isConstructor || fn == fn->cls->init) {
      // The fields set when it starts: the optional ones, which hold
      // undefined, and in a constructor those with initializers. In a class
      // that extends another, the initializers run after `super(...)`, and
      // the fields declared up the chain are set by then.
      initFn_ = fn;
      initThis_ = self;
      closureThisReady_ = true;
      flow_.setFields.clear();
      bool derived = fn->cls->parent != nullptr;
      for (const ClassField &f : fn->cls->fields)
        if (!f.isStatic && (f.optional || (fn->isConstructor && !derived && f.init && !f.method) ||
                            (!fn->isConstructor && f.redeclared)))
          flow_.setFields.push_back(f.name);
      flow_.beforeSuper = derived && fn->isConstructor;
      if (flow_.beforeSuper) {
        // `super(...)` must be a statement of the body itself.
        unsigned supers = 0;
        for (Stmt *stmt : fn->body->body) {
          auto *expr = dyn_cast<ExprStmt>(stmt);
          auto *call = expr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
          auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
          if (callee && callee->name == "super") {
            rootSupers_.insert(call);
            ++supers;
          }
        }
        if (supers == 0)
          diags_.error(fn->loc(), "the constructor of '" + fn->cls->name +
                                      "' must call 'super(...)': '" + fn->cls->name +
                                      "' extends '" + fn->cls->parent->name +
                                      "', whose constructor sets the fields it declares");
      }
    }
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    // A default value is computed at each call that leaves the parameter
    // out, where it sees the parameters before it.
    Param &param = fn->params[i];
    const Type *type = param.declaredType;
    if (param.defaultValue && type->isError() && !param.type) {
      // Its type comes from it, and checking it failed (see resolveParams).
      param.defaultValue->type = type;
      param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
      continue;
    }
    if (param.defaultValue) {
      llvm::StringSet<> names;
      collectNames(param.defaultValue, names);
      bool forward = false;
      for (size_t j = i; j < fn->params.size() && !forward; ++j)
        if (names.contains(fn->params[j].name)) {
          diags_.error(param.defaultValue->loc(),
                       "the default value of '" + param.label() + "' uses '" + fn->params[j].name +
                           "', which is not set yet: a default value can "
                           "use only the parameters before it");
          forward = true;
        }
      if (forward) {
        param.defaultValue->type = Type::error();
        param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
        continue;
      }
      if (auto *fe = dyn_cast<FunctionExpr>(param.defaultValue); fe && fe->fn->name.empty())
        fe->fn->name = param.name; // as JavaScript names it
      param.defaultValue = checkExpr(param.defaultValue, type->isError() ? nullptr : type);
      param.defaultValue = coerce(param.defaultValue, type);
    }
    param.sym = declareVar(param.name, param.loc, type, /*isConst=*/false);
    if (param.defaultValue && type->isOptional() && !param.defaultValue->type->isOptional() &&
        !param.defaultValue->type->isUndefined())
      setDefined(param.sym, true);
  }
  // The callback of a method of a typed array knows what its element holds.
  addByteCallbackFacts(fn);
  // `args` of `...args: [int, int]`, whose parameters are those.
  declareArgumentList(fn);
  // The parameters and the outermost block of the body share one scope. A
  // function that returns a boolean summarizes what its returns show.
  beginPredicate(fn);
  checkStatements(fn->body);
  if (!flow_.unreachable && sym->asserts)
    notePredicateExit(flow_, Flow(), fn->body->loc());
  endPredicate(fn);
  if (fn->isConstructor && initFn_ == fn && !flow_.beforeSuper)
    checkFieldsSet(fn->loc());
  popScope();
  functions_.pop_back();
  currentFunction_ = outer;
  currentClass_ = outerClass;
  initFn_ = outerInit;
  initThis_ = outerThis;
  closureThisReady_ = outerReady;

  // As in TypeScript, a function that may return undefined may also just end.
  // A generator that may finish without `return` returns undefined.
  const Type *ret = fn->isAsync && sym->ret && sym->ret->isPromise() ? sym->ret->promised()
                    : !fn->isGenerator                               ? sym->ret
                    : sym->ret && (sym->ret->isIterator() || sym->ret->isAsyncIterator())
                        ? sym->ret->returned()
                        : nullptr;
  if (ret && !ret->isVoid() && !ret->isError() && !ret->isOptional() && !ret->isUnknown() &&
      !alwaysReturns(fn->body))
    diags_.error(fn->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                ret->name() + "' on every path");
  if (sym->isGeneric() && diags_.errorCount() > errors)
    brokenGenerics_.insert(sym);
  if (fn->cls && fn->cls->isGeneric() && diags_.errorCount() > errors)
    brokenClasses_.insert(fn->cls);
}

void Sema::checkStmt(Stmt *stmt) {
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    pushScope();
    checkStatements(cast<BlockStmt>(stmt));
    popScope();
    break;
  case Stmt::Kind::VarDecl:
    checkVarDecl(cast<VarDeclStmt>(stmt));
    break;
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
    break; // declared with the block (declareTypes)
  case Stmt::Kind::Class:
    checkClassStmt(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Function:
    // check() handles top-level functions, and checkStatements those of a
    // block.
    if (!cast<FunctionDecl>(stmt)->group)
      diags_.error(stmt->loc(), "a function can be declared only directly in a block; in a "
                                "'switch', declare it before the 'switch'");
    break;
  case Stmt::Kind::Expr: {
    auto *s = cast<ExprStmt>(stmt);
    // `yield x;` drops what next() sends.
    if (auto *yield = dyn_cast<YieldExpr>(s->expr))
      yield->discarded = true;
    s->expr = checkExpr(s->expr);
    checkDiscarded(s->expr, /*callback=*/false, s->loc());
    // `assertIsCircle(s);` narrows from here on, as `asserts s is Circle`
    // says, and as its body shows.
    if (auto *call = dyn_cast<CallExpr>(s->expr)) {
      const FuncSymbol *callee = call->func ? call->func : call->method;
      if (callee && callee->asserts) {
        Flow ignored;
        applyPredicate(call, flow_, ignored);
      }
    }
    // Code after `process.exit()` never runs, as after `return`: in
    // `if (!file) process.exit(1)` the rest knows that file is there.
    if (isExit(s))
      flow_.unreachable = true;
    break;
  }
  case Stmt::Kind::If:
    checkIf(cast<IfStmt>(stmt));
    break;
  case Stmt::Kind::While:
    checkWhile(cast<WhileStmt>(stmt));
    break;
  case Stmt::Kind::DoWhile:
    checkDoWhile(cast<DoWhileStmt>(stmt));
    break;
  case Stmt::Kind::For:
    checkFor(cast<ForStmt>(stmt));
    break;
  case Stmt::Kind::ForOf:
    checkForOf(cast<ForOfStmt>(stmt));
    break;
  case Stmt::Kind::Switch:
    checkSwitch(cast<SwitchStmt>(stmt));
    break;
  case Stmt::Kind::Labeled:
    checkLabeled(cast<LabeledStmt>(stmt));
    break;
  case Stmt::Kind::Return:
    checkReturn(cast<ReturnStmt>(stmt));
    flow_.unreachable = true;
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
    checkJump(stmt);
    flow_.unreachable = true;
    break;
  case Stmt::Kind::Throw:
    checkThrow(cast<ThrowStmt>(stmt));
    flow_.unreachable = true;
    break;
  case Stmt::Kind::Try:
    checkTry(cast<TryStmt>(stmt));
    break;
  case Stmt::Kind::Empty:
  case Stmt::Kind::Debugger:
    break;
  }
}

void Sema::pushTarget(JumpTarget::Kind kind) {
  JumpTarget target{kind, std::move(pendingLabels_), Flow()};
  pendingLabels_.clear();
  target.breaks.unreachable = true;
  targets_.push_back(std::move(target));
}

void Sema::checkJump(Stmt *stmt) {
  bool isBreak = isa<BreakStmt>(stmt);
  llvm::StringRef label = isBreak ? cast<BreakStmt>(stmt)->label : cast<ContinueStmt>(stmt)->label;
  const char *keyword = isBreak ? "break" : "continue";
  JumpTarget *target = nullptr;
  for (JumpTarget &t : llvm::reverse(targets_)) {
    bool matches = label.empty() ? (isBreak ? t.kind != JumpTarget::Kind::Block
                                            : t.kind == JumpTarget::Kind::Loop)
                                 : llvm::is_contained(t.labels, label);
    if (matches) {
      target = &t;
      break;
    }
  }
  if (!target) {
    if (!label.empty())
      diags_.error(stmt->loc(), "cannot find label '" + label + "'");
    else if (isBreak)
      diags_.error(stmt->loc(), "'break' can only be used inside a loop or a 'switch'");
    else
      diags_.error(stmt->loc(), "'continue' can only be used inside a loop");
    return;
  }
  if (!isBreak && target->kind != JumpTarget::Kind::Loop) {
    diags_.error(stmt->loc(), llvm::Twine("'") + keyword + " " + label +
                                  "' needs a loop, and '" + label + "' does not label one");
    return;
  }
  if (isBreak && target->kind != JumpTarget::Kind::Loop)
    target->breaks = join(target->breaks, flow_);
}

static bool isLoop(const Stmt *stmt) {
  return isa<WhileStmt>(stmt) || isa<DoWhileStmt>(stmt) || isa<ForStmt>(stmt) || isa<ForOfStmt>(stmt);
}

void Sema::checkLabeled(LabeledStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels;
  Stmt *body = stmt;
  while (auto *labeled = dyn_cast<LabeledStmt>(body)) {
    bool duplicate = llvm::is_contained(labels, labeled->label) ||
                     llvm::any_of(targets_, [&](const JumpTarget &t) {
                       return llvm::is_contained(t.labels, labeled->label);
                     });
    if (duplicate)
      diags_.error(labeled->loc(), "duplicate label '" + labeled->label + "'");
    labels.push_back(labeled->label);
    body = labeled->body;
  }
  if (isLoop(body) || isa<SwitchStmt>(body)) {
    pendingLabels_ = labels; // for the loop or switch to take
    checkStmt(body);
    return;
  }
  if (isa<VarDeclStmt>(body) || isa<FunctionDecl>(body) || isa<TypeAliasDecl>(body) ||
      isa<EnumDecl>(body)) {
    diags_.error(body->loc(), "a declaration cannot have a label");
    return;
  }
  // `label: { ... break label; ... }`
  pendingLabels_ = labels;
  pushTarget(JumpTarget::Kind::Block);
  checkStmt(body);
  Flow breaks = targets_.back().breaks;
  targets_.pop_back();
  flow_ = join(flow_, breaks);
}

// A literal case test: comparing with it cannot change any variable, so what
// the comparison says about the discriminant holds in the case's body.
static bool isConstantTest(const Expr *test) {
  if (auto *unary = dyn_cast<UnaryExpr>(test); unary && unary->op == UnaryOp::Neg)
    test = unary->operand;
  return isa<IntLitExpr>(test) || isa<FloatLitExpr>(test) || isa<StringLitExpr>(test) ||
         isa<BoolLitExpr>(test) || isa<UndefinedExpr>(test);
}

// The tests run in order until one matches; then control enters that case
// (or `default` if none matches) and falls through the cases after it until
// a `break`.
void Sema::checkSwitch(SwitchStmt *stmt) {
  stmt->discriminant = checkExpr(stmt->discriminant);
  const Type *type = stmt->discriminant->type;
  if (type->isVoid()) {
    diags_.error(stmt->discriminant->loc(), "an expression of type 'void' cannot be compared");
    type = Type::error();
  }
  // As in JavaScript, the cases share one scope.
  pushScope();
  std::vector<Stmt *> all;
  for (SwitchCase &c : stmt->cases)
    all.insert(all.end(), c.body.begin(), c.body.end());
  declareTypes(all);

  const Type *base = type->nonOptional();
  // `case null:` of a Json, which matches only null, as `===` does. A case of
  // a literal type or a union takes its literal type: `case "circle":`.
  const Type *hint = base->isNumeric() || base->isDecimal() || base->isJson() ? base
                     : base->isLiteral() || base->isUnion()                    ? type
                                                                               : nullptr;
  Flow tests = flow_; // what holds while the tests run
  std::vector<Flow> matched(stmt->cases.size());
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    if (!c.test)
      continue;
    flow_ = tests;
    c.test = checkExpr(c.test, hint);
    const Type *testType = c.test->type;
    Flow whenTrue = flow_, whenFalse = flow_;
    // As with `==`: a value of a type parameter compares with anything here,
    // and each instance checks the types it has.
    bool generic = type->nonOptional()->isParam() || testType->nonOptional()->isParam();
    if (!generic && !type->isError() && !testType->isError() && !testType->isUndefined()) {
      const Type *common = comparisonType(type, testType);
      if (!common) {
        diags_.error(c.test->loc(), "a case of type '" + testType->name() +
                                        "' cannot be compared with a 'switch' value of type '" +
                                        type->name() + "'" +
                                        (holdsResults(type) || holdsResults(testType)
                                             ? "; two Results are never equal, as they are two "
                                               "objects in JavaScript: switch on what they hold"
                                             : ""));
      } else if (!common->isError()) {
        if (common->nonOptional()->isFunction())
          file_->comparesFunctions = true;
        // A literal discriminant has nothing to learn about, and converting
        // it here would change its type.
        bool constant = isConstantTest(c.test) && !isConstantTest(stmt->discriminant);
        c.test = convert(c.test, common);
        c.compareType = common;
        if (constant) {
          // `discriminant == test`, to learn from it as from a condition.
          BinaryExpr equal(c.loc, BinaryOp::Eq, convert(stmt->discriminant, common), c.test);
          equal.type = Type::boolTy();
          addConditionFacts(&equal, whenTrue, whenFalse);
        }
      }
    } else if (testType->isUndefined() && isa<UndefinedExpr>(c.test)) {
      BinaryExpr equal(c.loc, BinaryOp::Eq, stmt->discriminant, c.test);
      equal.type = Type::boolTy();
      addConditionFacts(&equal, whenTrue, whenFalse);
    }
    matched[i] = whenTrue;
    tests = whenFalse;
  }
  // A switch on a literal type handles each of its values (docs/objects.md,
  // section 7), or a default says that leaving some out is meant.
  if (std::optional<std::vector<const Type *>> left = unhandledCases(stmt)) {
    if (left->empty()) {
      stmt->exhaustive = true;
      tests.unreachable = true;
    } else {
      std::string names;
      for (size_t i = 0; i < left->size(); ++i)
        names += (i == 0 ? "" : i + 1 == left->size() ? " and " : ", ") + (*left)[i]->name().str();
      diags_.error(stmt->discriminant->loc(),
                   "the switch does not handle " + names + "; add 'case " +
                       left->front()->name().str() +
                       ":', or 'default: break' if leaving it out is meant");
    }
  }

  pushTarget(JumpTarget::Kind::Switch);
  // The statements of the cases after one go on after it (see block_).
  size_t position = 0;
  Flow fallthrough;
  fallthrough.unreachable = true;
  bool hasDefault = false;
  for (size_t i = 0; i < stmt->cases.size(); ++i) {
    SwitchCase &c = stmt->cases[i];
    hasDefault |= !c.test;
    flow_ = join(fallthrough, c.test ? matched[i] : tests);
    for (Stmt *s : c.body) {
      block_ = all;
      blockIndex_ = position++;
      checkStmt(s);
    }
    fallthrough = flow_;
    // A variable declared in this case is in scope in the cases after it,
    // but JavaScript reports its use there as an error unless control fell
    // through its declaration. Sema rejects such uses outright.
    for (Stmt *s : c.body)
      if (auto *decl = dyn_cast<VarDeclStmt>(s))
        for (const VarDeclarator &d : decl->decls)
          if (d.sym)
            otherCaseVars_.insert(d.sym);
  }
  Flow exit = fallthrough;
  if (!hasDefault)
    exit = join(exit, tests);
  exit = join(exit, targets_.back().breaks);
  targets_.pop_back();
  for (SwitchCase &c : stmt->cases)
    for (Stmt *s : c.body)
      if (auto *decl = dyn_cast<VarDeclStmt>(s))
        for (const VarDeclarator &d : decl->decls)
          otherCaseVars_.erase(d.sym);
  popScope();
  flow_ = exit;
}

void Sema::checkIf(IfStmt *stmt) {
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  // In an instance, `if (typeof x == "string")` may be decided by the type
  // of x: the branch that cannot run is dropped, unchecked.
  if (std::optional<bool> constant = checkingInstance_ ? constantCondition(stmt->cond) : std::nullopt) {
    auto *lit = ctx_.make<BoolLitExpr>(stmt->cond->loc(), *constant);
    lit->type = Type::boolTy();
    stmt->cond = lit;
    if (*constant) {
      flow_ = whenTrue;
      checkBody(stmt->thenStmt);
      stmt->elseStmt = nullptr;
    } else {
      stmt->thenStmt = ctx_.make<EmptyStmt>(stmt->thenStmt->loc());
      flow_ = whenFalse;
      if (stmt->elseStmt)
        checkBody(stmt->elseStmt);
    }
    return;
  }
  flow_ = whenTrue;
  checkBody(stmt->thenStmt);
  Flow afterThen = flow_;
  flow_ = whenFalse;
  if (stmt->elseStmt)
    checkBody(stmt->elseStmt);
  flow_ = join(afterThen, flow_);
}

// Loops are checked once. Their head state comes from scanning the loop for
// what it changes (loopHead), so what Sema assumes at the start of the body
// holds on every iteration, and everywhere in the loop.
void Sema::checkWhile(WhileStmt *stmt) {
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond});
  Flow head = loopHead(scan, stmt->cond, /*induction=*/true);
  flow_ = head;
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  checkLoopBody(stmt->body);
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
}

void Sema::checkDoWhile(DoWhileStmt *stmt) {
  // No induction: the body runs once before the condition is first checked.
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond});
  Flow head = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = head;
  checkLoopBody(stmt->body);
  flow_ = join(flow_, head); // `continue` jumps to the condition
  Flow whenTrue, whenFalse;
  stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
}

// Whether `stmt`, the body of a loop with `labels`, has a `continue` that
// goes to that loop; `loops` counts the loops around `stmt` inside it.
static bool continuesTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels, unsigned loops = 0) {
  if (!stmt)
    return false;
  auto inside = [&](const Stmt *s) { return continuesTo(s, labels, loops); };
  switch (stmt->kind()) {
  case Stmt::Kind::Continue: {
    llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
    return label.empty() ? loops == 0 : llvm::is_contained(labels, label);
  }
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, inside);
  case Stmt::Kind::If:
    return inside(cast<IfStmt>(stmt)->thenStmt) || inside(cast<IfStmt>(stmt)->elseStmt);
  case Stmt::Kind::While:
    return continuesTo(cast<WhileStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::DoWhile:
    return continuesTo(cast<DoWhileStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::For:
    return continuesTo(cast<ForStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::ForOf:
    return continuesTo(cast<ForOfStmt>(stmt)->body, labels, loops + 1);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                        [&](const SwitchCase &c) { return llvm::any_of(c.body, inside); });
  case Stmt::Kind::Labeled:
    return inside(cast<LabeledStmt>(stmt)->body);
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    return inside(s->block) || inside(s->handler) || inside(s->finalizer);
  }
  default:
    return false;
  }
}

void Sema::checkFor(ForStmt *stmt) {
  llvm::SmallVector<llvm::StringRef, 1> labels = pendingLabels_;
  pushScope(); // variables declared in the loop header
  block_ = {};
  if (stmt->init)
    checkStmt(stmt->init);
  LoopScan scan = scanLoop({stmt->body}, {stmt->cond, stmt->step});
  Flow head = loopHead(scan, stmt->cond, /*induction=*/true);
  flow_ = head;
  Flow whenTrue = head, whenFalse = head;
  whenFalse.unreachable = true;
  if (stmt->cond)
    stmt->cond = checkCondition(stmt->cond, whenTrue, whenFalse);
  flow_ = whenTrue;
  checkLoopBody(stmt->body);
  if (stmt->step) {
    // The step runs where the body ends (`node = node.next` after
    // `node != undefined`), unless a `continue` may jump there from
    // elsewhere; what holds at the head holds there too. If the body never
    // ends, the step never runs, and is checked as the body starts.
    if (continuesTo(stmt->body, labels))
      flow_ = head;
    else if (flow_.unreachable)
      flow_ = whenTrue;
    if (auto *yield = dyn_cast<YieldExpr>(stmt->step))
      yield->discarded = true; // as in a statement
    stmt->step = checkExpr(stmt->step);
    checkDiscarded(stmt->step);
  }
  flow_ = scan.breaks ? join(whenFalse, head) : whenFalse;
  popScope();
}

void Sema::checkForOf(ForOfStmt *stmt) {
  if (stmt->isAwait) {
    checkForAwait(stmt);
    return;
  }
  if (stmt->keys) {
    checkForIn(stmt);
  } else {
    markFused(stmt->iterable);
    stmt->iterable = iterableOf(apparent(checkExpr(stmt->iterable)));
    // `for (const x of args)` of an argument list (see expandArgumentList):
    // its elements, of the type that holds them all. (Any other tuple is not
    // iterated: its elements have types of their own.)
    auto *name = dyn_cast<NameExpr>(stmt->iterable);
    bool list = name && name->var && name->type->isTuple() && !name->type->tupleRest() &&
                llvm::any_of(functions_, [&](const FunctionDecl *fn) {
                  return fn->listName == name->name && name->var->owner == fn;
                });
    if (list) {
      auto *elements = ctx_.make<ArrayLitExpr>(name->loc());
      // The numbers as the widest of them, then a union of the kinds.
      const Type *number = nullptr;
      std::vector<const Type *> kinds;
      for (const Field &f : name->type->fields())
        if (f.type->isNumeric())
          number = number ? commonNumericType(number, f.type) : f.type;
      for (const Field &f : name->type->fields())
        kinds.push_back(f.type->isNumeric() ? number : f.type);
      for (size_t k = 0; k < name->type->fields().size(); ++k)
        elements->elements.push_back({ctx_.make<IndexExpr>(name->loc(), ctx_.make<NameExpr>(name->loc(), name->name),
                                                           ctx_.make<IntLitExpr>(name->loc(), k)),
                                      /*spread=*/false});
      const Type *element = kinds.empty() ? nullptr : Type::unionOf(kinds);
      if (!elements->elements.empty())
        stmt->iterable =
            checkExpr(elements, element && unionProblem(element).empty() ? Type::array(element) : nullptr);
    }
  }
  if (stmt->iterable->type->isMatch()) // the captures
    stmt->iterable = matchItems(stmt->iterable);
  const Type *type = stmt->iterable->type;
  const Type *element = Type::error();
  if (type->isArray()) {
    element = type->inner();
  } else if (type->isString()) {
    element = Type::string();
  } else if (type->isTypedArray()) {
    element = type->elementType();
  } else if (type->isIterator()) {
    element = type->yielded();
    checkSendsNothing(type, stmt->iterable->loc(), "'for...of'");
  } else if (type->isOptional() && iteratedType(type->inner()))
    diags_.error(stmt->iterable->loc(), "'" + type->name() +
                                            "' may be undefined; check it before iterating");
  else if (type->isTuple())
    diags_.error(stmt->iterable->loc(),
                 "cannot iterate over a tuple, whose elements have types of their own; "
                 "destructure it, or copy it into an array: '[...t]'");
  else if (type->isRecord())
    diags_.error(stmt->iterable->loc(),
                 "a Record is not iterable, as an object is not in JavaScript; walk its keys "
                 "with 'for...in', or its entries with 'for (const [k, v] of Object.entries(r))'");
  else if (type->isRuntimeObject() && asyncIteratedType(type))
    diags_.error(stmt->iterable->loc(), "'" + type->name() +
                                            "' gives its values later: iterate it with 'for await "
                                            "(const x of ...)'");
  else if (!type->isError())
    diags_.error(stmt->iterable->loc(), "cannot iterate over a value of type '" + type->name() + "'");
  if (stmt->binding.typeAnn)
    diags_.error(stmt->binding.typeAnn->loc,
                 stmt->keys ? "a 'for...in' variable cannot have a type annotation"
                            : "a 'for...of' variable cannot have a type annotation");
  if (stmt->binding.destructured && !stmt->binding.objectPattern && !element->isArray() &&
      !element->isTuple() && !element->isMatch() && !element->isError()) {
    diags_.error(stmt->binding.loc, "only arrays can be destructured; the elements are '" +
                                        element->name() + "'");
    element = Type::error();
  }

  LoopScan scan = scanLoop({stmt->body}, {});
  // An iterator runs code between the iterations: a generator's body, the
  // callbacks of its helpers.
  if (type->isIterator())
    scan.callsFunctions = true;
  // The array keeps each element alive during its iteration unless the body
  // could remove or replace elements, directly or through a function. Then a
  // const loop variable can borrow the element instead of counting it.
  stmt->borrowElements = stmt->isConst && type->isArray() && element->isManaged() &&
                         !scan.callsFunctions && !scan.releasesElements;
  Flow head = loopHead(scan, nullptr, /*induction=*/false);
  flow_ = head;
  pushScope();
  stmt->binding.sym =
      declareVar(stmt->binding.name, stmt->binding.loc, element, stmt->isConst);
  checkLoopBody(stmt->body);
  popScope();
  flow_ = head;
}

void Sema::checkLoopBody(Stmt *body) {
  pushTarget(JumpTarget::Kind::Loop);
  checkBody(body);
  targets_.pop_back();
}

// The body of `if`, `while` or `for`. As in TypeScript, a declaration there
// needs braces: it would be visible nowhere else.
void Sema::checkBody(Stmt *body) {
  if (isa<VarDeclStmt>(body) || isa<TypeAliasDecl>(body) || isa<EnumDecl>(body) ||
      isa<FunctionDecl>(body)) {
    diags_.error(body->loc(), "a declaration cannot be the whole body here; wrap it in '{ }'");
    return;
  }
  checkStmt(body);
}

void Sema::checkVarDecl(VarDeclStmt *decl) {
  llvm::ArrayRef<Stmt *> block = block_;
  size_t index = blockIndex_;
  block_ = {};
  for (size_t k = 0; k < decl->decls.size(); ++k) {
    VarDeclarator &d = decl->decls[k];
    const Type *declared = d.typeAnn ? resolveType(*d.typeAnn, /*allowVoid=*/false) : nullptr;
    // `const f = first` with a generic `first`: another name for it, as
    // generic as it is.
    if (auto *name = llvm::dyn_cast_or_null<NameExpr>(d.init);
        name && decl->isConst && !declared && !d.destructured && name->typeArgs.empty()) {
      if (FuncSymbol *generic = genericNamed(name)) {
        declare(d.name, d.loc, {nullptr, generic});
        d.alias = true;
        continue;
      }
    }
    // `const id = <T>(x: T) => x` at the top level: a generic function, as if
    // declared with `function` (an arrow there captures nothing: what it uses
    // are globals). Unlike a declaration, it exists from here on.
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init);
        fe && !fe->fn->typeParams.empty() && decl->isConst && !declared && !d.destructured &&
        scopes_.size() == 1) {
      FunctionDecl *fn = fe->fn;
      fn->isExpression = false;
      fn->lifted = true;
      fn->name = d.name;
      declareFunction(fn);
      findCycles({fn});
      lifted_.push_back(fn);
      d.alias = true;
      continue;
    }
    if (auto *fe = llvm::dyn_cast_or_null<FunctionExpr>(d.init)) {
      if (fe->fn->name.empty())
        fe->fn->name = d.name; // as JavaScript names it
      if (decl->isConst && !d.destructured) {
        // The constant exists while its function is checked, so that the
        // function can call itself through it.
        VarSymbol *var = declareVar(d.name, d.loc, nullptr, /*isConst=*/true);
        var->lambda = fe->fn;
        d.sym = var;
        const Type *function = declared && declared->nonOptional()->isFunction() ? declared->nonOptional() : nullptr;
        Signature sig = function ? signatureOf(function) : Signature();
        d.init = checkFunctionExpr(fe, function ? &sig : nullptr);
        if (declared)
          d.init = coerce(d.init, declared);
        // A function of another result is called through what adapts it.
        if (auto *cast = dyn_cast<CastExpr>(d.init); cast && cast->adaptsResult)
          var->lambda = nullptr;
        var->type = declared ? declared : d.init->type;
        continue;
      }
    }
    const Type *type = declared;
    Expr *value = nullptr; // the initializer before its conversion
    bool untyped = false;  // reported with the scope (see noteUntyped)
    // A literal takes its type from where the variable goes.
    const Type *hint = declared;
    std::vector<std::pair<const Type *, llvm::SMLoc>> places;
    if (!declared && d.init && !d.destructured && literalShaped(d.init)) {
      following_ = block.empty() ? std::vector<const Stmt *>() : followingStatements(block, index);
      hint = literalDestination(d, llvm::ArrayRef(decl->decls).drop_front(k + 1));
      places = std::move(destinationConflict_);
      destinationConflict_.clear();
    }
    // `const t = setInterval(() => { ... clearInterval(t) }, 10)`: the
    // callback of a timer runs later, when t holds the timer, so it may use
    // t; through a box, as a closure created before its variable does.
    VarSymbol *early = nullptr;
    if (const Type *timer = d.init && !d.destructured ? timerCallType(d.init) : nullptr) {
      early = declareVar(d.name, d.loc, declared ? declared : timer, decl->isConst);
      early->declSeq = ~0u;
    }
    if (d.init) {
      if (!early)
        initializing_.push_back(d.name);
      if (d.destructured && !d.objectPattern)
        markFused(d.init);
      d.init = value = checkExpr(d.init, hint);
      if (d.destructured && !d.objectPattern)
        d.init = value = iterableOf(d.init);
      if (!early)
        initializing_.pop_back();
      if (declared) {
        d.init = coerce(d.init, declared);
      } else {
        type = d.init->type;
        if (type->isVoid()) {
          diags_.error(d.init->loc(), "an expression of type 'void' cannot initialize a variable");
          type = Type::error();
        } else if (type->isUndefined() && !decl->isConst && !d.destructured) {
          untyped = true;
          type = Type::error();
        } else if (type->isUndefined()) {
          diags_.error(d.init->loc(), llvm::Twine("cannot infer a type from 'undefined'; add one, e.g. '") +
                                          (decl->isConst ? "const " : "let ") + d.name +
                                          ": int | undefined = undefined'");
          type = Type::error();
        }
      }
    } else if (decl->isConst) {
      diags_.error(d.loc, "'const' declarations must be initialized");
      if (!type)
        type = Type::error();
    } else if (!declared) {
      untyped = true;
      type = Type::error();
    }

    if (d.destructured && d.objectPattern) {
      // The fields are read by name: `%0.x` reports what has no field x.
    } else if (d.destructured && (type->isString() || type->isTypedArray()) && !declared) {
      // A string destructures into its characters, as JavaScript's iterator
      // gives them: `[...s]`; a typed array into its elements, and `...rest`
      // is an array of them, as in JavaScript.
      auto *chars = ctx_.make<ArrayLitExpr>(d.init->loc());
      chars->elements.push_back({d.init, /*spread=*/true});
      chars->type = Type::array(type->isString() ? Type::string() : type->elementType());
      d.init = value = chars;
      type = chars->type;
    } else if (d.destructured && type->isIterator() && !declared) {
      // `const [a, b] = it` takes the first two values of the iterator.
      d.init = value = destructuredIterator(d.init, d);
      type = d.init->type;
    } else if (d.destructured && !type->isArray() && !type->isTuple() && !type->isMatch() &&
               !type->isError() &&
               !type->nonOptional()->isJson()) { // a Json reads its elements by index
      std::string message = "only arrays can be destructured, found '" + type->name().str() + "'";
      if (type->isOptional() && type->inner()->isArray())
        message += "; the array may be undefined: check it first";
      diags_.error(d.init ? d.init->loc() : d.loc, message);
      type = Type::error();
    }

    if (early) {
      early->declSeq = ++seq_;
      d.sym = early;
    } else {
      d.sym = declareVar(d.name, d.loc, type, decl->isConst);
    }
    noteRegexVariable(d.sym, value, decl->isConst, block, index);
    if (!places.empty())
      destinations_[d.sym] = std::move(places);
    if (untyped)
      noteUntyped(d.sym, d.init != nullptr);
    if (value)
      assigned(d.sym, value);
    if (decl->isConst && d.init && type->isInteger())
      if (std::optional<int64_t> constant = intConstant(d.init))
        constVars_[d.sym] = *constant;
    // A string constant may name a field: `o[key]`.
    if (decl->isConst && d.init && type->isString())
      if (std::optional<std::string> key = constantKey(d.init))
        constKeys_[d.sym] = std::move(*key);
  }
}

// As in TypeScript, the type of a variable is where it is declared; unlike
// tsc, Sema does not take it from the assignments that follow. It looks at
// them only for the hint, so the error waits for the end of the scope.
void Sema::noteUntyped(VarSymbol *var, bool initUndefined) {
  untyped_.push_back({var, initUndefined, {}});
}

void Sema::assignedUntyped(const VarSymbol *var, const Type *type) {
  for (Untyped &u : untyped_)
    if (u.var == var && !type->isError() && !type->isVoid())
      u.assigned.push_back(type);
}

// Reports the variables declared in the innermost scope, which is ending.
void Sema::reportUntyped() {
  llvm::erase_if(untyped_, [&](const Untyped &u) {
    auto found = scopes_.back().find(u.var->name);
    if (found == scopes_.back().end() || found->second.var != u.var)
      return false;
    // The type that holds every value assigned, if there is one.
    const Type *type = u.initUndefined ? Type::undefinedTy() : nullptr;
    for (const Type *t : u.assigned) {
      const Type *common = type ? unify(type, t) : t;
      type = common ? common : Type::error();
    }
    std::string name = u.var->name.str();
    std::string hint;
    if (type && !type->isError() && !type->isUndefined())
      hint = "write the type: 'let " + name + ": " + type->name().str() + "'";
    else if (u.initUndefined)
      hint = "add one, e.g. 'let " + name + ": int | undefined'";
    else
      hint = "e.g. 'let " + name + ": int' or 'let " + name + " = 0'";
    if (u.initUndefined)
      diags_.error(u.var->loc, "cannot infer a type from 'undefined'; " + hint);
    else
      diags_.error(u.var->loc, "variable '" + name + "' needs a type annotation or an initializer; " + hint);
    return true;
  });
}

// The value of a return: of a function that returns a boolean, checked as a
// condition, so that the function's summary knows what it shows (see
// sema_predicate.cpp).
Expr *Sema::checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested) {
  if (predicateBuilds_.empty() || !predicateBuilds_.back().fn)
    return checkExpr(ret->value, hint);
  if (!tested) {
    // A boolean all the same, such as what a function that tests returns:
    // what it says as a condition.
    Expr *value = checkExpr(ret->value, hint);
    if (value->type->isBool()) {
      Flow whenTrue = flow_, whenFalse = flow_;
      addConditionFacts(value, whenTrue, whenFalse);
      notePredicateExit(whenTrue, whenFalse, ret->value->loc());
    }
    return value;
  }
  Flow whenTrue, whenFalse;
  Expr *value = checkBranching(ret->value, whenTrue, whenFalse);
  flow_ = join(whenTrue, whenFalse);
  notePredicateExit(whenTrue, whenFalse, ret->value->loc());
  return value;
}

void Sema::checkReturn(ReturnStmt *ret) {
  if (!currentFunction_) {
    diags_.error(ret->loc(), "'return' can only be used inside a function");
    if (ret->value)
      ret->value = checkExpr(ret->value);
    return;
  }

  const FuncSymbol *fn = currentFunction_;
  if (fn->decl && fn->decl->isConstructor) {
    if (ret->value) {
      ret->value = checkExpr(ret->value);
      diags_.error(ret->value->loc(), "a constructor cannot return a value: 'new' gives the object");
    } else if (initFn_ == fn->decl && flow_.beforeSuper) {
      diags_.error(ret->loc(), "the constructor returns before 'super(...)', which makes the "
                               "object (JavaScript throws a ReferenceError there)");
    } else if (initFn_ == fn->decl) {
      checkFieldsSet(ret->loc());
    }
    return;
  }
  // What a generator returns is what its iterator gives when it is done; an
  // async function, what its Promise gives.
  const Type *returned = fn->ret && fn->decl && fn->decl->isGenerator &&
                                 (fn->ret->isIterator() || fn->ret->isAsyncIterator())
                             ? fn->ret->returned()
                         : fn->ret && fn->decl && fn->decl->isAsync && fn->ret->isPromise()
                             ? fn->ret->promised()
                             : fn->ret;
  if (!fn->ret) {
    // Its type comes from what its return statements give. A literal takes
    // the type of an object an earlier one gave.
    const Type *hint = nullptr;
    if (ret->value && literalShaped(ret->value))
      for (const ReturnStmt *earlier : *returns_)
        if (earlier->value && !earlier->value->type->isError() &&
            (holdsObjects(earlier->value->type) || earlier->value->type->isResult())) {
          hint = earlier->value->type;
          break;
        }
    if (ret->value)
      ret->value = checkReturnValue(ret, hint, looksBoolean(ret->value));
    returns_->push_back(ret);
    return;
  }
  if (!ret->value) {
    if (fn->asserts)
      notePredicateExit(flow_, Flow(), ret->loc());
    if (!returned->isVoid() && !returned->isError() && !returned->isOptional())
      diags_.error(ret->loc(), functionLabel(fn->name) + " must return a value of type '" +
                                   returned->name() + "'");
    return;
  }

  // `return p` in an async generator awaits p.
  if (fn->decl && fn->decl->isAsync && fn->decl->isGenerator) {
    ret->value = checkExpr(ret->value, returned->isVoid() ? nullptr : returned);
    const Type *given = ret->value->type->isError() ? nullptr : awaitedType(ret->value->type);
    if (given && !given->isUndefined() && !given->isVoid() &&
        (returned->isVoid() || !isImplicitlyConvertible(given, returned)))
      diags_.error(ret->value->loc(), functionLabel(fn->name) + " returns '" + returned->name() +
                                          "', found '" + given->name() + "'");
    return;
  }
  ret->value = checkReturnValue(ret, returned->isVoid() ? nullptr : returned, returned->isBool());
  // `return p` in an async function: its Promise follows p.
  if (fn->decl && fn->decl->isAsync && fn->ret->isPromise() && ret->value->type->isPromise()) {
    const Type *given = ret->value->type->promised();
    if (given != returned && !given->isNever())
      diags_.error(ret->value->loc(), "this Promise gives '" + given->name() + "', but " +
                                          functionLabel(fn->name) + " gives '" +
                                          returned->name() + "'; await it and convert the value");
    return;
  }
  if (returned->isVoid()) {
    // `return f()` with a void `f` is fine, as in TypeScript, and so are
    // `return void f()` and `return undefined`, and any value in a callback
    // whose result is dropped.
    const Type *type = ret->value->type;
    if (!type->isVoid() && !type->isError() && !type->isUndefined() && !fn->discardsResult) {
      diags_.error(ret->value->loc(), functionLabel(fn->name) + " is declared to return 'void', "
                                                               "but returns '" + type->name() + "'");
      noteInheritedReturn(fn->decl);
    } else if (fn->discardsResult) {
      checkDiscarded(ret->value, /*callback=*/true);
    }
    return;
  }
  unsigned errors = diags_.errorCount();
  ret->value = coerce(ret->value, returned);
  if (diags_.errorCount() > errors)
    noteInheritedReturn(fn->decl);
  else if (fn->decl)
    checkByteCallbackResult(fn->decl, ret->value);
}

static bool isTrueLiteral(const Expr *expr) {
  auto *lit = llvm::dyn_cast_or_null<BoolLitExpr>(expr);
  return lit && lit->value;
}

namespace {

// Finds the `break` and `continue` statements inside a statement S that
// leave S: those that jump to S itself (except `continue` to a loop S) or to
// a statement around it.
class Exits {
public:
  Exits(bool isLoop, llvm::ArrayRef<llvm::StringRef> labels) : isLoop_(isLoop), labels_(labels) {}

  bool any(llvm::ArrayRef<Stmt *> stmts) {
    return llvm::any_of(stmts, [&](const Stmt *s) { return in(s); });
  }

  bool in(const Stmt *stmt) {
    switch (stmt->kind()) {
    case Stmt::Kind::Break: {
      llvm::StringRef label = cast<BreakStmt>(stmt)->label;
      return label.empty() ? breakables_ == 0 : !llvm::is_contained(inner_, label);
    }
    case Stmt::Kind::Continue: {
      llvm::StringRef label = cast<ContinueStmt>(stmt)->label;
      if (label.empty())
        return loops_ == 0 && !isLoop_;
      if (llvm::is_contained(inner_, label))
        return false;
      return !(isLoop_ && llvm::is_contained(labels_, label));
    }
    case Stmt::Kind::Block:
      return any(cast<BlockStmt>(stmt)->body);
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      return in(s->thenStmt) || (s->elseStmt && in(s->elseStmt));
    }
    case Stmt::Kind::While:
      return inLoop(cast<WhileStmt>(stmt)->body);
    case Stmt::Kind::DoWhile:
      return inLoop(cast<DoWhileStmt>(stmt)->body);
    case Stmt::Kind::For:
      return inLoop(cast<ForStmt>(stmt)->body);
    case Stmt::Kind::ForOf:
      return inLoop(cast<ForOfStmt>(stmt)->body);
    case Stmt::Kind::Switch: {
      ++breakables_;
      bool found = llvm::any_of(cast<SwitchStmt>(stmt)->cases,
                                [&](const SwitchCase &c) { return any(c.body); });
      --breakables_;
      return found;
    }
    case Stmt::Kind::Labeled: {
      auto *s = cast<LabeledStmt>(stmt);
      inner_.push_back(s->label);
      bool found = in(s->body);
      inner_.pop_back();
      return found;
    }
    case Stmt::Kind::Try: {
      auto *s = cast<TryStmt>(stmt);
      return in(s->block) || (s->handler && in(s->handler)) || (s->finalizer && in(s->finalizer));
    }
    default:
      return false;
    }
  }

private:
  bool inLoop(const Stmt *body) {
    ++loops_;
    ++breakables_;
    bool found = in(body);
    --loops_;
    --breakables_;
    return found;
  }

  bool isLoop_;
  llvm::ArrayRef<llvm::StringRef> labels_; // of S
  llvm::SmallVector<llvm::StringRef, 4> inner_; // declared inside S
  unsigned loops_ = 0, breakables_ = 0; // inside S, around the current statement
};

// Whether some `break` inside `stmt` jumps to one of `labels`.
bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  if (labels.empty())
    return false;
  switch (stmt->kind()) {
  case Stmt::Kind::Break:
    return llvm::is_contained(labels, cast<BreakStmt>(stmt)->label);
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body,
                        [&](const Stmt *s) { return breaksTo(s, labels); });
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    return breaksTo(s->thenStmt, labels) || (s->elseStmt && breaksTo(s->elseStmt, labels));
  }
  case Stmt::Kind::While:
    return breaksTo(cast<WhileStmt>(stmt)->body, labels);
  case Stmt::Kind::DoWhile:
    return breaksTo(cast<DoWhileStmt>(stmt)->body, labels);
  case Stmt::Kind::For:
    return breaksTo(cast<ForStmt>(stmt)->body, labels);
  case Stmt::Kind::ForOf:
    return breaksTo(cast<ForOfStmt>(stmt)->body, labels);
  case Stmt::Kind::Switch:
    return llvm::any_of(cast<SwitchStmt>(stmt)->cases, [&](const SwitchCase &c) {
      return llvm::any_of(c.body, [&](const Stmt *s) { return breaksTo(s, labels); });
    });
  case Stmt::Kind::Labeled:
    return breaksTo(cast<LabeledStmt>(stmt)->body, labels);
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    return breaksTo(s->block, labels) || (s->handler && breaksTo(s->handler, labels)) ||
           (s->finalizer && breaksTo(s->finalizer, labels));
  }
  default:
    return false;
  }
}

} // namespace

bool Sema::isExit(const Stmt *stmt) {
  auto *s = dyn_cast<ExprStmt>(stmt);
  auto *call = s ? dyn_cast<CallExpr>(s->expr) : nullptr;
  return call && call->builtin == Builtin::ProcessExit;
}

// Conservative: true only if control can never reach the end of `stmt`.
bool Sema::alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) {
  auto returns = [](const Stmt *s) { return alwaysReturns(s); };
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
  case Stmt::Kind::Throw:
    return true;
  case Stmt::Kind::Expr:
    return isExit(stmt);
  // A `finally` that never ends ends the whole statement; otherwise the
  // block, and the `catch` that runs when it throws, must both never end.
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    if (breaksTo(stmt, labels))
      return false;
    if (s->finalizer && alwaysReturns(s->finalizer))
      return true;
    return alwaysReturns(s->block) && (!s->handler || alwaysReturns(s->handler));
  }
  case Stmt::Kind::Block:
    return llvm::any_of(cast<BlockStmt>(stmt)->body, returns) && !breaksTo(stmt, labels);
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    // `if (true) S` runs S; so does the `if` of an instance whose types
    // decided the condition, which keeps only the branch that runs.
    if (auto *lit = dyn_cast<BoolLitExpr>(s->cond)) {
      if (lit->value && !s->elseStmt)
        return alwaysReturns(s->thenStmt) && !breaksTo(stmt, labels);
      if (!lit->value && isa<EmptyStmt>(s->thenStmt))
        return s->elseStmt && alwaysReturns(s->elseStmt) && !breaksTo(stmt, labels);
    }
    return s->elseStmt && alwaysReturns(s->thenStmt) && alwaysReturns(s->elseStmt) &&
           !breaksTo(stmt, labels);
  }
  // An endless loop never falls through unless something leaves it.
  case Stmt::Kind::While: {
    auto *s = cast<WhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::DoWhile: {
    auto *s = cast<DoWhileStmt>(stmt);
    return isTrueLiteral(s->cond) && !Exits(true, labels).in(s->body);
  }
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    return (!s->cond || isTrueLiteral(s->cond)) && !Exits(true, labels).in(s->body);
  }
  // A switch with `default` always enters some case, and control falls
  // through to the end of the last one unless something leaves the switch.
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    bool hasDefault =
        s->exhaustive || llvm::any_of(s->cases, [](const SwitchCase &c) { return !c.test; });
    if (!hasDefault || s->cases.empty())
      return false;
    Exits exits(false, labels);
    if (llvm::any_of(s->cases, [&](const SwitchCase &c) { return exits.any(c.body); }))
      return false;
    return llvm::any_of(s->cases.back().body, returns);
  }
  case Stmt::Kind::Labeled: {
    llvm::SmallVector<llvm::StringRef, 2> all(labels.begin(), labels.end());
    while (auto *labeled = dyn_cast<LabeledStmt>(stmt)) {
      all.push_back(labeled->label);
      stmt = labeled->body;
    }
    return alwaysReturns(stmt, all);
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

Expr *Sema::checkExpr(Expr *expr, const Type *expected, bool wraps) {
  // In an instance, an expression the generic code converted is converted
  // the same way (see noteForced).
  if (checkingInstance_ && !forced_.empty()) {
    auto found = forced_.find({expr->loc().getPointer(), unsigned(expr->kind())});
    if (found != forced_.end()) {
      const Type *to = found->second;
      return forceConversion(checkExprKind(expr, expected, wraps), to);
    }
  }
  return checkExprKind(expr, expected, wraps);
}

Expr *Sema::checkExprKind(Expr *expr, const Type *expected, bool wraps) {
  // A generic function as a value: its type arguments come from the type
  // expected.
  if (auto *name = dyn_cast<NameExpr>(expr))
    if (FuncSymbol *generic = genericNamed(name)) {
      const Type *function = expected ? expected->nonOptional() : nullptr;
      Signature sig = function && function->isFunction() ? signatureOf(function) : Signature();
      return checkGenericValue(name, generic, function && function->isFunction() ? &sig : nullptr);
    }
  // A built-in such as Math.sqrt where a function is expected.
  if (expected && (isa<NameExpr>(expr) || isa<MemberExpr>(expr)))
    if (const Type *function = expected->nonOptional(); function->isFunction()) {
      Signature sig = signatureOf(function);
      if (FunctionExpr *wrapper = builtinFunctionValue(expr, sig, unsigned(sig.params.size()), {}))
        return checkFunctionExpr(wrapper, &sig);
    }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return checkIntLit(cast<IntLitExpr>(expr), expected, /*negated=*/false);
  case Expr::Kind::FloatLit: {
    // Where a literal type is expected, the type of its value.
    if (const Type *literal = numberLiteralFor(expected, cast<FloatLitExpr>(expr)->value, true)) {
      expr->type = literal;
      return expr;
    }
    // Where a decimal is expected, the literal is read exactly.
    const Type *member = numberMemberOf(expected, /*fraction=*/true);
    if (expected && (expected->nonOptional()->isDecimal() || (member && member->isDecimal()))) {
      expr->type = Type::decimalTy();
      checkDecimalLiteralRange(expr);
      return expr;
    }
    expr->type = Type::doubleTy();
    return expr;
  }
  case Expr::Kind::BoolLit: {
    const Type *literal = boolLiteralFor(expected, cast<BoolLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::boolTy();
    return expr;
  }
  case Expr::Kind::StringLit: {
    const Type *literal = stringLiteralFor(expected, cast<StringLitExpr>(expr)->value);
    expr->type = literal ? literal : Type::string();
    return expr;
  }
  case Expr::Kind::RegexLit:
    return checkRegexLit(cast<RegexLitExpr>(expr));
  case Expr::Kind::Undefined:
    expr->type = Type::undefinedTy();
    return expr;
  case Expr::Kind::Template:
    return checkTemplate(cast<TemplateExpr>(expr), expected);
  case Expr::Kind::ArrayLit:
    return checkArrayLit(cast<ArrayLitExpr>(expr), expected);
  case Expr::Kind::ObjectLit:
    return checkObjectLit(cast<ObjectLitExpr>(expr), expected);
  case Expr::Kind::Name:
    if (Expr *null = checkNull(cast<NameExpr>(expr), expected))
      return null;
    return checkName(cast<NameExpr>(expr));
  case Expr::Kind::Member:
    return checkMember(cast<MemberExpr>(expr), expected);
  case Expr::Kind::Index:
    return checkIndex(cast<IndexExpr>(expr));
  case Expr::Kind::Unary:
    return checkUnary(cast<UnaryExpr>(expr), expected, wraps);
  case Expr::Kind::Binary:
    return checkBinary(cast<BinaryExpr>(expr), expected, wraps);
  case Expr::Kind::Assign:
    return checkAssign(cast<AssignExpr>(expr));
  case Expr::Kind::Update:
    return checkUpdate(cast<UpdateExpr>(expr));
  case Expr::Kind::Call:
    return checkCall(cast<CallExpr>(expr), expected);
  case Expr::Kind::New:
    return checkNew(cast<NewExpr>(expr), expected);
  case Expr::Kind::Class:
    return checkClassExpr(cast<ClassExpr>(expr), expected);
  case Expr::Kind::Conditional:
    return checkConditional(cast<ConditionalExpr>(expr), expected);
  case Expr::Kind::Cast:
    return checkCast(cast<CastExpr>(expr));
  case Expr::Kind::Sequence:
    return checkSequence(cast<SequenceExpr>(expr), expected, wraps);
  case Expr::Kind::OptionalChain:
    return checkOptionalChain(cast<OptionalChainExpr>(expr));
  case Expr::Kind::Function: {
    // Where a function type is expected, the parameters take its types.
    const Type *function = expected ? expected->nonOptional() : nullptr;
    if (function && function->isFunction()) {
      Signature sig = signatureOf(function);
      return checkFunctionExpr(cast<FunctionExpr>(expr), &sig);
    }
    return checkFunctionExpr(cast<FunctionExpr>(expr), nullptr);
  }
  case Expr::Kind::Yield:
    return checkYield(cast<YieldExpr>(expr));
  case Expr::Kind::Await:
    return checkAwait(cast<AwaitExpr>(expr), expected);
  }
  llvm_unreachable("unknown expression kind");
}

Expr *Sema::checkSequence(SequenceExpr *seq, const Type *expected, bool wraps) {
  for (size_t i = 0; i + 1 < seq->exprs.size(); ++i) {
    seq->exprs[i] = checkExpr(seq->exprs[i]);
    checkDiscarded(seq->exprs[i]);
  }
  Expr *&last = seq->exprs.back();
  last = checkExpr(last, expected, wraps);
  seq->type = last->type;
  return seq;
}

Expr *Sema::checkOptionalChain(OptionalChainExpr *chain) {
  bool outer = std::exchange(chainShortCircuits_, false);
  Flow before = flow_;
  chain->chain = checkExpr(chain->chain);
  chain->mayShortCircuit = chainShortCircuits_;
  chainShortCircuits_ = outer;
  const Type *type = chain->chain->type;
  chain->type = type;
  if (!chain->mayShortCircuit || type->isError())
    return chain;
  // The rest of the chain may not have run.
  flow_ = join(before, flow_);
  if (!type->isVoid())
    chain->type = Type::optional(type);
  return chain;
}

Expr *Sema::chainLink(Expr *object) {
  const Type *type = object->type;
  // A Json that is null ends the chain too, as in JavaScript: `j?.name`.
  if (type->nonOptional()->isJson()) {
    auto *link = ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
    link->type = Type::json();
    link->shortCircuit = true;
    chainShortCircuits_ = true;
    return link;
  }
  if (!type->isOptional())
    return object; // `?.` on a value that is always there is a plain `.`
  auto *link = ctx_.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type->inner();
  link->shortCircuit = true;
  chainShortCircuits_ = true;
  return link;
}

// An integer literal takes the numeric type the context expects; otherwise
// int, or long if it does not fit.
Expr *Sema::checkIntLit(IntLitExpr *lit, const Type *expected, bool negated) {
  auto fits = [&](unsigned bits) {
    uint64_t limit = uint64_t(1) << (bits - 1);
    return negated ? lit->value <= limit : lit->value < limit;
  };

  if (expected) {
    // `0 | 1`, or a union with a number type: that type.
    double value = negated ? -double(lit->value) : double(lit->value);
    if (const Type *literal = numberLiteralFor(expected, value, /*fraction=*/false);
        literal && (!negated || value != 0)) {
      lit->type = literal;
      return lit;
    }
    if (const Type *member = numberMemberOf(expected, /*fraction=*/false))
      expected = member;
    expected = expected->nonOptional();
  }
  if (expected && (expected->isNumeric() || expected->isDecimal())) {
    lit->type = expected;
    if (expected->isInteger() && !fits(expected->bitWidth()))
      diags_.error(lit->loc(), "integer literal does not fit in type '" + expected->name() + "'");
  } else if (fits(32)) {
    lit->type = Type::intTy();
  } else if (fits(64)) {
    lit->type = Type::longTy();
  } else { // only a double holds it, as any number in JavaScript
    lit->type = Type::doubleTy();
  }
  // The type a literal takes in generic code may come from a type parameter
  // (`reduce(f, 0)` over `T extends number` starts from a double); an
  // instance gives it the same.
  std::pair<const char *, unsigned> key{lit->loc().getPointer(), unsigned(Expr::Kind::IntLit)};
  if (!functions_.empty() && functions_.front()->sym && functions_.front()->sym->isGeneric())
    forced_[key] = lit->type;
  else if (checkingInstance_)
    if (const Type *type = forced_.lookup(key); type && isImplicitlyConvertible(lit->type, type))
      lit->type = type;
  return lit;
}

const Type *Sema::varType(VarSymbol *var) {
  if (var->type)
    return var->type;
  // A function's variable, while its type is being worked out.
  FunctionDecl *fn = var->function ? var->function : var->lambda;
  if (!fn || !fn->sym)
    return Type::error();
  const Type *type = functionType(fn->sym);
  if (var->function && fn->sym->ret)
    var->type = type;
  return type;
}

Expr *Sema::checkName(NameExpr *name) {
  if (name->name == "this")
    return checkThis(name);
  // A class as a value that Sema made (see classValueAt), which may have no
  // name.
  if (name->cls && name->type)
    return name;
  if (Expr *node = checkNodeName(name))
    return node;
  ScopeEntry entry = lookupValue(name->name);
  if (!name->typeArgs.empty() && (entry.var || entry.func))
    diags_.error(name->loc(), "'" + name->name + "' is not a generic function; remove the type arguments");
  if (entry.var) {
    // A static field of a class expression gets its value once, when the
    // class is made, not at each call of the function around it.
    if (staticInit_ && currentClass_ && currentClass_->expression && !entry.var->isGlobal &&
        llvm::is_contained(currentClass_->locals, name->name))
      diags_.error(name->loc(), "a class expression cannot use '" + name->name +
                                    "', a variable of the function around it: the class is made "
                                    "once, not at each call, and its static fields get their "
                                    "values the first time; pass the value to its constructor, "
                                    "or keep it in a global");
    name->var = entry.var;
    noteUse(entry.var, name->loc());
    name->type = varType(entry.var);
    checkVisible(name, entry.var);
    // Of a type parameter, with its type known here from `typeof`.
    if (const Type *narrowed = narrowedType(entry.var))
      return convert(name, narrowed);
    // Narrowed: known to hold a value here.
    if (name->type->isOptional() && isDefined(entry.var))
      return convert(name, name->type->inner());
    // A Result known to be an Ok is a `Result<T, never>`, whose value can be
    // read; one known to be an Err, a `Result<never, E>`.
    if (const Type *type = name->type; type->isResult()) {
      if (flow_.okResults.contains(entry.var) && !type->errType()->isNever())
        return convert(name, Type::result(type->okType(), Type::never()));
      if (flow_.errResults.contains(entry.var) && !type->okType()->isNever())
        return convert(name, Type::result(Type::never(), type->errType()));
    }
    return name;
  }
  if (!entry.func && !entry.enumDecl)
    if (Expr *constant = globalConstant("", name->name, name->loc()))
      return constant;
  if (entry.func) {
    // A top-level function as a value.
    name->func = entry.func;
    name->type = functionType(entry.func);
    return name;
  }
  // `String`, `Date`... as values: the classes that records `design:*` hold
  // (docs/decorators.md, section 3).
  if (!entry.var && !entry.func && !entry.enumDecl && !entry.cls)
    if (ClassDecl *cls = builtinConstructor(name->name))
      return checkClassValue(name, cls);
  static const llvm::StringLiteral builtins[] = {"String", "Number", "parseInt", "parseFloat", "isNaN",
                                                 "isFinite", "Boolean"};
  // The global objects, by what is used of them first.
  static const std::pair<llvm::StringLiteral, llvm::StringLiteral> objects[] = {
      {"process", "process.argv"}, {"console", "console.log(x)"}, {"Date", "Date.now()"},
      {"performance", "performance.now()"}, {"RegExp", "new RegExp(pattern)"},
      {"JSON", "JSON.parse(text)"}};
  auto object = llvm::find_if(objects, [&](const auto &o) { return o.first == name->name; });
  llvm::StringRef neverthrow = entry.enumDecl ? llvm::StringRef() : neverthrowName(name->name);
  if (neverthrow == "ok" || neverthrow == "err")
    diags_.error(name->loc(), "'" + name->name + "' can be used as a value only where a function "
                                                 "type is expected, e.g. 'xs.map(" +
                                  name->name + ")'");
  else if (neverthrow == "Result")
    diags_.error(name->loc(), "'" + name->name +
                                  "' can only be used through its functions, e.g. '" +
                                  name->name + ".combine(results)'");
  else if (neverthrow == "Ok" || neverthrow == "Err")
    diags_.error(name->loc(), "'" + name->name +
                                  "' can be used only as a type; make a Result with '" +
                                  (neverthrow == "Ok" ? "ok(value)" : "err(error)") +
                                  "', and test one with '" +
                                  (neverthrow == "Ok" ? "isOk()" : "isErr()") + "'");
  else if (neverthrow == "fromThrowable")
    diags_.error(name->loc(), "'" + name->name + "' can only be called: '" + name->name + "(f)'");
  else if (neverthrow == "safeTry")
    diags_.error(name->loc(), "'" + name->name +
                                  "' is not supported yet; call it: 'safeTry(function* () { ... "
                                  "})'");
  else if (neverthrow == "ResultAsync")
    diags_.error(name->loc(), "'" + name->name +
                                  "' can only be used through its functions, e.g. '" +
                                  name->name + ".fromPromise(p, toError)'");
  else if (!neverthrow.empty())
    diags_.error(name->loc(), "'" + name->name + "' can only be called: '" + name->name + "(...)'");
  else if (name->name == "this")
    diags_.error(name->loc(), "'this' exists only in the methods of classes, which are not supported yet");
  else if (llvm::is_contained(builtins, name->name))
    diags_.error(name->loc(), "'" + name->name + "' can be used as a value only where a function type "
                                                 "is expected, e.g. 'const f: (s: string) => double = " +
                                  name->name + "'");
  else if (name->name == "super")
    diags_.error(name->loc(), "'super' is not a value: call the constructor of the class "
                              "extended, 'super(...)', or a method of it, 'super.m(...)'");
  else if (name->name == "arguments")
    diags_.error(name->loc(), "'arguments' is not supported; use a rest parameter, e.g. '...args: int[]'");
  else if (name->name == "BigInt")
    reportBigInt(name->loc(), "'BigInt'");
  else if (name->name == "decimal")
    diags_.error(name->loc(), "'decimal' is used as 'decimal(x)', which makes a decimal of x, or "
                              "through its functions, e.g. 'decimal.max(a, b)'");
  else if (name->name == "null")
    diags_.error(name->loc(), "'null' is not supported: the only value for nothing is 'undefined'; "
                              "write 'undefined' (null exists only in Json values)");
  else if (const char *why = whyUnsupported(name->name))
    diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
  else if (entry.enumDecl)
    diags_.error(name->loc(), "enum '" + name->name + "' can only be used as '" + name->name +
                                  ".Member' or '" + name->name + "[value]'");
  else if (entry.cls)
    return checkClassValue(name, entry.cls);
  // A class expression sees its own name in its body, as in JavaScript.
  else if (currentClass_ && currentClass_->expression && !currentClass_->name.empty() &&
           currentClass_->name == name->name)
    return checkClassValue(name, currentClass_);
  else if (currentClass_ && currentClass_->expression &&
           llvm::is_contained(currentClass_->locals, name->name))
    diags_.error(name->loc(), "a class expression cannot use '" + name->name +
                                  "', a variable of the function around it: the class is made "
                                  "once, not at each call, and its code runs later; pass the "
                                  "value to its constructor, or keep it in a global");
  else if (reportNotLoaded(name)) {
    // A name of a file that is checked later: reported.
  } else if (early_ &&
             (state(module_).topLevelNames.contains(name->name) || declaredLater(name->name)))
    diags_.error(name->loc(), "'" + name->name + "' is declared after the code that first calls '" +
                                  early_->name + "', which needs to know what '" + early_->name +
                                  "' returns; move the declaration up, or declare the return type");
  else if (const BlockNames *later = declaredLater(name->name); later && functions_.size() > later->functions)
    diags_.error(name->loc(), "'" + name->name + "' is declared later in the block, after this function "
                                                 "is created; move the declaration up, or declare the "
                                                 "function with 'function', which can use it");
  else if (later)
    diags_.error(name->loc(), "'" + name->name + "' is used before it is declared");
  else if (llvm::is_contained(initializing_, name->name))
    diags_.error(name->loc(), "'" + name->name + "' is used in its own initializer, before it has a value");
  else if (object != std::end(objects))
    diags_.error(name->loc(), "'" + name->name + "' can only be used through its properties, e.g. '" +
                                  object->second + "'");
  else
    diags_.error(name->loc(), "cannot find name '" + name->name + "'");
  name->type = Type::error();
  return name;
}

void Sema::checkVisible(NameExpr *name, const VarSymbol *var) {
  if (otherCaseVars_.contains(var))
    diags_.error(name->loc(), "'" + name->name +
                                  "' is declared in an earlier case of the 'switch'; declare it "
                                  "before the 'switch' to use it in several cases");
}

Expr *Sema::checkUnary(UnaryExpr *unary, const Type *expected, bool wraps) {
  const char *op = opSpelling(unary->op);
  switch (unary->op) {
  case UnaryOp::Neg:
  case UnaryOp::Plus: {
    // `-1` where a literal type is expected, `-1 | 1`: the literal is
    // checked as an int and negated, and the result is of the literal type.
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal = numberLiteralFor(expected, -double(lit->value), false);
          literal && lit->value != 0) {
        unary->operand = checkIntLit(lit, literal->literalBase(), /*negated=*/true);
        unary->type = literal;
        return unary;
      }
    }
    if (auto *lit = dyn_cast<FloatLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
      if (const Type *literal = numberLiteralFor(expected, -lit->value, true)) {
        lit->type = Type::doubleTy();
        unary->type = literal;
        return unary;
      }
    }
    const Type *wanted = expected ? expected->nonOptional() : nullptr;
    if (const Type *member = numberMemberOf(expected, isa<FloatLitExpr>(unary->operand)))
      wanted = member;
    const Type *hint = wanted && (wanted->isNumeric() || wanted->isDecimal()) ? wanted : nullptr;
    if (auto *lit = dyn_cast<IntLitExpr>(unary->operand))
      unary->operand = checkIntLit(lit, hint, /*negated=*/unary->op == UnaryOp::Neg);
    else
      unary->operand = apparent(checkExpr(unary->operand, hint, wraps));
    const Type *type = unary->operand->type;
    if (type->isNever()) { // see neverOperands
      unary->operand = convert(unary->operand, Type::doubleTy());
      type = Type::doubleTy();
    }
    if (type->isString() || type->isBool()) {
      // `+s` is Number(s), and `-s` is -Number(s), as in JavaScript.
      unary->type = Type::doubleTy();
      break;
    }
    // `+d` is the time of a date.
    if (type->isDate() && unary->op == UnaryOp::Plus)
      return dateTime(unary->operand);
    bool literal = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
    if (type->isDecimal() && unary->op == UnaryOp::Plus && !literal) {
      // `+x` makes a number of x in JavaScript, which a decimal does only
      // explicitly; `+2` is just 2.
      diags_.error(unary->loc(), "unary '+' cannot be applied to a decimal; for a double, write "
                                 "'Number(x)', or drop the '+'");
      type = Type::error();
    } else if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
      diags_.error(unary->loc(), llvm::Twine("operator '") + op + "' cannot be applied to type '" +
                                     type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::Not: {
    Flow whenTrue, whenFalse;
    checkBranching(unary, whenTrue, whenFalse);
    flow_ = join(whenTrue, whenFalse);
    break;
  }
  case UnaryOp::BitNot: {
    const Type *hint = expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    unary->operand = apparent(checkExpr(unary->operand, hint, /*wraps=*/true));
    const Type *type = unary->operand->type;
    if (type->isFloat()) {
      unary->operand = toInt32(unary->operand);
      type = Type::intTy();
    } else if (!type->isInteger() && !type->isError()) {
      diags_.error(unary->loc(), "operator '~' cannot be applied to type '" + type->name() + "'");
      type = Type::error();
    }
    unary->type = type;
    break;
  }
  case UnaryOp::TypeOf:
    // Known from the type, except whether a `T | undefined` holds a value.
    unary->operand = checkExpr(unary->operand);
    unary->type = Type::string();
    break;
  case UnaryOp::Void:
    // `void f()` runs f and drops what it returns; the value is undefined,
    // usable wherever the literal `undefined` is.
    unary->operand = checkExpr(unary->operand);
    unary->type = Type::undefinedTy();
    break;
  case UnaryOp::Delete:
    return checkDelete(unary);
  case UnaryOp::NonNull: {
    // `x!`: the program stops if x is undefined, so from here on a variable
    // holds a value.
    unary->operand = checkExpr(unary->operand, expected);
    const Type *type = unary->operand->type;
    if (type->isUndefined()) {
      diags_.error(unary->loc(), "the value is always undefined");
      type = Type::error();
    }
    unary->type = type->nonOptional();
    if (auto *name = dyn_cast<NameExpr>(unary->operand); name && name->var && type->isOptional())
      setDefined(name->var, true);
    else if (std::optional<FieldPath> path = fieldPath(unary->operand); path && type->isOptional())
      defineField(flow_, *path);
    break;
  }
  }
  if (unary->type && unary->type->isInteger())
    foldUnary(unary, wraps);
  return unary;
}

// A name or an integer literal as written, for messages that show how to
// rewrite the code around it.
static std::optional<std::string> simpleText(const Expr *expr) {
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name.str();
  if (auto *lit = dyn_cast<IntLitExpr>(expr))
    return std::to_string(lit->value);
  return std::nullopt;
}

// `delete` removes a property of an object, and an object always has the
// fields of its type. An array element would leave a hole, and arrays have
// none.
Expr *Sema::checkDelete(UnaryExpr *unary) {
  unary->type = Type::boolTy();
  // A field that is not there and one that holds undefined are the same.
  auto reportField = [&](const Expr *object, const Field *field) {
    if (field->type->isOptional())
      diags_.error(unary->loc(), "'delete' cannot remove field '" + field->name +
                                     "': a field that is not there is undefined; write '" +
                                     fieldText(object, field->name) + " = undefined'");
    else
      diags_.error(unary->loc(), "'delete' cannot remove field '" + field->name +
                                     "': an object always has the fields of its type");
  };
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  if (auto *member = dyn_cast<MemberExpr>(unary->operand); member && !member->optional) {
    // The object first: `delete r.key` removes a key of a Record.
    member->object = checkExpr(member->object);
    const Type *object = member->object->type;
    if (object->isDict())
      return checkDictDelete(unary, member->object, nullptr, member->name);
    if (object->isError())
      return unary;
    if (object->nonOptional()->isObject() && object->nonOptional()->field(member->name))
      reportField(member->object, object->nonOptional()->field(member->name));
    else
      diags_.error(unary->loc(), "'delete' removes fields of objects, and this is not a field");
    return unary;
  }
  if (!index) {
    unary->operand = checkExpr(unary->operand);
    const Type *type = unary->operand->type;
    auto *member = dyn_cast<MemberExpr>(unary->operand);
    if (type->isError())
      return unary;
    const Type *object = member ? member->object->type->nonOptional() : nullptr;
    if (object && object->isObject() && object->field(member->name))
      reportField(member->object, object->field(member->name));
    else
      diags_.error(unary->loc(), "'delete' removes fields of objects, and this is not a field");
    return unary;
  }
  std::string array = simpleText(index->object).value_or("a");
  std::string position = simpleText(index->index).value_or("i");
  index->object = checkExpr(index->object);
  if (index->object->type->isDict())
    return checkDictDelete(unary, index->object, index->index, {});
  index->index = checkExpr(index->index);
  index->type = Type::error();
  const Type *type = index->object->type->nonOptional();
  if (type->isArray())
    diags_.error(unary->loc(), "'delete' would leave a hole in the array, and arrays have none; "
                               "remove the element with '" + array + ".splice(" + position +
                                   ", 1)', or replace it: '" + array + "[" + position + "] = ...'");
  else if (type->isString())
    diags_.error(unary->loc(), "strings are immutable; a character cannot be deleted");
  else if (type->isTuple())
    diags_.error(unary->loc(),
                 "'delete' would leave a hole in the tuple, which has a fixed length; assign "
                 "the element instead");
  else if (std::optional<std::string> key =
               type->isObject() ? constantKey(index->index) : std::nullopt;
           key && type->field(*key))
    reportField(index->object, type->field(*key));
  else if (type->isObject())
    diags_.error(unary->loc(), "'delete' cannot remove a field: an object always has the fields of its "
                               "type");
  else if (!type->isError())
    diags_.error(unary->loc(), "'delete' removes fields of objects, and this is not a field");
  return unary;
}

// `key in object` asks whether the object has the property; an object always
// has the fields of its type. Arrays have no holes, so for them it is a
// comparison of the index with the length.
Expr *Sema::checkIn(BinaryExpr *bin) {
  std::optional<std::string> key = simpleText(bin->lhs);
  bool literalKey = isa<IntLitExpr>(bin->lhs);
  std::string array = simpleText(bin->rhs).value_or("a");
  bin->lhs = checkExpr(bin->lhs);
  bin->rhs = checkExpr(bin->rhs);
  bin->type = Type::boolTy();
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (l->isError() || r->isError())
    return bin;
  if (r->nonOptional()->isDict())
    return checkDictIn(bin);
  // `"radius" in s` tells the members of a union of objects apart by their
  // fields (docs/objects.md, section 7).
  if (const Type *u = r->nonOptional(); u->isUnion()) {
    if (r->isOptional()) {
      diags_.error(bin->rhs->loc(), "the value may be undefined, and 'in' would throw then; check "
                                    "it first");
      return bin;
    }
    auto *text = dyn_cast<StringLitExpr>(bin->lhs);
    std::optional<std::string> name = text ? constantKey(bin->lhs) : std::nullopt;
    const Type *objects = u->slotPart(UnionSlot::Object);
    if (!name || !objects) {
      diags_.error(bin->loc(), "operator 'in' tells the objects of a union apart by a field, "
                               "written as a string literal: '\"radius\" in shape'");
      return bin;
    }
    const Type *holder = objects;
    size_t with = 0, total = 0;
    for (const Type *m :
         objects->isUnion() ? objects->members() : llvm::ArrayRef<const Type *>(holder)) {
      ++total;
      const Field *f = m->field(*name);
      with += f != nullptr;
      if (f && f->type->isOptional()) {
        diags_.error(bin->loc(), "operator 'in' cannot test field '" + *name + "' of '" +
                                     m->name() + "', which may hold undefined, as one that is "
                                     "not there; check the value instead: '" +
                                     fieldText(bin->rhs, *name) + " != undefined'");
        return bin;
      }
    }
    if (with == 0 || (with == total && objects == u))
      diags_.error(bin->loc(), "operator 'in' is always " + std::string(with ? "true" : "false") +
                                   " here: " + (with ? "every" : "no") + " member of '" +
                                   u->name() + "' has field '" + *name + "'");
    return bin;
  }
  if (const Type *object = r->nonOptional(); object->isObject()) {
    // A field that is not there and one that holds undefined are the same.
    std::optional<std::string> name = object->isTuple() ? std::nullopt : constantKey(bin->lhs);
    const Field *field = name ? object->field(*name) : nullptr;
    if (object->isTuple())
      diags_.error(
          bin->loc(),
          "operator 'in' is not supported for tuples: a tuple always has all its elements");
    else if (field && field->type->isOptional())
      diags_.error(bin->loc(),
                   "operator 'in' is not supported for objects: a field that is not there is "
                   "undefined; check it with '" +
                       fieldText(bin->rhs, *name) + " != undefined'");
    else if (field)
      diags_.error(bin->loc(),
                   "operator 'in' is not supported for objects: an object always has the "
                   "fields of its type, so this would always be true");
    else if (name)
      diags_.error(bin->loc(), "operator 'in' is not supported for objects: type '" +
                                   object->name() + "' has no field '" + *name +
                                   "', so this would always be false");
    else
      diags_.error(bin->loc(),
                   "operator 'in' is not supported for objects: an object always has the "
                   "fields of its type");
    return bin;
  }
  if (!r->nonOptional()->isArray()) {
    reportOperandTypes(bin->loc(), "in", l, r);
    return bin;
  }
  std::string i = l->isInteger() && key ? *key : "i";
  std::string check = literalKey ? i + " < " + array + ".length"
                                 : i + " >= 0 && " + i + " < " + array + ".length";
  diags_.error(bin->loc(), "operator 'in' is not supported for arrays, which have no holes; compare "
                           "the index with the length instead: '" + check + "'");
  return bin;
}

namespace {

// The source text of the two sides of `bin`, an operator `length` characters
// long; empty if the source is not known.
std::pair<llvm::StringRef, llvm::StringRef> operandTexts(llvm::StringRef source, llvm::SMLoc op,
                                                         size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end())
    return {};
  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

// An operand as it was before toStringOperand converted it.
const Expr *unconverted(const Expr *expr) {
  auto *cast = dyn_cast<CastExpr>(expr);
  return cast && cast->isImplicit() && cast->type->isString() ? cast->operand : expr;
}

// The parts `+` joins in the chain ending at `bin`, with their source text:
// for `a + b + "x" + c` they are `a + b` (a sum: the numbers before the first
// string add up, as in JavaScript), "x" and c.
void concatParts(const BinaryExpr *bin, std::vector<std::pair<const Expr *, llvm::StringRef>> &parts) {
  auto [lhs, rhs] = operandTexts(bin->source, bin->loc(), 1);
  auto *left = dyn_cast<BinaryExpr>(unconverted(bin->lhs));
  if (left && left->op == BinaryOp::Add && left->type && left->type->isString())
    concatParts(left, parts);
  else
    parts.push_back({unconverted(bin->lhs), lhs});
  parts.push_back({unconverted(bin->rhs), rhs});
}

// `text` without parentheses around all of it, which `${...}` makes
// unnecessary. Text with quotes keeps them: a parenthesis in a string would
// confuse the count.
llvm::StringRef withoutParens(llvm::StringRef text) {
  while (text.size() >= 2 && text.front() == '(' && text.back() == ')' &&
         text.find_first_of("\"'`") == llvm::StringRef::npos) {
    int depth = 0;
    size_t close = 0;
    for (size_t i = 0; i < text.size() && !close; ++i) {
      depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
      if (depth == 0)
        close = i;
    }
    if (close != text.size() - 1)
      break;
    text = text.drop_front().drop_back().trim();
  }
  return text;
}

// The parts as one template literal, or "" if some text is not known. If
// the template would hold just one value, `only` gets its text.
std::string concatTemplate(llvm::ArrayRef<std::pair<const Expr *, llvm::StringRef>> parts,
                           llvm::StringRef &only) {
  std::string result = "`";
  unsigned values = 0;
  for (auto [expr, text] : parts) {
    if (text.empty())
      return "";
    bool quoted = text.size() >= 2 && text.back() == text.front();
    if (isa<StringLitExpr>(expr) && quoted && (text.front() == '"' || text.front() == '\'')) {
      // The escapes of the literal mean the same in a template; a backquote
      // and `${` would not.
      llvm::StringRef body = text.drop_front().drop_back();
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
          result += body.substr(i, 2);
          ++i;
          continue;
        }
        if (body[i] == '`' || (body[i] == '$' && body.substr(i + 1).starts_with("{")))
          result += '\\';
        result += body[i];
      }
    } else if (isa<TemplateExpr>(expr) && quoted && text.front() == '`') {
      result += text.drop_front().drop_back();
    } else {
      only = withoutParens(text);
      ++values;
      result += ("${" + only + "}").str();
    }
  }
  if (values != 1 || result.size() != only.size() + 4) // "`${" and "}"
    only = "";
  return result + "`";
}

} // namespace

Expr *Sema::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = opSpelling(bin->op);
  if (expected)
    expected = expected->nonOptional();
  switch (bin->op) {
  case BinaryOp::In:
    return checkIn(bin);

  case BinaryOp::InstanceOf:
    return checkInstanceOf(bin);

  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Flow whenTrue, whenFalse;
    checkLogical(bin, expected, whenTrue, whenFalse, /*tested=*/false,
                 bin->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or);
    flow_ = join(whenTrue, whenFalse);
    return bin;
  }

  case BinaryOp::Nullish:
    return checkNullish(bin, expected);

  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return checkEquality(bin);

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    checkComparisonOperands(bin);
    neverOperands(bin);
    bin->type = Type::boolTy();
    // Two values of one type parameter compare as TypeScript allows; each
    // instance checks its type (objects do not compare).
    if (bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type)
      return bin;
    bin->lhs = apparent(bin->lhs);
    bin->rhs = apparent(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (l->isError() || r->isError())
      return bin;
    // Strings compare by UTF-16 code units, as in JavaScript.
    if (l->isString() && r->isString())
      return bin;
    if (Expr *dates = checkDateOperands(bin))
      return dates;
    if (l->isDecimal() || r->isDecimal())
      return checkDecimalComparison(bin);
    if (l->isNumeric() && r->isNumeric()) {
      if (const Type *common = commonNumericType(l, r)) {
        bin->lhs = convert(bin->lhs, common);
        bin->rhs = convert(bin->rhs, common);
        return bin;
      }
    }
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // Only the low bits of both sides matter: the count is masked. A literal
    // takes no type from the context, as for `&` and `|`: `-1 >>> 0` is the
    // 4294967295 of JavaScript where a long is expected too, not a 64-bit
    // shift of a long -1.
    bin->lhs = apparent(checkExpr(bin->lhs, nullptr, /*wraps=*/true));
    bin->rhs = apparent(checkExpr(bin->rhs, nullptr, /*wraps=*/true));
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    if (!l->isNumeric() || !r->isNumeric()) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    if (l->isFloat()) {
      // JavaScript's `x >>> n` is unsigned, and an int cannot hold it.
      if (bin->op == BinaryOp::UShr) {
        diags_.error(bin->lhs->loc(), "operator '>>>' cannot be applied to type 'double'; "
                                      "convert it to int with 'x | 0' first");
        return bin;
      }
      bin->lhs = toInt32(bin->lhs);
      l = Type::intTy();
    }
    if (r->isFloat())
      bin->rhs = toInt32(bin->rhs);
    // The result has the type of the left side. The shift count is masked to
    // the bit width, so converting it to that type loses nothing.
    bin->rhs = convert(bin->rhs, l);
    bin->type = l;
    // JavaScript's `x >>> n` is unsigned. Shifted by 1 to 31 bits an int
    // still fits in an int, but `-1 >>> 0` is 4294967295, so with a count
    // that may be 0 the result is a long, unless only its low 32 bits matter
    // (`(h >>> 0) | 0`) or it is a constant that fits.
    if (bin->op == BinaryOp::UShr && l->isInt() && !wraps) {
      std::optional<int64_t> count = intConstant(bin->rhs);
      if (!count || (*count & 31) == 0) {
        std::optional<int64_t> value = intConstant(bin->lhs);
        if (!value || static_cast<uint32_t>(*value) > uint32_t(std::numeric_limits<int32_t>::max()))
          bin->type = Type::longTy();
      }
    }
    foldBinary(bin, wraps);
    return bin;
  }

  default: { // arithmetic and bitwise
    bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                   bin->op == BinaryOp::BitXor;
    // The operands of `&`, `|` and `^` take no type from the context: they
    // become 32-bit integers the way JavaScript makes them, and `x | 0` stays
    // ToInt32 wherever it is (`2166136261 | 0` cuts the literal to 32 bits,
    // `Math.floor(x) | 0` wraps around).
    const Type *hint = nullptr;
    if (expected && !bitwise && (expected->isNumeric() || expected->isDecimal()))
      hint = expected;
    // A decimal power takes an integer exponent, so the exponent takes no
    // decimal from the context.
    bool pow = bin->op == BinaryOp::Pow;
    const Type *rhsHint = pow && hint && hint->isDecimal() ? nullptr : hint;
    // The low bits of a sum, difference, product or power depend only on the
    // low bits of the operands; not so for a quotient.
    bool operandsWrap = bitwise || (wraps && bin->op != BinaryOp::Div && bin->op != BinaryOp::Rem);
    // In the chain `a + b + c` the last `+` is checked first; it reports
    // joining numbers with strings for the whole chain (see reportMixedConcat).
    BinaryExpr *chain = std::exchange(concatChain_, nullptr);
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed = chainEnd ? std::exchange(mixedConcat_, nullptr) : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add)
      concatChain_ = chain ? chain : bin;
    // Literals next to a decimal are decimals (see sema_decimal.cpp): a
    // literal left side waits for the right one.
    bool literals = !bitwise && !pow;
    if (literals && isLiteralTree(bin->lhs) && !isLiteralTree(bin->rhs)) {
      BinaryExpr *pending = std::exchange(concatChain_, nullptr);
      bin->rhs = apparent(checkExpr(bin->rhs, rhsHint, operandsWrap));
      concatChain_ = pending;
      bin->lhs = apparent(checkExpr(bin->lhs, literalHint(bin->rhs, hint), operandsWrap));
      concatChain_ = nullptr;
    } else {
      bin->lhs = apparent(checkExpr(bin->lhs, hint, operandsWrap));
      concatChain_ = nullptr;
      if (literals && isLiteralTree(bin->rhs))
        rhsHint = literalHint(bin->lhs, rhsHint);
      bin->rhs = apparent(checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    neverOperands(bin);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (bin->op == BinaryOp::Add) {
      // JavaScript joins a number or a boolean with a string as text: `10 + "1"`
      // is "101", which is rarely meant where it is not written as a template.
      auto joinsAsText = [](const Type *type) {
        const Type *base = type->nonOptional();
        return base->isNumeric() || base->isDecimal() || base->isBool();
      };
      if (!mixedConcat_ && ((l->isString() && joinsAsText(r)) || (r->isString() && joinsAsText(l))))
        mixedConcat_ = bin;
      if (chainEnd) {
        if (mixedConcat_)
          reportMixedConcat(bin, mixedConcat_);
        mixedConcat_ = outerMixed;
      }
    }
    bin->type = Type::error();
    if (l->isError() || r->isError())
      return bin;
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      // As in JavaScript, `+` concatenates when either side is a string.
      bin->lhs = toStringOperand(bin->lhs);
      bin->rhs = toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (Expr *dates = checkDateOperands(bin))
      return dates;
    if (!bitwise && (l->isDecimal() || r->isDecimal()))
      return checkDecimalArithmetic(bin);
    if (bitwise && l->isNumeric() && r->isNumeric() && (l->isFloat() || r->isFloat() || l != r)) {
      // As in JavaScript, both sides become 32-bit integers first, so `x | 0`
      // drops the fraction of a double exactly the way JavaScript does. A
      // long next to an int keeps its low 32 bits the same way: `x | 0` is
      // JavaScript's ToInt32 for a long too. Only two longs work on 64 bits.
      bin->lhs = toInt32(bin->lhs);
      bin->rhs = toInt32(bin->rhs);
      bin->type = Type::intTy();
      foldBinary(bin, wraps);
      return bin;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? commonNumericType(l, r) : nullptr;
    if (!common) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = convert(bin->lhs, common);
    bin->rhs = convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger())
      return bin;
    // An integer to a negative power is a fraction (as with `/`, integers
    // stay integers); a negative constant exponent is surely a mistake.
    if (bin->op == BinaryOp::Pow) {
      if (std::optional<int64_t> exponent = intConstant(bin->rhs); exponent && *exponent < 0) {
        diags_.error(bin->rhs->loc(), "'" + common->name() +
                                          "' raised to a negative power is a fraction, which an "
                                          "integer cannot hold; use a double, e.g. '2.0 ** -1'");
        return bin;
      }
    }
    checkDivisor(bin->op, bin->rhs);
    foldBinary(bin, wraps);
    return bin;
  }
  }
}

// `==` and `!=` (the only equality operators; there is no null to convert). Any
// value can be compared with undefined; `T | undefined` compares with `T`.
Expr *Sema::checkEquality(BinaryExpr *bin) {
  if (Expr *null = checkNullEquality(bin))
    return null;
  checkComparisonOperands(bin);
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::boolTy();
  // typeof never gives "bigint": there is none.
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf && typeOf->op == UnaryOp::TypeOf && text && text->value == u"bigint")
      reportBigInt(text->loc(), "'typeof x == \"bigint\"'");
  }
  if (l->isError() || r->isError())
    return bin;
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined())
    return bin;
  if (l->nonOptional()->isJson() || r->nonOptional()->isJson())
    return checkJsonEquality(bin);
  // In JavaScript two Results are two objects, never equal; their values
  // cannot be compared for them.
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    diags_.error(bin->loc(), llvm::Twine("Results cannot be compared with '") +
                                 opSpelling(bin->op) +
                                 "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
                                 "objects; compare what they hold, with 'isOk()', 'match' or "
                                 "'value'");
    return bin;
  }
  // TypeScript compares a value of a type parameter with anything; each
  // instance checks the types it has.
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam())
    return bin;

  const Type *common = comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal())
      reportDecimalMix(bin);
    else
      reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (common->nonOptional()->isFunction())
    file_->comparesFunctions = true;
  bin->lhs = convert(bin->lhs, common);
  bin->rhs = convert(bin->rhs, common);
  return bin;
}

// `a ?? b`: b when a is undefined. When a cannot be undefined (for example an
// index Sema proved in range), b is checked but never evaluated.
Expr *Sema::checkNullish(BinaryExpr *bin, const Type *expected) {
  bin->lhs = checkExpr(bin->lhs, expected);
  // `s.match(re) ?? []`: the match is the array of its captures there, as
  // it is an array in JavaScript.
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs))
    bin->lhs = matchItems(bin->lhs);
  const Type *l = bin->lhs->type;
  Flow afterLhs = flow_;
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = checkExpr(bin->rhs, hint);
  flow_ = join(afterLhs, flow_); // the right side may not run
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (trial_ && l->isError() != r->isError()) // as in checkConditional
      bin->type = l->isError() ? r : l->nonOptional();
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  if (l->isUndefined()) {
    bin->type = r;
    return bin;
  }
  const Type *base =
      r->isUndefined() ? l->nonOptional() : unionUnify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs = convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = convert(bin->rhs, result);
  bin->type = result;
  return bin;
}

const Type *Sema::checkAssignTarget(Expr *&target, bool compound) {
  if (auto *name = dyn_cast<NameExpr>(target)) {
    VarSymbol *var = checkVarTarget(name);
    return var ? var->type : Type::error();
  }

  if (auto *index = dyn_cast<IndexExpr>(target)) {
    index->object = checkExpr(index->object);
    const Type *type = index->object->type;
    index->type = Type::error();
    if (checkGenericIndex(index))
      return index->type;
    if (type->nonOptional()->isJson())
      return checkJsonTarget(index);
    if (type->isDict())
      return checkDictTarget(index, compound);
    // An element of a typed array; the index and the value are checked once
    // the value is known (see checkBytesStore).
    if (type->isTypedArray()) {
      index->index = checkPosition(index->index);
      index->type = type->elementType();
      return index->type;
    }
    if (type->isBytes()) {
      index->index = checkExpr(index->index);
      diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed; " +
                                     (type->bytes() == Type::Bytes::DataView
                                          ? "write its bytes with setUint8(i, value) and the like"
                                          : "write its bytes through a view, e.g. 'new "
                                            "Uint8Array(buffer)[i] = value'"));
      return Type::error();
    }
    // `o[key] = v` with a constant key assigns the field it names.
    if (type->isObject()) {
      Expr *checked = checkKeyedField(index, /*write=*/true);
      if (index->keyed) // a field chosen by a key of literals
        return index->type;
      auto *member = dyn_cast<MemberExpr>(checked);
      if (!member)
        return Type::error();
      target = member;
      return checkFieldTarget(member, compound);
    }
    if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
      index->index = checkExpr(index->index);
      diags_.error(index->object->loc(),
                   "the value may be undefined; check it before assigning to it");
      return Type::error();
    }
    index->index = checkPosition(index->index);
    if (type->isError())
      return type;
    if (type->isArray()) {
      index->type = type->inner();
      return index->type;
    }
    if (type->isString())
      diags_.error(index->loc(), "strings are immutable; a character cannot be assigned to");
    else if (type->isOptional() && type->inner()->isArray())
      diags_.error(index->object->loc(), "the array may be undefined; check it first");
    else
      diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    return Type::error();
  }

  if (auto *member = dyn_cast<MemberExpr>(target)) {
    auto *name = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = name ? lookup(name->name).cls : nullptr)
      return checkStaticTarget(target, cls);
    if (name && name->name == "this")
      thisMember_ = member->name;
    member->object = checkExpr(member->object);
    thisMember_ = {};
    const Type *type = member->object->type;
    member->type = Type::error();
    if (type->isError())
      return type;
    if (type->nonOptional()->isJson())
      return checkJsonTarget(member);
    // A static field through a class as a value known when compiling.
    if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef());
        cls && cls->type && isa<NameExpr>(member->object))
      return checkStaticTarget(target, cls);
    if (type->isDict())
      return checkDictTarget(member, compound);
    if (type->isOptional() && type->inner()->isDict()) {
      diags_.error(member->object->loc(), "the value may be undefined; check it before assigning "
                                          "to '" + member->name + "'");
      return Type::error();
    }
    // A field of every member of a union: through a type they all are, or
    // in each, where it has one type (sema_union.cpp).
    if (type->isUnion()) {
      if (const Type *common = commonBaseWith(type, member->name)) {
        member->object = convert(member->object, common);
        return checkFieldTarget(member, compound);
      }
      return checkUnionFieldTarget(member);
    }
    if (type->nonOptional()->isBytes()) {
      diags_.error(member->loc(),
                   member->name == "length" || member->name == "byteLength"
                       ? "the length of '" + type->nonOptional()->name().str() +
                             "' cannot change; make a new one of the length wanted"
                       : "property '" + member->name.str() + "' of type '" +
                             type->nonOptional()->name().str() + "' cannot be assigned to");
      return Type::error();
    }
    if (type->isRuntimeObject())
      return checkRuntimeTarget(member, compound);
    if (type->isObject() || (type->isOptional() && type->inner()->isObject()))
      return checkFieldTarget(member, compound);
    if (type->isArray() && member->name == "length") {
      if (compound) {
        diags_.error(member->loc(), "only '=' can change the length of an array");
        return Type::error();
      }
      member->type = Type::intTy();
      return Type::longTy(); // a new length is computed as long, like positions
    }
    if (type->isString() && member->name == "length")
      diags_.error(member->loc(), "the length of a string cannot be changed");
    else
      diags_.error(member->loc(), "property '" + member->name + "' of type '" + type->name() +
                                      "' cannot be assigned to");
    return Type::error();
  }

  if (isa<ArrayLitExpr>(target))
    diags_.error(target->loc(), "destructuring assignment can only be a statement, e.g. '[a, b] = [b, a]'");
  else if (isa<ObjectLitExpr>(target))
    diags_.error(target->loc(),
                 "destructuring assignment can only be a statement, e.g. '({ a, b } = o)'");
  else
    diags_.error(target->loc(), "only variables, fields, array elements and 'length' can be assigned to");
  return Type::error();
}

VarSymbol *Sema::checkVarTarget(NameExpr *name) {
  if (name->name == "this") {
    diags_.error(name->loc(), "cannot assign to 'this'");
    return nullptr;
  }
  ScopeEntry entry = lookupValue(name->name);
  if (!entry.var) {
    checkName(name); // reports the error
    return nullptr;
  }
  name->var = entry.var;
  name->type = varType(entry.var);
  checkVisible(name, entry.var);
  if (entry.var->isConst)
    diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is a " +
                                  (entry.var->function ? "function" : "constant"));
  else
    reportImportAssign(name, entry.var);
  entry.var->reassigned = true;
  noteUse(entry.var, name->loc());
  return entry.var;
}

// After the value of `a[i] = v` is known: the store happens then, so the
// index must be in range at that point.
static const char kWriteOutOfRange[] =
    "cannot prove that the index is within the array (0 <= index <= length); writing further "
    "would leave holes. Check the index first, e.g. 'if (i < a.length)', or use push()";

Expr *Sema::checkAssign(AssignExpr *assign) {
  // The `o.x = v` of `o.x ??= v` (and of `||=`, `&&=`), not one inside v.
  bool logical = std::exchange(logicalAssign_, false);
  // `env.NAME = v` of node:process is `process.env.NAME = v`; a name
  // imported from a built-in module cannot be assigned to.
  if (auto *member = dyn_cast<MemberExpr>(assign->target))
    member->object = processAlias(member->object);
  else if (auto *index = dyn_cast<IndexExpr>(assign->target))
    index->object = processAlias(index->object);
  else if (auto *name = dyn_cast<NameExpr>(assign->target); name && nodeName(name->name)) {
    diags_.error(name->loc(), "cannot assign to '" + name->name + "' because it is imported");
    assign->value = checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  if (Expr *exitCode = checkProcessAssign(assign))
    return exitCode;
  bool compound = assign->op.has_value();
  BinaryOp op = compound ? *assign->op : BinaryOp::Add;

  if (compound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd))
    return checkLogicalAssign(assign);

  if (compound && op == BinaryOp::Nullish) {
    // `r.key ??= v` of a Record (known by its variable, or process.env): a
    // key that may be missing.
    if (auto *member = dyn_cast<MemberExpr>(assign->target); member && !member->optional) {
      auto *name = dyn_cast<NameExpr>(member->object);
      VarSymbol *var = name ? lookupValue(name->name).var : nullptr;
      if ((var && var->type && var->type->isRecord()) || isProcessMember(member->object, "env")) {
        Expr *key = keyLiteral(member->loc(), member->name);
        auto *index = ctx_.make<IndexExpr>(member->loc(), member->object, key);
        assign->target = index;
        if (Expr *set = checkRecordNullishAssign(assign, index))
          return set;
      }
    }
    if (isa<MemberExpr>(assign->target))
      return checkFieldLogicalAssign(assign);
    if (auto *index = dyn_cast<IndexExpr>(assign->target))
      if (Expr *set = checkRecordNullishAssign(assign, index))
        return set;
    // `x ??= v`: assigns only if x is undefined.
    auto *name = dyn_cast<NameExpr>(assign->target);
    VarSymbol *var = name ? checkVarTarget(name) : nullptr;
    if (!name)
      diags_.error(assign->target->loc(), "only variables can be the target of '??='");
    const Type *type = var ? var->type : Type::error();
    assign->type = type;
    if (!type->isOptional() && !type->isError()) {
      diags_.error(assign->loc(), "'??=' needs a variable that may be undefined; '" + name->name +
                                      "' has type '" + type->name() + "'");
      type = Type::error();
    }
    Flow before = flow_;
    assign->value = checkExpr(assign->value, type->isError() ? nullptr : type->inner());
    const Type *valueType = assign->value->type;
    flow_ = join(before, flow_);
    if (type->isError())
      return assign;
    bool definite = !valueType->isOptional() && !valueType->isUndefined();
    assign->value = coerce(assign->value, definite ? type->inner() : type);
    if (var) {
      if (definite)
        setDefined(var, true);
      assign->type = definite ? type->inner() : type;
    }
    return assign;
  }

  // `x += n` of a variable of a union that a check narrowed to a member:
  // `x = x + n`, the sum of the member, which goes back into the union.
  if (auto *target = dyn_cast<NameExpr>(assign->target); target && compound) {
    const VarSymbol *var = lookup(target->name).var;
    if (var && var->type && var->type->nonOptional()->isUnion() && narrowedType(var)) {
      auto *read = ctx_.make<NameExpr>(target->loc(), target->name);
      assign->value = ctx_.make<BinaryExpr>(assign->loc(), op, read, assign->value);
      assign->op.reset();
      compound = false;
    }
  }

  const Type *type = checkAssignTarget(assign->target, compound);
  // `o.x = v` of an accessor, a call of its setter (sema_library.cpp).
  if (pendingSetter_)
    return checkSetterCall(assign);
  assign->type = type;
  auto *index = dyn_cast<IndexExpr>(assign->target);
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *name = dyn_cast<NameExpr>(assign->target);
  const VarSymbol *var = name ? name->var : nullptr;
  // `a[i] op= v` reads the element before v is evaluated.
  bool reportedRange = false;
  if (index && compound && index->object->type->isArray() &&
      !provesInBounds(index->object, index->index, /*allowEnd=*/false)) {
    diags_.error(index->loc(), "cannot prove that the index is within the array; check it first, "
                               "e.g. 'if (i < a.length)'");
    reportedRange = true;
  }

  bool definite = false; // the value assigned is not undefined
  if (!type) {
    // `this.x = value` in the initializers, where x has the type of the
    // value, as a variable without a type does.
    Expr *value = checkExpr(assign->value);
    type = value->type;
    if (type->isVoid() || type->isUndefined()) {
      diags_.error(value->loc(), "cannot infer the type of field '" + member->name +
                                     "' from this value; declare it, e.g. '" + member->name +
                                     ": int | undefined = undefined'");
      type = Type::error();
    }
    ClassDecl *cls = classOf(member->object->type);
    for (ClassField &f : cls->fields)
      if (f.name == member->name && !f.isStatic)
        f.resolved = type;
    assign->value = value;
    assign->type = member->type = type;
    definite = !type->isOptional() && !type->isUndefined();
  } else if (!compound) {
    if (auto *fe = dyn_cast<FunctionExpr>(assign->value); fe && name && fe->fn->name.empty())
      fe->fn->name = name->name; // as JavaScript names it
    Expr *value = checkExpr(assign->value, type->isError() ? nullptr : type);
    assign->value = coerce(value, type);
    definite = !value->type->isOptional() && !value->type->isUndefined();
    if (var)
      assigned(var, value);
    if (var && type->isError() && !untyped_.empty())
      assignedUntyped(var, value->type);
  } else {
    std::string spelling = std::string(opSpelling(op)) + "=";
    if (type->isString()) {
      assign->value = checkExpr(assign->value);
      const Type *valueType = assign->value->type;
      if (op != BinaryOp::Add) {
        if (!valueType->isError())
          reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
      } else if (valueType->nonOptional()->isNumeric() || valueType->nonOptional()->isBool()) {
        // As with `s + 5` (see reportMixedConcat).
        std::string message = "operator '+=' cannot be applied to types 'string' and '" +
                              valueType->name().str() + "' (JavaScript would join them as text)";
        auto [target, value] = operandTexts(assign->source, assign->loc(), 2);
        if (target.empty())
          message += "; convert the value with String()";
        else
          message += ("; write '" + target + " += String(" + withoutParens(value) + ")'").str();
        diags_.error(assign->loc(), message);
      } else {
        assign->value = toStringOperand(assign->value);
      }
      if (var)
        flow_.facts.forget(var);
    } else {
      bool shift = op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
      bool bitwise = op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
      bool integerOnly = shift || bitwise;
      // As with `x | v`, v takes no type from x (see checkBinary), and the
      // exponent of a decimal power is an integer.
      bool decimalPow = type->isDecimal() && op == BinaryOp::Pow;
      assign->value = checkExpr(assign->value, integerOnly || decimalPow ? nullptr : type,
                                /*wraps=*/integerOnly);
      const Type *valueType = assign->value->type;
      if (type->isDecimal() && !integerOnly && !valueType->isError()) {
        checkDecimalAssign(assign, op);
      } else if (!type->isError() && !valueType->isError()) {
        if (integerOnly && valueType->isFloat())
          assign->value = toInt32(assign->value); // as with `x | v`
        if (integerOnly && type->isFloat() && valueType->isNumeric() && op != BinaryOp::UShr) {
          // `x |= v` on a double works on 32-bit integers, as in JavaScript;
          // the result goes back into x as a double.
          assign->value = toInt32(assign->value);
        } else if (integerOnly ? !type->isInteger() : !type->isNumeric()) {
          reportOperandTypes(assign->loc(), spelling.c_str(), type, valueType);
        } else if (bitwise && assign->value->type->isInteger() && assign->value->type != type) {
          // As with `x | v`: a long next to an int keeps its low 32 bits, and
          // the operator works on 32-bit integers; a long x gets the result
          // back as a long.
          assign->value = toInt32(assign->value);
        } else {
          // `x op= v` must produce a value of x's type, so v converts to it.
          assign->value = shift ? convert(assign->value, type) : coerce(assign->value, type);
          if (type->isInteger())
            checkDivisor(op, assign->value);
          // `x >>>= n` is `x = x >>> n`, a long unless the count is 1 to 31
          // (see checkBinary), which an int x cannot hold.
          if (op == BinaryOp::UShr && type->isInt()) {
            std::optional<int64_t> count = intConstant(assign->value);
            if (!count || (*count & 31) == 0) {
              auto [target, value] = operandTexts(assign->source, assign->loc(), 4);
              std::string message = std::string("the result of '>>>=' may not fit in 'int'; ") +
                                    kUnsignedShift;
              if (!target.empty())
                message += (", e.g. 'const u = " + target + " >>> " + value + "'").str();
              diags_.error(assign->loc(), message + ", or shift by 1 to 31 bits");
            }
          }
        }
      }
      if (var) {
        std::optional<Linear> delta = linear(assign->value);
        if ((op == BinaryOp::Add || op == BinaryOp::Sub) && delta && delta->term.isZero())
          incremented(var, op == BinaryOp::Add ? delta->offset : -delta->offset);
        else
          flow_.facts.forget(var);
      }
    }
  }

  if (Expr *set = recordAssign(assign))
    return set;
  if (index && index->object->type->isTypedArray())
    checkBytesStore(index, assign->value, assign->op, assign->loc());
  if (member && !type->isError())
    if (Expr *set = onabortAssign(assign, member, logical))
      return set;
  if (index && index->object->type->isArray()) {
    // The store happens after the value is evaluated, so prove it here.
    bool inRange = provesInBounds(index->object, index->index, /*allowEnd=*/false);
    if (!inRange && !reportedRange &&
        (compound || !provesInBounds(index->object, index->index, /*allowEnd=*/true)))
      diags_.error(index->loc(), kWriteOutOfRange);
    index->mayAppend = !inRange;
    if (!inRange)
      arrayResized(index->object->type, /*grew=*/true, /*shrank=*/false);
  }
  if (member && member->object->type->isArray()) {
    if (std::optional<Linear> length = linear(assign->value);
        length && length->term.isZero() && length->offset < 0)
      diags_.error(assign->value->loc(), "an array length cannot be negative");
    arrayResized(member->object->type, /*grew=*/true, /*shrank=*/true);
    if (std::optional<Linear> self = lengthOf(member->object); self && !self->term.isZero())
      if (std::optional<Linear> length = linear(assign->value))
        flow_.facts.addEqual(self->term, length->term, length->offset);
  }
  if (member && member->object->type->isObject()) {
    // The store happens now; until here the field kept its old value.
    fieldsAssigned(member->name);
    if (std::optional<FieldPath> path = fieldPath(member); path && definite)
      defineField(flow_, *path);
    if (isInitThis(member->object) && !llvm::is_contained(flow_.setFields, member->name))
      flow_.setFields.push_back(member->name);
  }
  return assign;
}

// A copy of a name, a literal index or a chain of fields of a name (`o.a.b`),
// which reads the same value again; null for anything else.
static Expr *copyChain(AstContext &ctx, const Expr *e) {
  if (auto *name = dyn_cast<NameExpr>(e))
    return ctx.make<NameExpr>(name->loc(), name->name);
  if (auto *lit = dyn_cast<IntLitExpr>(e))
    return ctx.make<IntLitExpr>(lit->loc(), lit->value);
  if (auto *lit = dyn_cast<StringLitExpr>(e))
    return ctx.make<StringLitExpr>(lit->loc(), lit->value);
  if (auto *member = dyn_cast<MemberExpr>(e); member && !member->optional)
    if (Expr *object = copyChain(ctx, member->object))
      return ctx.make<MemberExpr>(member->loc(), object, member->name);
  return nullptr;
}

// `x ||= v` is `x || (x = v)` and `x &&= v` is `x && (x = v)`: v is evaluated
// and assigned only when x does not decide the result. x is read twice, so it
// must be a variable, a field of one or an element at a simple index. Its
// truth is tested as in a condition.
Expr *Sema::checkLogicalAssign(AssignExpr *assign) {
  const char *op = assign->op == BinaryOp::LogOr ? "||=" : "&&=";
  Expr *read = nullptr;
  if (isa<NameExpr>(assign->target) || isa<MemberExpr>(assign->target)) {
    read = copyChain(ctx_, assign->target);
  } else if (auto *index = dyn_cast<IndexExpr>(assign->target); index && !index->optional) {
    Expr *object = copyChain(ctx_, index->object), *position = copyChain(ctx_, index->index);
    if (object && position)
      read = ctx_.make<IndexExpr>(index->loc(), object, position);
  }
  if (!read) {
    diags_.error(assign->target->loc(), llvm::Twine("'") + op +
                                            "' needs a variable, a field or an array element at a "
                                            "simple index, such as 'a[i]'");
    assign->value = checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = ctx_.make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  Flow whenTrue, whenFalse;
  bool outer = std::exchange(logicalAssign_, true);
  checkLogical(bin, nullptr, whenTrue, whenFalse, /*tested=*/false,
               bin->op == BinaryOp::LogAnd ? TruthUse::AndAssign : TruthUse::OrAssign);
  logicalAssign_ = outer;
  flow_ = join(whenTrue, whenFalse);
  return bin;
}

// `o.f ??= v` is `o.f ?? (o.f = v)`: o is read twice, so it must be a
// variable or a field of one.
Expr *Sema::checkFieldLogicalAssign(AssignExpr *assign) {
  Expr *read = copyChain(ctx_, assign->target);
  if (!read) {
    diags_.error(assign->target->loc(), "'??=' needs a variable or a field of one, such as 'o.x'");
    assign->value = checkExpr(assign->value);
    assign->type = Type::error();
    return assign;
  }
  auto *set = ctx_.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *bin = ctx_.make<BinaryExpr>(assign->loc(), BinaryOp::Nullish, read, set);
  bool outer = std::exchange(logicalAssign_, true);
  Expr *result = checkNullish(bin, nullptr);
  logicalAssign_ = outer;
  auto *member = cast<MemberExpr>(set->target);
  const Type *type = member->type;
  if (type && !type->isOptional() && !type->isError()) {
    diags_.error(assign->loc(), "'??=' needs a field that may be undefined; '" + member->name +
                                    "' has type '" + type->name() + "'");
    return result;
  }
  // The field holds a value now, unless the one assigned may be undefined.
  const Type *valueType = set->value->type;
  if (auto *conv = dyn_cast<CastExpr>(set->value); conv && conv->isImplicit())
    valueType = conv->operand->type;
  if (!valueType->isOptional() && !valueType->isUndefined())
    if (std::optional<FieldPath> path = fieldPath(member))
      defineField(flow_, *path);
  return result;
}

Expr *Sema::checkUpdate(UpdateExpr *update) {
  update->type = Type::error();
  const Type *type = checkAssignTarget(update->target, /*compound=*/true);
  if (type->isError())
    return update;
  if (!type->isNumeric() && !type->isDecimal()) {
    diags_.error(update->loc(), llvm::Twine("operator '") + (update->increment ? "++" : "--") +
                                    "' cannot be applied to type '" + type->name() + "'");
    return update;
  }
  update->type = type;
  if (auto *name = dyn_cast<NameExpr>(update->target)) {
    if (name->var)
      incremented(name->var, update->increment ? 1 : -1);
  } else if (auto *index = dyn_cast<IndexExpr>(update->target); index && !index->keyed) {
    if (index->object->type->isTypedArray())
      checkBytesStore(index, nullptr, update->increment ? BinaryOp::Add : BinaryOp::Sub,
                      update->loc());
    else if (!provesInBounds(index->object, index->index, /*allowEnd=*/false))
      diags_.error(index->loc(), "cannot prove that the index is within the array; check it "
                                 "first, e.g. 'if (i < a.length)'");
  } else if (auto *member = dyn_cast<MemberExpr>(update->target)) {
    fieldsAssigned(member->name);
  }
  return update;
}

Expr *Sema::checkCall(CallExpr *call, const Type *expected) {
  call->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  // `...args` of an argument list: its parameters.
  expandListSpreads(call->args, call->spread);
  // `x[Symbol.iterator]()`: the method of that name (docs/iterators.md,
  // section 5).
  if (auto *index = dyn_cast<IndexExpr>(call->callee); index && !index->optional)
    if (auto *key = dyn_cast<MemberExpr>(index->index);
        key && (key->name == "iterator" || key->name == "asyncIterator") &&
        isGlobalName(key->object, "Symbol"))
      call->callee = ctx_.make<MemberExpr>(
          index->loc(), index->object,
          key->name == "iterator" ? "[Symbol.iterator]" : "[Symbol.asyncIterator]");

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && name->name == "super")
    return checkSuperCall(call);
  if (Expr *node = checkNodeCall(call, expected))
    return node;
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && !call->optional) {
    if (isGlobalName(member->object, "JSON"))
      return checkJsonStatic(call, member);
    if (isGlobalName(member->object, "Promise")) // Promise.withResolvers<T>() too
      return checkPromiseStatic(call, member, expected);
    if (isGlobalName(member->object, "Reflect"))
      return checkReflectCall(call, member, expected);
    // Type arguments, for a generic method of a class; those of anything else
    // are reported once the receiver is known.
    auto rejectTypeArgs = [&] {
      if (member->typeArgs.empty())
        return;
      diags_.error(member->loc(), "type arguments can be given only to a generic function and to "
                                  "'JSON.parse<T>'");
      member->typeArgs.clear();
    };
    if (auto *object = dyn_cast<NameExpr>(member->object); !object || !lookup(object->name).cls) {
      static const llvm::StringLiteral globals[] = {"console", "Math",  "decimal", "String", "Number",
                                                    "Object",  "Result", "Iterator", "Array", "RegExp",
                                                    "Intl",    "Map",   "process", "Date",  "Symbol"};
      if (object && llvm::any_of(globals, [&](llvm::StringRef g) { return isGlobalName(object, g); }))
        rejectTypeArgs();
    }
    if (auto *object = dyn_cast<NameExpr>(member->object); object && object->name == "super")
      return checkSuperMethodCall(call, member);
    if (isGlobalName(member->object, "console")) {
      // info and debug print to stdout, as log does; error and warn the same
      // way to stderr.
      bool toStderr = member->name == "error" || member->name == "warn";
      if (toStderr || member->name == "log" || member->name == "info" || member->name == "debug") {
        checkConsoleLog(call);
        if (toStderr)
          call->builtin = Builtin::ConsoleError;
        return call;
      }
      diags_.error(member->loc(), "'console." + member->name +
                                      "' is not supported yet; only 'console.log', 'info', "
                                      "'debug', 'error' and 'warn' are");
      checkArgsLoosely();
      return call;
    }
    if (Expr *io = checkProcessCall(call, member))
      return io;
    if (isGlobalName(member->object, "Math"))
      return checkMathCall(call, member, expected);
    if (isGlobalName(member->object, "decimal"))
      return checkDecimalStatic(call, member);
    if (isGlobalName(member->object, "String") || isGlobalName(member->object, "Number")) {
      if (rejectSpread(call))
        return call;
      return checkStaticCall(call, member, expected);
    }
    if (isGlobalName(member->object, "Object"))
      return checkObjectCall(call, member, expected);
    if (Expr *result = checkResultAsyncStatic(call, member, expected)) // sema_async.cpp
      return result;
    if (Expr *result = checkResultStatic(call, member))
      return result;
    auto *object = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = object ? lookup(object->name).cls : nullptr)
      return checkClassStaticCall(call, member, cls, expected);
    if (object && object->name == "this")
      thisMember_ = member->name;
    if (isGlobalName(member->object, "Iterator"))
      return checkIteratorFrom(call, member);
    if (isGlobalName(member->object, "Array") && member->name == "isArray")
      return checkArrayIsArray(call, member);
    if (isGlobalName(member->object, "Array"))
      return checkArrayFrom(call, member);
    if (isGlobalName(member->object, "RegExp"))
      return checkRegExpStatic(call, member);
    if (isGlobalName(member->object, "Intl"))
      return checkIntlCall(call, member);
    if (auto *inner = dyn_cast<MemberExpr>(member->object);
        inner && isGlobalName(inner->object, "Intl"))
      return checkIntlStatic(call, member);
    if (isGlobalName(member->object, "Map"))
      return checkMapStatic(call, member);
    if (isGlobalName(member->object, "crypto"))
      return checkWebCryptoCall(call, member);
    if (Expr *bytes = checkBytesStatic(call, member))
      return bytes;
    // A consumer of a lazy chain runs it where it is written.
    static const llvm::StringLiteral consumers[] = {"toArray", "forEach", "reduce",
                                                    "some",    "every",   "find"};
    if (llvm::is_contained(consumers, member->name))
      markFused(member->object);
    member->object = apparent(checkExpr(member->object));
    thisMember_ = {};
    if (member->optional)
      member->object = chainLink(member->object);
    const Type *receiver = member->object->type;
    if (!receiver->isClass() && !receiver->isUnion())
      rejectTypeArgs();
    if (receiver->nonOptional()->isJson())
      return reportJsonMethod(call, member);
    if (Expr *checked = checkUnknownMethod(call, member))
      return checked;
    if (receiver->isFunction() && receiver->hasThis())
      return checkThisCall(call, member);
    if (receiver->isListParam())
      return checkListMethod(call, member);
    if (receiver->isResult())
      return checkResultMethod(call, member, expected);
    if (receiver->isRegExp())
      return checkRegexMethod(call, member);
    if (receiver->isDate())
      return checkDateMethod(call, member);
    if (receiver->isDateFormat())
      return checkDateFormatMethod(call, member);
    if (receiver->isPromise())
      return checkPromiseMethod(call, member, expected);
    if (receiver->isTimer())
      return checkTimerMethod(call, member);
    if (receiver->isCrypto())
      return checkCryptoMethod(call, member);
    if (receiver->isWeakRef())
      return checkWeakRefMethod(call, member);
    if (receiver->isBytes())
      return checkBytesMethod(call, member, expected);
    if (receiver->isIntl())
      return checkIntlMethod(call, member);
    if (receiver->isRuntimeObject())
      return checkRuntimeMethod(call, member, expected);
    // The methods of arrays work on a match: its captures are an array.
    if (receiver->isMatch()) {
      member->object = matchItems(member->object);
      receiver = member->object->type;
    }
    // A method of every member of a union: through a type they all are, or
    // that of each.
    if (receiver->isUnion()) {
      const Type *common = commonBaseWith(receiver, member->name);
      if (!common) {
        rejectTypeArgs();
        return checkUnionMethodCall(call, member);
      }
      member->object = convert(member->object, common);
      receiver = common;
    }
    if (receiver->isClass())
      return checkMethodCall(call, member, expected);
    rejectTypeArgs();
    if (receiver->isTuple())
      return checkTupleMethod(call, member);
    if (receiver->isObject())
      return checkFieldCall(call, member);
    if (!(receiver->isArray() && member->name == "push") && rejectSpread(call))
      return call;
    if (receiver->isIterator())
      return checkIteratorMethod(call, member);
    if (receiver->isDict())
      return checkDictMethod(call, member);
    if (receiver->isString() && (member->name == "localeCompare" || member->name == "normalize"))
      return checkStringLocale(call, member);
    if ((receiver->isNumeric() || receiver->isDecimal()) && member->name == "toLocaleString")
      return checkNumberLocale(call, member);
    if (receiver->isString())
      return checkStringMethod(call, member);
    if (receiver->isArray())
      return checkArrayMethod(call, member, expected);
    if (receiver->isNumeric())
      return checkNumberMethod(call, member);
    if (receiver->isDecimal())
      return checkDecimalMethod(call, member);
    if (receiver->isBool() && member->name == "toString" && call->args.empty()) {
      // `x.toString()` is `String(x)`.
      call->args.push_back(toStringOperand(member->object));
      call->builtin = Builtin::StringOf;
      call->type = Type::string();
      return call;
    }
    if (receiver->isOptional())
      diags_.error(member->object->loc(), "the value may be undefined; check it before calling '" +
                                              member->name + "'");
    else if (receiver->isFunction() &&
             (member->name == "call" || member->name == "apply" || member->name == "bind"))
      diags_.error(member->loc(), "'" + member->name + "' is not supported: it sets 'this', and a "
                                                         "method taken as a value is bound to its "
                                                         "object already; call the function "
                                                         "directly");
    else if (!receiver->isError())
      diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                      receiver->name() + "'");
    checkArgsLoosely();
    return call;
  }

  if (auto *name = dyn_cast<NameExpr>(call->callee); name && !call->optional) {
    if (Expr *lib = checkLibCall(call, name))
      return lib;
    if (Expr *result = checkResultCall(call, name, expected))
      return result;
    if (Expr *json = checkJsonGlobal(call, name))
      return json;
    if (Expr *task = checkAsyncGlobal(call, name))
      return task;
    if (Expr *builtin = checkGlobalCall(call, name, expected))
      return builtin;
    ScopeEntry entry = lookupValue(name->name);
    // once() of node:events and the like (sema_events.cpp).
    if (entry.func && !entry.var && entry.func->decl &&
        libFunctions_.lookup(entry.func->decl->name) == entry.func->decl)
      if (Expr *events = checkEventsFunction(call, entry.func->decl->name, name->loc()))
        return events;
    if (entry.func && !entry.var)
      return checkDirectCall(call, entry.func, name->loc(), expected); // a top-level function
    // A function declared in a block, or a constant arrow: called directly,
    // with the environment its variable holds.
    VarSymbol *var = entry.var;
    FunctionDecl *known = var ? (var->function ? var->function : var->isConst ? var->lambda : nullptr) : nullptr;
    if (known && known->sym) {
      name->var = var;
      noteUse(var, name->loc());
      name->type = varType(var);
      return checkDirectCall(call, known->sym, name->loc());
    }
    if (!entry.var && !entry.func) {
      // `Function("a", "return a")`, which is a value only as a class.
      if (const char *why = !entry.cls ? whyUnsupported(name->name) : nullptr)
        diags_.error(name->loc(), "'" + name->name + "' is not supported: " + why);
      else if (!checkName(name)->type->isError()) // NaN, Infinity
        diags_.error(name->loc(), "'" + name->name + "' is not a function");
      checkArgsLoosely();
      return call;
    }
  }

  // A call through a function value: `f(x)` with a variable f, `fs[i](x)`,
  // `make()(x)`, `f?.(x)`.
  auto *calleeName = dyn_cast<NameExpr>(call->callee);
  call->callee = checkExpr(call->callee);
  if (call->optional)
    call->callee = chainLink(call->callee);
  const Type *type = call->callee->type;
  if (type->isFunction())
    return checkValueCall(call, type);
  if (type->isOptional() && type->inner()->isFunction())
    diags_.error(call->callee->loc(), "the function may be undefined; check it first, or call it with "
                                      "'?.()', which gives undefined then");
  else if (calleeName && !type->isError())
    diags_.error(call->callee->loc(), "'" + calleeName->name + "' is not a function; it has type '" +
                                          type->name() + "'");
  else if (!type->isError())
    diags_.error(call->callee->loc(), "a value of type '" + type->name() + "' cannot be called");
  checkArgsLoosely();
  return call;
}

Expr *Sema::checkDirectCall(CallExpr *call, FuncSymbol *func, llvm::SMLoc loc, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  llvm::ArrayRef<TypeRef> typeArgs = name ? llvm::ArrayRef<TypeRef>(name->typeArgs) : llvm::ArrayRef<TypeRef>();
  if (func->isGeneric())
    return checkGenericCall(call, func, loc, expected, typeArgs);
  if (!typeArgs.empty())
    diags_.error(loc, "'" + func->name + "' is not a generic function; remove the type arguments");
  resolveParams(func);
  call->func = func;
  call->type = returnType(func);
  return checkArguments(call, func->params, func->minArgs, func->hasRest, loc);
}

Expr *Sema::checkValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    // `f(x)` of `(this: unknown, x: int) => R` would give it no `this`.
    diags_.error(call->callee->loc(), "the function takes its 'this' first, which a plain call "
                                      "does not pass; call it with '.call(obj, ...)' or "
                                      "'.apply(obj, args)'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  return checkArguments(call, type->params(), type->minArgs(), type->hasRest(), call->callee->loc());
}

Expr *Sema::checkArguments(CallExpr *call, llvm::ArrayRef<const Type *> params, size_t minArgs,
                           bool hasRest, llvm::SMLoc loc, const std::vector<bool> *checked) {
  // `...t` of a tuple in a variable, and of an argument list, as checkCall
  // does for the other calls.
  if (!checked)
    expandListSpreads(call->args, call->spread);
  size_t fixed = params.size() - (hasRest ? 1 : 0);
  // In generic code, a rest parameter of a list of types (`...args: A`):
  // spreads of lists (docs/generics.md, section 9).
  if (hasRest && (params.back()->isListParam() || spreadsLists(params.back()))) {
    for (size_t i = 0; i < call->args.size() && i < fixed; ++i)
      if (call->spreads(i) && rejectSpread(call))
        return call;
    if (!checkArity(call, loc, static_cast<unsigned>(minArgs), ~0u))
      return call;
    for (size_t i = 0; i < fixed; ++i) {
      bool given = checked && i < checked->size() && (*checked)[i];
      call->args[i] = given ? coerce(call->args[i], params[i]) : checkArg(call->args[i], params[i]);
    }
    checkListArguments(call, fixed, params.back(), loc, checked);
    return call;
  }
  for (size_t i = 0; i < call->args.size() && i < (hasRest ? fixed : call->args.size()); ++i)
    if (call->spreads(i) && rejectSpread(call))
      return call;
  if (!checkArity(call, loc, static_cast<unsigned>(minArgs),
                  hasRest ? ~0u : static_cast<unsigned>(params.size())))
    return call;
  auto isChecked = [&](size_t i) { return checked && i < checked->size() && (*checked)[i]; };
  Expr *checkedRest = nullptr;
  const Type *listRest = hasRest && (params.back()->isTuple() || params.back()->isListParam())
                             ? params.back()
                             : nullptr;
  if (listRest && call->args.size() == fixed + 1 && call->spreads(fixed)) {
    // A list of parameters, `...args: A` (docs/generics.md, section 9): a
    // list of its type spread there goes as it is.
    Expr *&arg = call->args[fixed];
    if (!isChecked(fixed))
      arg = checkExpr(arg, listRest);
    if (arg->type != listRest && !arg->type->isError() && !listRest->isError())
      diags_.error(arg->loc(), "cannot spread '" + arg->type->name() + "' into '" + listRest->name() + "'");
    checkedRest = arg;
    call->spread.clear();
  } else if (hasRest) {
    // The arguments for the rest parameter become one array, as `f(1, ...a)`
    // passes `[1, ...a]`.
    llvm::SMLoc restLoc = call->args.size() > fixed ? call->args[fixed]->loc() : call->loc();
    auto *rest = ctx_.make<ArrayLitExpr>(restLoc);
    const Type *restType = params.back();
    for (size_t i = fixed; i < call->args.size(); ++i) {
      Expr *arg = call->args[i];
      // Checked already: the array has the elements as they are.
      if (isChecked(i)) {
        checkedRest = rest;
        const Type *element = restType->isArray() ? restType->inner() : restType;
        // A tuple of the arguments for a list of parameters.
        if (listRest && listRest->isTuple() && i - fixed < listRest->fields().size())
          element = listRest->fields()[i - fixed].type;
        if (!call->spreads(i))
          arg = coerce(arg, element);
        else if (arg->type != restType && !arg->type->isError() && !restType->isError())
          diags_.error(arg->loc(), "cannot spread '" + arg->type->name() + "' into '" + restType->name() + "'");
      }
      rest->elements.push_back({arg, call->spreads(i)});
    }
    if (checkedRest && listRest && listRest->isTuple() &&
        rest->elements.size() != listRest->fields().size()) {
      diags_.error(rest->loc(), "expected " + std::to_string(fixed + listRest->fields().size()) +
                                    " arguments, but got " + std::to_string(call->args.size()));
      rest->type = Type::error();
    } else if (checkedRest) {
      rest->type = restType;
    }
    call->args.resize(std::min(call->args.size(), fixed));
    while (call->args.size() < fixed) // optional parameters left out
      call->args.push_back(ctx_.make<UndefinedExpr>(call->loc()));
    call->args.push_back(rest);
    call->spread.clear();
  }
  // Parameters left out get undefined, and with it their default values.
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (call->args[i] == checkedRest)
      continue;
    call->args[i] = isChecked(i) ? coerce(call->args[i], params[i]) : checkArg(call->args[i], params[i]);
  }
  calledFunction();
  return call;
}

bool Sema::checkArity(CallExpr *call, llvm::SMLoc loc, unsigned min, unsigned max) {
  size_t count = call->args.size();
  if (count >= min && count <= max)
    return true;
  if (call->tagged) // the arguments are not written out
    diags_.error(loc, "expected " + argumentCount(min, max) + ", but the tagged template passes " +
                          llvm::Twine(count) + ": the array of its texts" +
                          (count == 1   ? ""
                           : count == 2 ? " and 1 value"
                                        : " and " + std::to_string(count - 1) + " values"));
  else
    diags_.error(loc, "expected " + argumentCount(min, max) + ", but got " + llvm::Twine(count));
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return false;
}

bool Sema::rejectSpread(CallExpr *call) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->spreads(i))
      continue;
    diags_.error(call->args[i]->loc(), "a spread argument can only be passed to a rest parameter, "
                                       "Math.min, Math.max or push");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    call->type = Type::error();
    return true;
  }
  return false;
}

Expr *Sema::checkArg(Expr *arg, const Type *to) {
  return coerce(checkExpr(arg, to->isError() ? nullptr : to), to);
}

Expr *Sema::checkConsoleLog(CallExpr *call) {
  call->builtin = Builtin::ConsoleLog;
  call->type = Type::voidTy();
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    arg = checkExpr(arg);
    if (arg->type->isVoid())
      diags_.error(arg->loc(), "an expression of type 'void' cannot be printed");
    // `console.log(...xs)`: each element as an argument of its own; in
    // generic code, `...args` of a list of types.
    const Type *type = arg->type;
    if (call->spreads(i) && !type->isArray() && !type->isListParam() && !spreadsLists(type) &&
        !type->isError())
      diags_.error(arg->loc(), "only an array can be spread into console.log, found '" + type->name() + "'");
  }
  return call;
}

bool Sema::isGlobalName(const Expr *expr, llvm::StringRef name) const {
  auto *n = dyn_cast<NameExpr>(expr);
  if (!n || n->name != name)
    return false;
  // A name imported from a built-in module hides the global one too
  // (setTimeout of node:timers/promises).
  if (nodeName(name))
    return false;
  ScopeEntry shadow = lookup(name);
  return !shadow.var && !shadow.func && !shadow.cls;
}

Expr *Sema::toStringOperand(Expr *expr) {
  const Type *type = expr->type;
  if (type->isString() || type->isError())
    return expr;
  if (type->isVoid()) {
    diags_.error(expr->loc(), "an expression of type 'void' cannot be converted to a string");
    return expr;
  }
  if (type->isRegExp())
    return regexToString(expr);
  // As in JavaScript: the text of toString(), in local time.
  if (type->isDate())
    return dateToString(expr);
  if (type->isBytes())
    return bytesToString(expr);
  // As TypeScript allows; an instance for objects or functions reports it at
  // the call.
  if (type->nonOptional()->isParam()) {
    noteForced(expr, Type::string());
    return convert(expr, Type::string());
  }
  if (holdsFunctions(type)) {
    // JavaScript would give the source code of the function.
    diags_.error(expr->loc(), "a function cannot be converted to a string; print it with "
                              "console.log, or use its name");
    return expr;
  }
  if (type->nonOptional()->isTuple()) {
    diags_.error(expr->loc(),
                 "a tuple cannot be converted to a string; print it with console.log, or build "
                 "the text from its elements");
    return expr;
  }
  if (holdsResults(type)) {
    diags_.error(expr->loc(), "a Result cannot be converted to a string: JavaScript would give "
                              "'[object Object]'; print it with console.log, or make the text with "
                              "'match'");
    return expr;
  }
  if (ClassDecl *cls = classOf(type->nonOptional());
      cls && !methodsNamed(cls, "toString", /*isStatic=*/false).empty()) {
    // JavaScript would call it: tz calls no code by itself.
    auto *name = dyn_cast<NameExpr>(expr);
    std::string text = name ? name->name.str() : "o";
    diags_.error(expr->loc(), "an object is not converted to a string by calling its toString(): "
                              "tz calls no code by itself; call it, e.g. '${" + text +
                                  ".toString()}'");
    return expr;
  }
  if (const Type *dict = type->nonOptional(); dict->isDict()) {
    const char *kind = dict->isRecord() ? "Record" : dict->isMap() ? "Map" : "Set";
    diags_.error(expr->loc(), std::string("a ") + kind +
                                  " cannot be converted to a string: JavaScript would give "
                                  "'[object " +
                                  (dict->isRecord() ? "Object" : kind) +
                                  "]'; print it with console.log, or build the text from its "
                                  "entries");
    return expr;
  }
  if (type->nonOptional()->isIterator()) {
    diags_.error(expr->loc(), "an iterator cannot be converted to a string: JavaScript would give "
                              "'[object Generator]'; collect its values first, e.g. "
                              "'[...it].join()'");
    return expr;
  }
  if (holdsUnknown(type)) {
    diags_.error(expr->loc(), "an unknown value cannot be converted to a string: it may hold an "
                              "object or a function; check what it holds first, e.g. 'typeof x == "
                              "\"string\"'");
    return expr;
  }
  if (holdsJson(type)) {
    // An object would be "[object Object]" in JavaScript.
    diags_.error(expr->loc(),
                 "a Json cannot be converted to a string: it may hold an object; check what it "
                 "holds first (e.g. 'typeof x == \"string\"'), or write it as JSON with "
                 "'JSON.stringify(x)'");
    return expr;
  }
  // An Error gives "TypeError: message", as JavaScript's Error.prototype.toString.
  if (isErrorType(type))
    return convert(expr, Type::string());
  if (holdsObjects(type)) {
    // JavaScript would give "[object Object]", or call a toString method.
    diags_.error(expr->loc(), "an object cannot be converted to a string: JavaScript would give "
                              "'[object Object]'; print it with console.log, or build the text from "
                              "its fields");
    return expr;
  }
  // A union: as the member it holds, each of which must convert (a union of
  // string literals is a string already).
  if (const Type *u = type->nonOptional(); u->isUnion() && u->isTagged()) {
    for (const Type *m : u->members())
      // A date, a regex and an Error have texts of their own, as alone.
      if ((m->isObject() && !m->isDate() && !m->isRegExp() && !isErrorType(m)) ||
          m->isDict() || m->isFunction() || m->isIterator()) {
        diags_.error(expr->loc(), "a value of type '" + type->name() +
                                      "' cannot be converted to a string: it may hold '" +
                                      m->name() + "'; check what it holds first");
        return expr;
      }
    auto *cast = dyn_cast<CastExpr>(convert(expr, Type::string()));
    if (cast)
      cast->text = true;
    return cast ? cast : expr;
  }
  // Numbers, booleans, undefined, arrays (joined with commas) and optionals of
  // those convert as in JavaScript.
  return convert(expr, Type::string());
}

void Sema::reportMixedConcat(const BinaryExpr *root, const BinaryExpr *mixed) {
  const Type *l = unconverted(mixed->lhs)->type, *r = unconverted(mixed->rhs)->type;
  std::string message = ("operator '+' cannot be applied to types '" + l->name() + "' and '" +
                         r->name() + "' (JavaScript would join them as text)")
                            .str();
  std::vector<std::pair<const Expr *, llvm::StringRef>> parts;
  concatParts(root, parts);
  llvm::StringRef only;
  std::string joined = concatTemplate(parts, only);
  if (joined.empty()) {
    diags_.error(mixed->loc(), message + "; use a template literal instead");
    return;
  }
  if (!only.empty()) { // `"" + n`
    diags_.error(mixed->loc(), message + "; convert it with 'String(" + only + ")'");
    return;
  }
  // `count + input` may be meant as a sum: then the string needs Number().
  if (parts.size() == 2) {
    auto [first, firstText] = parts[0];
    auto [second, secondText] = parts[1];
    auto isText = [](const Expr *e) {
      return e->type->isString() && !isa<StringLitExpr>(e) && !isa<TemplateExpr>(e);
    };
    std::string sum;
    if (first->type->isNumeric() && isText(second))
      sum = (firstText + " + Number(" + secondText + ")").str();
    else if (isText(first) && second->type->isNumeric())
      sum = ("Number(" + firstText + ") + " + secondText).str();
    if (!sum.empty()) {
      diags_.error(mixed->loc(), message + "; to add, convert the string: '" + sum +
                                     "'; to join, use a template: '" + joined + "'");
      return;
    }
  }
  diags_.error(mixed->loc(), message + "; use a template: '" + joined + "'");
}

Expr *Sema::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError() ? expr : convert(expr, Type::intTy());
}

Expr *Sema::checkTemplate(TemplateExpr *tmpl, const Type *expected) {
  std::vector<const Type *> parts;
  bool constant = true;
  for (Expr *&e : tmpl->exprs) {
    e = checkExpr(e);
    // A copy of generic code converts a value of a type parameter to a string
    // here, as the generic check did (noteForced): the type it had.
    const Expr *part = e;
    if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit() && conv->type->isString())
      part = conv->operand;
    parts.push_back(part->type);
    constant &= isa<NameExpr>(part) || isa<StringLitExpr>(part) || isa<IntLitExpr>(part) ||
                isa<BoolLitExpr>(part);
    e = toStringOperand(e);
  }
  tmpl->type = Type::string();
  // Where a template literal type is expected, the type the parts make, as
  // in TypeScript: `on${x}` of a string x is an `on${string}`. One literal
  // of a template of variables and literals is that string literal itself.
  const Type *want = expected ? expected->nonOptional() : nullptr;
  auto templated = [](const Type *t) {
    return t->isPattern() || t->typeOp() == TypeOp::Template || t->isStringLiteral();
  };
  if (!want || !(templated(want) || (want->isUnion() && llvm::any_of(want->members(), templated))))
    return tmpl;
  std::vector<const Type *> operands;
  for (size_t i = 0; i < tmpl->quasis.size(); ++i) {
    operands.push_back(Type::stringLiteral(tmpl->quasis[i]));
    if (i < parts.size()) {
      const Type *part = parts[i];
      if (!(part->isString() || part->isNumeric() || part->isBool() || part->isLiteral() ||
            part->isParam()))
        return tmpl;
      operands.push_back(part);
    }
  }
  const Type *type = evalTypeOp(Type::typeOp(TypeOp::Template, operands), operands);
  if (type->isPattern() ||
      (type->typeOp() == TypeOp::Template && isImplicitlyConvertible(type, want))) {
    tmpl->type = type;
  } else if (type->isStringLiteral() && constant) {
    auto *literal = ctx_.make<StringLitExpr>(tmpl->loc(), type->literalText());
    literal->type = type;
    return literal;
  }
  return tmpl;
}

Expr *Sema::checkMember(MemberExpr *member, const Type *expected) {
  member->type = Type::error();
  if (Expr *part = listPart(member))
    return part;
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    ScopeEntry entry = lookup(name->name);
    if (entry.enumDecl)
      return checkEnumMember(entry.enumDecl, member, expected);
    if (entry.cls)
      return checkStaticMember(member, entry.cls, expected);
  }
  if (Expr *node = checkNodeMember(member))
    return node;
  if (Expr *io = checkProcessMember(member))
    return io;
  if (isGlobalName(member->object, "decimal")) {
    diags_.error(member->loc(), "'decimal." + member->name +
                                    "' can only be called, e.g. 'decimal.max(a, b)'; decimal "
                                    "has the functions max, min and sum");
    return member;
  }
  if (isGlobalName(member->object, "RegExp"))
    return checkRegExpStaticMember(member);
  if (isGlobalName(member->object, "crypto"))
    return checkWebCryptoMember(member);
  if (Expr *bytes = checkBytesStaticMember(member))
    return bytes;
  if (isGlobalName(member->object, "Intl")) {
    diags_.error(member->loc(), "'Intl." + member->name +
                                    "' can only be called or made with 'new', e.g. 'new "
                                    "Intl.DateTimeFormat(\"ru\")'");
    return member;
  }
  for (llvm::StringRef global : {"console", "Math", "String", "Number"}) {
    if (isGlobalName(member->object, global)) {
      if (Expr *constant = globalConstant(global, member->name, member->loc()))
        return constant;
      diags_.error(member->loc(), "'" + global + "." + member->name +
                                      "' can be used as a value only where a function type is "
                                      "expected, e.g. as a callback");
      return member;
    }
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object && object->name == "this")
    thisMember_ = member->name;
  if (object && object->name == "super") {
    // JavaScript reads a field through `super` from the prototype, where it
    // is not.
    diags_.error(member->loc(), "'super." + member->name + "' can only be called, 'super." +
                                    member->name + "(...)'; to read the field or the method, use "
                                    "'this." + member->name + "'");
    return member;
  }
  member->object = apparent(checkExpr(member->object));
  thisMember_ = {};
  if (member->optional)
    member->object = chainLink(member->object);
  const Type *type = member->object->type;
  if (type->isError())
    return member;
  if (type->nonOptional()->isJson())
    return checkJsonMember(member);
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef())
    return checkUnknownMember(member);
  // A member of every member of a union: through a type they all are, or
  // read from each (sema_union.cpp).
  if (type->isUnion()) {
    const Type *common = commonBaseWith(type, member->name);
    if (!common)
      return checkUnionMember(member);
    member->object = convert(member->object, common);
    type = common;
  }
  if (type->isResult())
    return checkResultMember(member);
  if (type->isTuple())
    return checkTupleMember(member);
  if (type->isClass())
    return checkClassMember(member, expected);
  if (type->isRegExp())
    return checkRegexMember(member);
  if (type->isMatch())
    return checkMatchMember(member);
  if (type->isDate())
    return checkDateMember(member);
  if (type->isBytes())
    return checkBytesMember(member);
  if (type->isCrypto())
    return checkCryptoMember(member);
  if (type->isIntl())
    return checkIntlMember(member, expected);
  if (type->isWeakRef()) {
    diags_.error(member->loc(), "property '" + member->name + "' of a WeakRef can only be " +
                                    "called: 'deref()'");
    return member;
  }
  if (type->isDict())
    return checkDictMember(member);
  if (type->isRuntimeObject())
    return checkRuntimeMember(member);
  if (type->isObject() && type->overload(member->name))
    return checkOverloadMember(member, expected);
  if (type->isObject())
    return checkField(member);
  // `args.length` of a list of types too, in generic code.
  if ((type->isString() || type->isArray() || type->isListParam()) && member->name == "length") {
    member->type = Type::intTy();
    return member;
  }
  if (type->isOptional())
    diags_.error(member->object->loc(), "the value may be undefined; check it before reading '" +
                                            member->name + "'");
  else if (type == Type::array(Type::string()) && member->name == "raw")
    diags_.error(member->loc(), "'raw' is not supported: a tag gets the texts of its template as a "
                                "plain 'string[]', with the escapes already applied");
  else if (type->isArray() || type->isString() || type->isIterator())
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'; methods must be called");
  else
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'");
  return member;
}

Expr *Sema::checkPosition(Expr *expr) {
  expr = apparent(checkExpr(expr, Type::longTy()));
  const Type *type = expr->type;
  if (type->isError())
    return expr;
  if (!type->isInteger()) {
    std::string message = "an index or position must be an integer, found '" + type->name().str() + "'";
    if (type->isFloat())
      message += "; round it with Math.floor(x) or drop the fraction with 'x | 0'";
    else if (type->isOptional())
      message += "; the value may be undefined: check it first or give a default with '??'";
    diags_.error(expr->loc(), message);
    return expr;
  }
  return convert(expr, Type::longTy());
}

Expr *Sema::checkIndex(IndexExpr *index) {
  if (Expr *part = listPart(index))
    return part;
  if (auto *name = dyn_cast<NameExpr>(index->object))
    if (EnumDecl *decl = lookup(name->name).enumDecl)
      return checkEnumIndex(decl, index);
  index->object = processAlias(index->object); // `env[name]` of node:process
  if (Expr *variable = checkProcessEnvIndex(index))
    return variable;
  index->object = apparent(checkExpr(index->object));
  if (index->optional)
    index->object = chainLink(index->object);
  index->type = Type::error();
  if (Expr *generic = checkGenericIndex(index))
    return generic;
  if (index->object->type->isMatch())
    return checkMatchIndex(index);
  if (index->object->type->nonOptional()->isJson())
    return checkJsonIndex(index);
  if (index->object->type->isBytes())
    return checkBytesIndex(index);
  if (const Type *type = index->object->type; type->isUnknown() || type->isAnyObject()) {
    index->index = checkExpr(index->index);
    reportUnknownUse(index->object, "");
    return index;
  }
  // `o[key]` with a constant key reads the field it names.
  if (index->object->type->isObject())
    return checkKeyedField(index, /*write=*/false);
  if (index->object->type->isDict())
    return checkDictIndex(index);
  if (index->object->type->isOptional() && index->object->type->inner()->isObject()) {
    index->index = checkExpr(index->index);
    diags_.error(index->object->loc(), "the value may be undefined; check it before indexing");
    return index;
  }
  index->index = checkPosition(index->index);
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError())
    return index;
  // `args[i]` of a list of types, in generic code: `A[0]` for a number,
  // `A[number]` of any.
  if (type->isListParam()) {
    const Expr *at = index->index;
    while (isa<CastExpr>(at) && cast<CastExpr>(at)->isImplicit())
      at = cast<CastExpr>(at)->operand;
    if (auto *lit = dyn_cast<IntLitExpr>(at))
      index->type = Type::indexedAccess(type, Type::numberLiteral(double(lit->value), Type::intTy()));
    else
      index->type = Type::optional(listElement(type));
    return index;
  }
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  if (!element) {
    if (type->isOptional() && (type->inner()->isArray() || type->inner()->isString()))
      diags_.error(index->object->loc(), "the value may be undefined; check it before indexing");
    else
      diags_.error(index->loc(), "type '" + type->name() + "' cannot be indexed");
    return index;
  }
  // Proved in range: a plain read. Otherwise JavaScript yields undefined.
  bool inRange = provesInBounds(index->object, index->index, /*allowEnd=*/false);
  index->mayBeOutOfRange = !inRange;
  index->type = inRange ? element : Type::optional(element);
  return index;
}

Expr *Sema::checkConditional(ConditionalExpr *cond, const Type *expected) {
  Flow whenTrue, whenFalse;
  cond->cond = checkCondition(cond->cond, whenTrue, whenFalse);
  // As in checkIf: the branch an instance's types rule out is dropped.
  if (std::optional<bool> constant = checkingInstance_ ? constantCondition(cond->cond) : std::nullopt) {
    flow_ = *constant ? whenTrue : whenFalse;
    return checkExpr(*constant ? cond->thenExpr : cond->elseExpr, expected);
  }
  // A literal branch takes the type of the other one, which is checked first
  // then (each branch in its own flow): `flag ? { x: 1 } : p`, and a number
  // literal next to a decimal is a decimal.
  auto waits = [&](const Expr *branch) { return literalShaped(branch) || isLiteralTree(branch); };
  auto hint = [&](const Expr *literal, const Expr *other) {
    if (isLiteralTree(literal))
      return literalHint(other, expected);
    return expected ? expected : other->type->isError() ? nullptr : other->type;
  };
  bool thenWaits = waits(cond->thenExpr), elseWaits = waits(cond->elseExpr);
  if (thenWaits && !elseWaits) {
    flow_ = whenFalse;
    cond->elseExpr = checkExpr(cond->elseExpr, expected);
    Flow afterElse = flow_;
    flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, hint(cond->thenExpr, cond->elseExpr));
    flow_ = join(flow_, afterElse);
  } else {
    flow_ = whenTrue;
    cond->thenExpr = checkExpr(cond->thenExpr, expected);
    Flow afterThen = flow_;
    flow_ = whenFalse;
    const Type *elseHint = expected;
    if (elseWaits && !thenWaits)
      elseHint = hint(cond->elseExpr, cond->thenExpr);
    else if (isLiteralTree(cond->elseExpr))
      elseHint = literalHint(cond->thenExpr, expected);
    cond->elseExpr = checkExpr(cond->elseExpr, elseHint);
    flow_ = join(afterThen, flow_);
  }

  const Type *a = cond->thenExpr->type, *b = cond->elseExpr->type;
  cond->type = Type::error();
  if (a->isError() || b->isError()) {
    // A call in a cycle may have no type yet (see inferCycle): the other
    // branch tells.
    if (trial_ && a->isError() != b->isError())
      cond->type = a->isError() ? b : a;
    return cond;
  }
  if (a->isVoid() && b->isVoid()) {
    cond->type = a;
    return cond;
  }
  const Type *common = unionUnify(a, b);
  if (!common) {
    diags_.error(cond->loc(), "the branches of '?:' have incompatible types '" + a->name() +
                                  "' and '" + b->name() + "'");
    return cond;
  }
  cond->thenExpr = convert(cond->thenExpr, common);
  cond->elseExpr = convert(cond->elseExpr, common);
  cond->type = common;
  return cond;
}

// `x as T` is not part of the language: in TypeScript it changes nothing at
// run time, so a conversion written with it would behave differently under
// tsc. The error says what to write instead.
Expr *Sema::checkCast(CastExpr *cast) {
  const Type *to = resolveType(*cast->target, /*allowVoid=*/false);
  // An array literal gets the type it is cast to, so that `[] as int[]`
  // reports only this error.
  bool arrayLiteral = isa<ArrayLitExpr>(cast->operand);
  cast->operand = checkExpr(cast->operand, arrayLiteral && !to->isError() ? to : nullptr);
  const Type *from = cast->operand->type;
  cast->type = to;
  std::string message = "'as' is not supported";
  if (from->isError() || to->isError())
    ;
  else if (arrayLiteral)
    message += "; declare the type of the variable instead, e.g. 'let x: " + to->name().str() +
               " = ...'";
  else if (from == to || isImplicitlyConvertible(from, to))
    message += "; the value converts to '" + to->name().str() + "' by itself, so remove 'as'";
  else if (from->isOptional() && isImplicitlyConvertible(from->inner(), to))
    message += "; the value may be undefined: check it first (e.g. 'x != undefined') or give a "
               "default with '??'";
  else if (from->isFloat() && to->isInt())
    message += "; drop the fraction with 'x | 0', or round with Math.round, Math.floor, Math.ceil "
               "or Math.trunc";
  else if (from->isFloat() && to->isInteger())
    message += "; round with Math.round, Math.floor, Math.ceil or Math.trunc where a '" +
               to->name().str() + "' is expected";
  else if (from->isInteger() && to->isInteger())
    message += "; Math.trunc(x) where an 'int' is expected converts it and checks that it fits";
  else if (from->isFloat() && to->isDecimal())
    message += "; convert the double with 'decimal(x)'";
  else if (from->isDecimal() && to->isNumeric())
    message += "; 'Number(x)' gives the nearest double";
  else if (from->nonOptional()->isJson())
    message += "; convert a Json with 'fromJson<" + to->name().str() +
               ">(x)', which checks what it holds";
  else
    message += ", and type '" + from->name().str() + "' cannot become '" + to->name().str() + "'";
  diags_.error(cast->loc(), message);
  return cast;
}

} // namespace tz
