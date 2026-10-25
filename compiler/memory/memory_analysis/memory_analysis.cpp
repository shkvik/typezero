#include "compiler/lower/memory/memory_analysis/memory_analysis.h"
#include "compiler/lower/functions/closure_lowerer/closure_lowerer.h"
#include "compiler/lower/memory/cycle_scanner/cycle_scanner.h"
#include "compiler/lower/memory/owner_scanner/owner_scanner.h"
#include "compiler/lower/memory/store_scanner/store_scanner.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

namespace {

// The type a generic copy sees a value of `type` as (Sema::representation).
const Type *representationOf(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::Object:
  case Type::Kind::Iterator:
  case Type::Kind::Dict:
    return Type::anyObject();
  case Type::Kind::Function:
    return Type::function({}, 0, false, Type::voidTy());
  case Type::Kind::Array:
    return Type::array(representationOf(type->inner()));
  case Type::Kind::Optional:
    return Type::optional(representationOf(type->inner()));
  default:
    return type;
  }
}

// How CodeGen walks the references in a value for the group functions: the
// reference itself, or the environment of a function; anything else goes to
// the runtime by its descriptor.
enum class RefShape { None, Pointer, Function, Optional, Other };

} // namespace

MemoryAnalysis::MemoryAnalysis(CodeGen &codegen) : codegen_(codegen) {}

void MemoryAnalysis::analyzeCycles(SourceFile &file) {
  CycleScanner scan;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        scan.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
          scan.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          scan.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          scan.function(fn);
        }
        scan.function(cls->init);
        for (const ClassField &f : cls->fields) {
          scan.function(f.method);
        }
        scan.classDecl(cls);
      }
      continue;
    }
    scan.stmt(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    scan.function(fn);
  }
  for (const Type *type : codegen_.subtypes_) {
    scan.addType(type);
  }
  for (const auto &[from, to] : file.views) {
    scan.addType(from);
    scan.addType(to);
  }

  // Layer 2: the functions in owned objects whose references to their
  // owners need no count.
  OwnerScanner owners;
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        owners.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
          owners.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          owners.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          owners.function(fn);
        }
        owners.function(cls->init);
        for (const ClassField &f : cls->fields) {
          owners.function(f.method);
        }
      }
    }
    owners.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    owners.function(fn);
  }
  // A class whose construction or methods let `this` go somewhere.
  auto escapes = [](const ClassDecl *cls) {
    for (const FunctionDecl *fn : cls->constructors) {
      if (fn->thisVar && CycleScanner::thisEscapes(fn->body, fn->thisVar)) {
        return true;
      }
    }
    if (cls->init && cls->init->thisVar &&
        CycleScanner::thisEscapes(cls->init->body, cls->init->thisVar)) {
      return true;
    }
    for (const FunctionDecl *fn : cls->methods) {
      if (!fn->isStatic && fn->thisVar && fn->body &&
          CycleScanner::thisEscapes(fn->body, fn->thisVar)) {
        return true;
      }
    }
    for (const ClassField &f : cls->fields) {
      if (f.method && f.method->thisVar &&
          CycleScanner::thisEscapes(f.method->body, f.method->thisVar)) {
        return true;
      }
    }

    return false;
  };
  std::vector<OwnerScanner::BackRef> backRefs = owners.proven(escapes);

  // The object types each type of reference may point at: itself, the
  // types declared to be it, and those code sees as it (views).
  std::vector<const Type *> objects, dicts;
  for (const Type *t : scan.types) {
    if (t->isObject() && !t->isAnyObject()) {
      objects.push_back(t);
    }
    if (t->isDict()) {
      dicts.push_back(t);
    }
  }
  std::map<const Type *, std::vector<const Type *>> runtimeTypes;
  auto runtimeTypesOf = [&](const Type *type) -> const std::vector<const Type *> & {
    auto [it, inserted] = runtimeTypes.try_emplace(type);
    if (inserted) {
      for (const Type *s : objects) {
        if (s->isSubtypeOf(type)) {
          it->second.push_back(s);
        }
      }
      for (const auto &[from, to] : file.views) {
        if (to == type && !llvm::is_contained(it->second, from)) {
          it->second.push_back(from);
        }
      }
    }

    return it->second;
  };

  // An array or dictionary type is added to when it, or how generic code
  // sees it, is.
  auto containerRepresentation = [](const Type *type) -> const Type * {
    if (type->isArray()) {
      return Type::array(representationOf(type->inner()));
    }
    if (type->isDict()) {
      return Type::dict(type->dictKind(), representationOf(type->keyType()),
                        type->isSet() ? type->valueType() : representationOf(type->valueType()));
    }

    return type;
  };
  auto isMutated = [&](const Type *type) {
    return scan.mutated.contains(type) || scan.mutated.contains(containerRepresentation(type));
  };

  // The graph, without the references layer 2 proved need no count; an
  // owned object that is cyclic all the same may outlive its owner through
  // its group, so its proof is dropped and the graph made again.
  for (;;) {
    codegen_.uncounted_.clear();
    for (const OwnerScanner::BackRef &b : backRefs) {
      codegen_.uncounted_.insert({b.fn, b.self});
    }
    codegen_.cyclicTypes_.clear();
    codegen_.cyclicEnvs_.clear();
    codegen_.cyclicBoxes_.clear();
    cyclicArrayReps_.clear();
    refMember_.clear();
    codegen_.hasCycles_ = codegen_.anyCyclicObject_ = fnMayBeMember_ = false;
    CycleGraph graph;
    using Kind = CycleGraph::Kind;
    auto fnNodeOf = [&](const Type *type) { return graph.node(Kind::FnNode, type->nonOptional()); };
    const Type *anyFunction = representationOf(Type::function({}, 0, false, Type::voidTy()));
    const unsigned anyNode = graph.node(Kind::AnyObjectNode, nullptr);
    const unsigned unknownNode = graph.node(Kind::UnknownNode, nullptr);
    // The nodes a value of `type` may refer to.
    std::function<void(const Type *, std::vector<unsigned> &)> refers =
        [&](const Type *type, std::vector<unsigned> &out) {
          switch (type->kind()) {
          case Type::Kind::Optional:
            refers(type->inner(), out);
            break;
          case Type::Kind::Union:
            for (const Type *m : type->members()) {
              refers(m, out);
            }
            break;
          case Type::Kind::Result:
            refers(type->okType(), out);
            refers(type->errType(), out);
            break;
          case Type::Kind::Object:
            if (type->isAnyObject()) {
              out.push_back(anyNode);
            } else if (!type->isWeakRef()) {
              for (const Type *s : runtimeTypesOf(type)) {
                out.push_back(graph.node(Kind::TypeNode, s));
              }
            }
            break;
          case Type::Kind::Array:
          case Type::Kind::Dict:
            out.push_back(graph.node(Kind::TypeNode, type));
            break;
          case Type::Kind::Function:
            out.push_back(fnNodeOf(type));
            break;
          case Type::Kind::Unknown:
            out.push_back(unknownNode);
            break;
          case Type::Kind::Promise:
            // A promise may be a member (promiseIsMember): it holds its value
            // or its error, and an async function's frame whatever that
            // holds, so it may hold anything.
            out.push_back(anyNode);
            out.push_back(unknownNode);
            out.push_back(fnNodeOf(anyFunction));
            break;
          default:
            // Iterators and timers are never members: a cycle through what
            // they hold is not freed (see README).
            break;
          }
        };
    auto edges = [&](unsigned from, const Type *type, bool late) {
      std::vector<unsigned> to;
      refers(type, to);
      for (unsigned n : to) {
        graph.edge(from, n, late);
      }
    };
    auto captureEdges = [&](unsigned from, const VarSymbol *var) {
      if (var->boxed()) {
        graph.edge(from, graph.node(Kind::BoxNode, ClosureLowerer::typeOf(var)), false);
      } else {
        edges(from, ClosureLowerer::typeOf(var), false);
      }
    };
    for (const Type *t : scan.types) {
      if (t->isObject() && !t->isAnyObject() && !t->isWeakRef()) {
        unsigned n = graph.node(Kind::TypeNode, t);
        for (const Field &f : t->fields()) {
          edges(n, f.type, scan.mutableFields.count(f.name) != 0);
        }
        // What the methods of a literal capture lies in the object.
        if (const ClassDecl *cls = t->classDecl(); cls && cls->group) {
          for (const VarSymbol *var : cls->group->captures) {
            captureEdges(n, var);
          }
        }
      } else if (t->isArray()) {
        edges(graph.node(Kind::TypeNode, t), t->inner(), isMutated(t));
      } else if (t->isDict()) {
        unsigned n = graph.node(Kind::TypeNode, t);
        edges(n, t->keyType(), isMutated(t));
        if (!t->isSet()) {
          edges(n, t->valueType(), isMutated(t));
        }
      }
    }
    for (const auto &[key, captures] : scan.envs) {
      unsigned n = graph.node(Kind::EnvNode, key);
      for (const VarSymbol *var : captures) {
        if (!codegen_.uncounted_.contains({key, var})) {
          captureEdges(n, var);
        }
      }
    }
    // What a value of a function type may hold: the environments of the
    // closures of that type, the objects of the methods taken as values of
    // it, and whatever a value of a type converted to it holds. Generic code
    // sees every function as one type.
    for (const auto &[type, keys] : scan.closuresOf) {
      for (const void *key : keys) {
        unsigned fn = fnNodeOf(type), env = graph.node(Kind::EnvNode, key);
        graph.edge(fn, env, false);
      }
    }
    for (const auto &[from, to] : scan.fnCasts) {
      unsigned a = fnNodeOf(to), b = fnNodeOf(from);
      graph.edge(a, b, false);
    }
    for (const VarSymbol *var : scan.boxed) {
      unsigned n = graph.node(Kind::BoxNode, ClosureLowerer::typeOf(var));
      edges(n, ClosureLowerer::typeOf(var), true);
    }
    for (const auto &[fnType, holders] : scan.boundOf) {
      for (const Type *type : holders) {
        std::vector<unsigned> to;
        refers(type, to);
        unsigned fn = fnNodeOf(fnType);
        for (unsigned n : to) {
          graph.edge(fn, n, false);
        }
      }
    }
    {
      unsigned any = fnNodeOf(anyFunction);
      for (unsigned v = 0, end = unsigned(graph.nodes.size()); v < end; ++v) {
        if (graph.nodes[v].kind == Kind::FnNode && v != any) {
          graph.edge(any, v, false);
        }
      }
    }
    for (const Type *t : objects) {
      if (!t->isWeakRef()) {
        unsigned n = graph.node(Kind::TypeNode, t);
        graph.edge(anyNode, n, false);
      }
    }
    for (const Type *t : dicts) {
      unsigned n = graph.node(Kind::TypeNode, t);
      graph.edge(anyNode, n, false);
    }
    // An object in an unknown is the reference itself; a function is in a box,
    // which is a member when what it holds may be one (CodeGen::boxValue): a
    // listener in the Record of an emitter of node:events that captures the
    // emitter is a cycle through it. Any other value is in a box no group has.
    for (const Type *type : scan.toUnknown) {
      std::vector<unsigned> to;
      refers(type, to);
      for (unsigned n : to) {
        const CycleGraph::Node &node = graph.nodes[n];
        auto *t = static_cast<const Type *>(node.key);
        if ((node.kind == Kind::TypeNode && (t->isObject() || t->isDict())) || n == anyNode ||
            node.kind == Kind::FnNode) {
          graph.edge(unknownNode, n, false);
        }
      }
    }

    // The strongly connected components (Tarjan); one is cyclic if an edge
    // inside it may be set late.
    size_t count = graph.nodes.size();
    std::vector<int> order(count, -1), low(count, 0), component(count, -1);
    std::vector<bool> onStack(count, false);
    std::vector<unsigned> stack;
    int next = 0, components = 0;
    std::function<void(unsigned)> visit = [&](unsigned v) {
      order[v] = low[v] = next++;
      stack.push_back(v);
      onStack[v] = true;
      for (auto [w, late] : graph.nodes[v].edges) {
        if (order[w] < 0) {
          visit(w);
          low[v] = std::min(low[v], low[w]);
        } else if (onStack[w]) {
          low[v] = std::min(low[v], order[w]);
        }
      }
      if (low[v] != order[v]) {
        return;
      }
      unsigned w;
      do {
        w = stack.back();
        stack.pop_back();
        onStack[w] = false;
        component[w] = components;
      } while (w != v);
      ++components;
    };
    for (unsigned v = 0; v < count; ++v) {
      if (order[v] < 0) {
        visit(v);
      }
    }
    std::vector<bool> cyclicComponent(size_t(components), false);
    for (unsigned v = 0; v < count; ++v) {
      for (auto [w, late] : graph.nodes[v].edges) {
        if (late && component[v] == component[w]) {
          cyclicComponent[size_t(component[v])] = true;
        }
      }
    }

    for (unsigned v = 0; v < count; ++v) {
      if (!cyclicComponent[size_t(component[v])]) {
        continue;
      }
      const CycleGraph::Node &node = graph.nodes[v];
      switch (node.kind) {
      case Kind::TypeNode: {
        auto *type = static_cast<const Type *>(node.key);
        codegen_.cyclicTypes_.insert(type);
        if (type->isObject() || type->isDict()) {
          codegen_.anyCyclicObject_ = true;
        }
        if (type->isArray()) {
          cyclicArrayReps_.insert(representationOf(type));
        }
        codegen_.hasCycles_ = true;
        break;
      }
      case Kind::EnvNode:
        codegen_.cyclicEnvs_.insert(node.key);
        codegen_.hasCycles_ = true;
        break;
      case Kind::BoxNode:
        codegen_.cyclicBoxes_.insert(static_cast<const Type *>(node.key));
        codegen_.hasCycles_ = true;
        break;
      default:
        break;
      }
    }
    // The function types whose values may hold a member: the environment of
    // a cyclic closure, or a cyclic object whose method it is, through the
    // types converted to them too.
    for (unsigned v = 0; v < count; ++v) {
      if (graph.nodes[v].kind != Kind::FnNode) {
        continue;
      }
      std::vector<unsigned> work{v};
      llvm::DenseSet<unsigned> seen{v};
      bool member = false;
      while (!work.empty() && !member) {
        unsigned n = work.back();
        work.pop_back();
        for (auto [w, late] : graph.nodes[n].edges) {
          const CycleGraph::Node &node = graph.nodes[w];
          if (node.kind == Kind::FnNode) {
            if (seen.insert(w).second) {
              work.push_back(w);
            }
          } else if ((node.kind == Kind::EnvNode && codegen_.cyclicEnvs_.contains(node.key)) ||
                     (node.kind == Kind::TypeNode &&
                      codegen_.cyclicTypes_.contains(static_cast<const Type *>(node.key)))) {
            member = true;
          }
        }
      }
      auto *type = static_cast<const Type *>(graph.nodes[v].key);
      fnMayBeMember_ = fnMayBeMember_ || member;
      // `resolve` of `new Promise` has its promise as its environment, which
      // may be a member in a program with cycles (promiseIsMember).
      refMember_[type] = member || codegen_.hasCycles_;
    }
    // What may hold a member, and may be held by one, is one too: an object,
    // array, dictionary, environment or box outside the groups that a member
    // held, and that held a reference into a group, would count from outside
    // it a reference that the group may own through it (an X that holds a T
    // both in a field and in an array of them), and the group could then
    // never be freed though nothing outside reaches it. One that no member
    // holds stays outside: what it holds is held from outside indeed, and the
    // groups stay as small as the cycles make them.
    if (codegen_.hasCycles_) {
      auto nodeOf = [&](Kind kind, const void *key) -> int {
        auto it = graph.index.find({int(kind), key});

        return it == graph.index.end() ? -1 : int(it->second);
      };
      auto isMemberNode = [&](unsigned v) {
        const CycleGraph::Node &node = graph.nodes[v];
        switch (node.kind) {
        case Kind::TypeNode:
          return codegen_.cyclicTypes_.contains(static_cast<const Type *>(node.key));
        case Kind::EnvNode:
          return codegen_.cyclicEnvs_.contains(node.key);
        case Kind::BoxNode:
          return codegen_.cyclicBoxes_.contains(static_cast<const Type *>(node.key));
        default:
          return false;
        }
      };
      // A function value, an object of any type or an unknown is the reference
      // to what it holds: what holds it holds that.
      auto passesOn = [&](unsigned v) {
        Kind kind = graph.nodes[v].kind;

        return kind == Kind::FnNode || kind == Kind::AnyObjectNode || kind == Kind::UnknownNode;
      };
      // What the promises of each value hold, found before the nodes are
      // counted (refers may make nodes).
      std::vector<unsigned> callbacks;
      for (const Type *t : scan.reactionCallbacks) {
        callbacks.push_back(fnNodeOf(t));
      }
      std::vector<std::pair<const Type *, std::vector<unsigned>>> promised;
      for (const Type *t : scan.types) {
        if (t->isPromise()) {
          promised.push_back({t->promised(), {}});
          refers(t->promised(), promised.back().second);
        }
      }
      std::vector<bool> held;
      auto findHeld = [&] {
        held.assign(graph.nodes.size(), false);
        std::vector<unsigned> work;
        std::vector<bool> seen(graph.nodes.size(), false);
        auto reach = [&](unsigned v) {
          held[v] = true;
          if (passesOn(v) && !seen[v]) {
            seen[v] = true;
            work.push_back(v);
          }
        };
        for (unsigned v = 0; v < graph.nodes.size(); ++v) {
          if (isMemberNode(v)) {
            seen[v] = true;
            work.push_back(v);
          }
        }
        // A reaction to a promise is a member that holds its callbacks
        // (attachReaction), and a promise one that holds its value.
        for (unsigned n : callbacks) {
          reach(n);
        }
        for (const auto &[value, to] : promised) {
          if (codegen_.promiseIsMember(value)) {
            for (unsigned n : to) {
              reach(n);
            }
          }
        }
        while (!work.empty()) {
          unsigned v = work.back();
          work.pop_back();
          for (auto [w, late] : graph.nodes[v].edges) {
            reach(w);
          }
        }
      };
      auto isHeld = [&](Kind kind, const void *key) {
        int v = nodeOf(kind, key);

        return v >= 0 && held[unsigned(v)];
      };
      for (bool changed = true; changed;) {
        changed = false;
        findHeld();
        auto add = [&](llvm::DenseSet<const Type *> &set, const Type *t) {
          if (set.insert(t).second) {
            refMember_.clear();
            changed = true;
          }
        };
        for (const Type *t : scan.types) {
          if (codegen_.cyclicTypes_.contains(t) || t->isWeakRef() || t->isAnyObject() ||
              t->isRuntimeObject() || t->isBytes() || t->isCrypto() || t->isIntl() || t->isDate() ||
              t->isRegExp() || t->isMatch() || t->isTimer()) {
            continue;
          }
          if (!(t->isObject() || t->isArray() || t->isDict()) || !isHeld(Kind::TypeNode, t)) {
            continue;
          }
          bool holds = false;
          if (t->isObject()) {
            for (const Field &f : t->fields()) {
              holds = holds || mayHoldMember(f.type);
            }
          } else if (t->isArray()) {
            holds = mayHoldMember(t->inner());
          } else {
            holds = mayHoldMember(t->keyType()) || (!t->isSet() && mayHoldMember(t->valueType()));
          }
          if (!holds) {
            continue;
          }
          add(codegen_.cyclicTypes_, t);
          if (t->isObject() || t->isDict()) {
            codegen_.anyCyclicObject_ = true;
          }
          if (t->isArray()) {
            cyclicArrayReps_.insert(representationOf(t));
          }
        }
        for (const auto &[key, captures] : scan.envs) {
          if (codegen_.cyclicEnvs_.contains(key) || !isHeld(Kind::EnvNode, key)) {
            continue;
          }
          bool holds = false;
          for (const VarSymbol *var : captures) {
            holds = holds || (var->boxed() ? codegen_.isCyclicBox(ClosureLowerer::typeOf(var)) : mayHoldMember(ClosureLowerer::typeOf(var)));
          }
          if (holds && codegen_.cyclicEnvs_.insert(key).second) {
            refMember_.clear();
            changed = true;
          }
        }
        for (const VarSymbol *var : scan.boxed) {
          if (isHeld(Kind::BoxNode, ClosureLowerer::typeOf(var)) && mayHoldMember(ClosureLowerer::typeOf(var))) {
            add(codegen_.cyclicBoxes_, ClosureLowerer::typeOf(var));
          }
        }
      }
    }
    size_t before = backRefs.size();
    llvm::erase_if(backRefs, [&](const OwnerScanner::BackRef &b) {
      const ClassDecl *made = owners.madeBy(b.field);

      return !made || refMayBeMember(made->type);
    });
    if (backRefs.size() == before) {
      for (const OwnerScanner::BackRef &b : backRefs) {
        codegen_.ownedFields_.insert({b.owner->type, b.field.str()});
        codegen_.backRefFields_.insert({owners.madeBy(b.field)->type, b.callback.str()});
      }
      break;
    }
  }

  // WeakRef<T>: the objects of T, and of the types declared to be it.
  for (const Type *t : scan.types) {
    if (t->isWeakRef()) {
      for (const Type *s : runtimeTypesOf(t->weakTarget())) {
        codegen_.weakTargets_.insert(s);
      }
    }
  }

  if (codegen_.hasCycles_) {
    findUnstored(file);
  }

  // The globals that hold their values for good: declared at the top level
  // of a file and never assigned again (see pinValue). Top-level code that
  // awaits keeps those no function uses in its coroutine, which ends while
  // timers may still run: they are not eternal there.
  for (Stmt *item : file.items) {
    if (auto *decl = dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        if (d.sym && !d.alias && !d.sym->reassigned && mayHoldMember(d.sym->type) &&
            (d.sym->usedInFunction || !file.topLevelAwait)) {
          codegen_.eternals_.insert(d.sym);
        }
      }
    }
    // The static fields of a class are globals too.
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const ClassField &f : cls->fields) {
        if (f.isStatic && f.var && f.init && !f.var->reassigned && mayHoldMember(f.var->type)) {
          codegen_.eternals_.insert(f.var);
        }
      }
    }
  }
}

void MemoryAnalysis::findUnstored(SourceFile &file) {
  auto isSite = [this](const Expr *e) -> bool {
    switch (e->kind()) {
    case Expr::Kind::New: {
      auto *create = cast<NewExpr>(e);
      return create->cls && codegen_.isCyclic(create->cls->type);
    }
    case Expr::Kind::ObjectLit: {
      auto *lit = cast<ObjectLitExpr>(e);
      const Type *type = lit->cls ? lit->cls->type : lit->type;
      return type && codegen_.isCyclic(type);
    }
    case Expr::Kind::ArrayLit:
      return e->type && codegen_.isCyclic(e->type);
    case Expr::Kind::Call:
      switch (cast<CallExpr>(e)->builtin) {
      case Builtin::ArrSlice:
      case Builtin::ArrMap:
      case Builtin::ArrFilter:
      case Builtin::ArrFlatMap:
      case Builtin::ArrConcat:
      case Builtin::DictNew:
        return e->type && codegen_.isCyclic(e->type);
      default:

        return false;
      }
    case Expr::Kind::Function: {
      const FunctionDecl *fn = cast<FunctionExpr>(e)->fn;
      const void *key = fn->group ? static_cast<const void *>(fn->group) : fn;
      return !fn->nonEscaping && codegen_.isCyclicEnv(key);
    }
    default:
      return false;
    }
  };
  StoreScanner scan(isSite);
  for (Stmt *item : file.items) {
    if (auto *fn = dyn_cast<FunctionDecl>(item)) {
      if (fn->typeParams.empty() && !file.unusedFunctions.contains(fn)) {
        scan.function(fn);
      }
      continue;
    }
    if (auto *cls = dyn_cast<ClassDecl>(item)) {
      for (const FunctionDecl *fn : cls->methods) {
        if (fn->typeParams.empty() && (fn->isStatic ? !cls->generic : cls->isEmitted())) {
          scan.function(fn);
        }
      }
      if (cls->isEmitted()) {
        for (const FunctionDecl *fn : cls->methodInstances) {
          scan.function(fn);
        }
        for (const FunctionDecl *fn : cls->constructors) {
          scan.function(fn);
        }
        scan.function(cls->init);
        for (const ClassField &f : cls->fields) {
          scan.function(f.method);
        }
      }
      // Reflection may call any method of the class (Reflect.get, then
      // Reflect.apply), and whoever called it may keep what it gives.
      if (cls->reflectMethods) {
        for (const ClassDecl *c = cls; c; c = c->parent) {
          for (const FunctionDecl *fn : c->methods) {
            if (!fn->isStatic) {
              scan.escapes(fn);
            }
          }
        }
      }
    }
    scan.topLevel(item);
  }
  for (const FunctionDecl *fn : file.instances) {
    scan.function(fn);
  }
  codegen_.unstoredSites_ = scan.unstored();
}

bool MemoryAnalysis::refMayBeMember(const Type *type) {
  if (!codegen_.hasCycles_) {
    return false;
  }
  auto found = refMember_.find(type);
  if (found != refMember_.end()) {
    return found->second;
  }
  bool result = false;
  switch (type->kind()) {
  case Type::Kind::Object:
    if (type->isAnyObject()) {
      result = codegen_.anyCyclicObject_;
    } else if (!type->isWeakRef()) {
      result = codegen_.isCyclic(type) || llvm::any_of(codegen_.subtypes_, [&](const Type *s) {
                 return codegen_.isCyclic(s) && s->isSubtypeOf(type);
               });
    }
    break;
  case Type::Kind::Array:
    result = codegen_.isCyclic(type) || cyclicArrayReps_.contains(type);
    break;
  case Type::Kind::Dict:
    result = codegen_.isCyclic(type);
    break;
  case Type::Kind::Function:
    // The environment of a closure, or a promise: `resolve` of `new Promise`
    // has its promise as its environment, and a promise may be a member
    // (promiseIsMember, runtime/core/async/async.c).
    result = fnMayBeMember_ || codegen_.hasCycles_;
    break;
  case Type::Kind::Promise:
    // By the type of its value, or of its error (promiseIsMember); a
    // promise of one type may be seen as one of another.
    result = codegen_.hasCycles_;
    break;
  case Type::Kind::Unknown:
    // An object, a promise, or the box of a value that may hold a member,
    // which is a member itself (CodeGen::boxValue).
    result = codegen_.hasCycles_;
    break;
  case Type::Kind::Union:
    result = llvm::any_of(type->members(), [&](const Type *m) { return mayHoldMember(m); });
    break;
  default:
    break;
  }
  refMember_[type] = result;

  return result;
}

bool MemoryAnalysis::mayHoldMember(const Type *type) {
  if (!codegen_.hasCycles_) {
    return false;
  }
  switch (type->kind()) {
  case Type::Kind::Optional:
    return mayHoldMember(type->inner());
  case Type::Kind::Result:
    return mayHoldMember(type->okType()) || mayHoldMember(type->errType());
  default:
    return refMayBeMember(type);
  }
}

} // namespace tz
