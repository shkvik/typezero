#include "compiler/memory/cycle_finder/cycle_finder.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

namespace tz {

using Kind = CycleGraph::Kind;

CycleFinder::CycleFinder(MemoryPlan &plan, const CycleScanner &scan, const SourceFile &file)
    : plan_(plan), scan_(scan), file_(file) {
  for (const Type *t : scan_.types) {
    if (t->isObject() && !t->isAnyObject()) {
      objects_.push_back(t);
    }
    if (t->isDict()) {
      dicts_.push_back(t);
    }
  }
}

const Type *CycleFinder::representationOf(const Type *type) {
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

const Type *CycleFinder::valueTypeOf(const VarSymbol *var) {
  if (var->type) {
    return var->type;
  }
  const FunctionDecl *fn = var->function ? var->function : var->lambda;
  const FuncSymbol *sym = fn->sym;

  return Type::function(sym->params, sym->minArgs, sym->hasRest, sym->ret, sym->hasThis);
}

const std::vector<const Type *> &CycleFinder::runtimeTypesOf(const Type *type) {
  auto [it, inserted] = runtimeTypes_.try_emplace(type);
  if (inserted) {
    for (const Type *s : objects_) {
      if (s->isSubtypeOf(type)) {
        it->second.push_back(s);
      }
    }
    for (const auto &[from, to] : file_.views) {
      if (to == type && !llvm::is_contained(it->second, from)) {
        it->second.push_back(from);
      }
    }
  }

  return it->second;
}

const Type *CycleFinder::containerRepresentation(const Type *type) {
  if (type->isArray()) {
    return Type::array(representationOf(type->inner()));
  }
  if (type->isDict()) {
    return Type::dict(type->dictKind(), representationOf(type->keyType()),
                      type->isSet() ? type->valueType() : representationOf(type->valueType()));
  }

  return type;
}

bool CycleFinder::isMutated(const Type *type) const {
  return scan_.mutated.contains(type) || scan_.mutated.contains(containerRepresentation(type));
}

unsigned CycleFinder::fnNodeOf(const Type *type) {
  return graph.node(Kind::FnNode, type->nonOptional());
}

int CycleFinder::nodeOf(Kind kind, const void *key) const {
  auto it = graph.index.find({int(kind), key});

  return it == graph.index.end() ? -1 : int(it->second);
}

void CycleFinder::refers(const Type *type, std::vector<unsigned> &out) {
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
      out.push_back(anyNode_);
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
    out.push_back(unknownNode_);
    break;
  case Type::Kind::Promise:
    // A promise may be a member (promiseIsMember): it holds its value or its error, and an
    // async function's frame whatever that holds, so it may hold anything.
    out.push_back(anyNode_);
    out.push_back(unknownNode_);
    out.push_back(fnNodeOf(anyFunction_));
    break;
  default:
    // Iterators and timers are never members: a cycle through what they hold is not freed (see
    // README).
    break;
  }
}

void CycleFinder::edges(unsigned from, const Type *type, bool late) {
  std::vector<unsigned> to;
  refers(type, to);
  for (unsigned n : to) {
    graph.edge(from, n, late);
  }
}

void CycleFinder::captureEdges(unsigned from, const VarSymbol *var) {
  if (var->boxed()) {
    graph.edge(from, graph.node(Kind::BoxNode, valueTypeOf(var)), false);
  } else {
    edges(from, valueTypeOf(var), false);
  }
}

void CycleFinder::run() {
  plan_.cyclicTypes.clear();
  plan_.cyclicEnvs.clear();
  plan_.cyclicBoxes.clear();
  plan_.cyclicArrayReps.clear();
  plan_.refMember.clear();
  plan_.hasCycles = plan_.anyCyclicObject = plan_.fnMayBeMember = false;
  graph = CycleGraph();
  anyFunction_ = representationOf(Type::function({}, 0, false, Type::voidTy()));
  anyNode_ = graph.node(Kind::AnyObjectNode, nullptr);
  unknownNode_ = graph.node(Kind::UnknownNode, nullptr);
  buildGraph();
  markCyclic(cyclicNodes());
  markFunctions();
}

void CycleFinder::buildGraph() {
  for (const Type *t : scan_.types) {
    if (t->isObject() && !t->isAnyObject() && !t->isWeakRef()) {
      unsigned n = graph.node(Kind::TypeNode, t);
      for (const Field &f : t->fields()) {
        edges(n, f.type, scan_.mutableFields.count(f.name) != 0);
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
  for (const auto &[key, captures] : scan_.envs) {
    unsigned n = graph.node(Kind::EnvNode, key);
    for (const VarSymbol *var : captures) {
      if (!plan_.uncounted.contains({key, var})) {
        captureEdges(n, var);
      }
    }
  }
  // What a value of a function type may hold: the environments of the closures of that type,
  // the objects of the methods taken as values of it, and whatever a value of a type converted
  // to it holds. Generic code sees every function as one type.
  for (const auto &[type, keys] : scan_.closuresOf) {
    for (const void *key : keys) {
      unsigned fn = fnNodeOf(type), env = graph.node(Kind::EnvNode, key);
      graph.edge(fn, env, false);
    }
  }
  for (const auto &[from, to] : scan_.fnCasts) {
    unsigned a = fnNodeOf(to), b = fnNodeOf(from);
    graph.edge(a, b, false);
  }
  for (const VarSymbol *var : scan_.boxed) {
    unsigned n = graph.node(Kind::BoxNode, valueTypeOf(var));
    edges(n, valueTypeOf(var), true);
  }
  for (const auto &[fnType, holders] : scan_.boundOf) {
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
    unsigned any = fnNodeOf(anyFunction_);
    for (unsigned v = 0, end = unsigned(graph.nodes.size()); v < end; ++v) {
      if (graph.nodes[v].kind == Kind::FnNode && v != any) {
        graph.edge(any, v, false);
      }
    }
  }
  for (const Type *t : objects_) {
    if (!t->isWeakRef()) {
      unsigned n = graph.node(Kind::TypeNode, t);
      graph.edge(anyNode_, n, false);
    }
  }
  for (const Type *t : dicts_) {
    unsigned n = graph.node(Kind::TypeNode, t);
    graph.edge(anyNode_, n, false);
  }
  // An object in an unknown is the reference itself; a function is in a box, which is a member
  // when what it holds may be one (Lowering::boxValue): a listener in the Record of an emitter
  // of node:events that captures the emitter is a cycle through it. Any other value is in a box
  // no group has.
  for (const Type *type : scan_.toUnknown) {
    std::vector<unsigned> to;
    refers(type, to);
    for (unsigned n : to) {
      const CycleGraph::Node &node = graph.nodes[n];
      auto *t = static_cast<const Type *>(node.key);
      if ((node.kind == Kind::TypeNode && (t->isObject() || t->isDict())) || n == anyNode_ ||
          node.kind == Kind::FnNode) {
        graph.edge(unknownNode_, n, false);
      }
    }
  }
}

std::vector<bool> CycleFinder::cyclicNodes() const {
  // The strongly connected components (Tarjan); one is cyclic if an edge inside it may be set
  // late.
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
  std::vector<bool> cyclic(count, false);
  for (unsigned v = 0; v < count; ++v) {
    cyclic[v] = cyclicComponent[size_t(component[v])];
  }

  return cyclic;
}

void CycleFinder::markCyclic(const std::vector<bool> &cyclic) {
  for (unsigned v = 0; v < cyclic.size(); ++v) {
    if (!cyclic[v]) {
      continue;
    }
    const CycleGraph::Node &node = graph.nodes[v];
    switch (node.kind) {
    case Kind::TypeNode: {
      auto *type = static_cast<const Type *>(node.key);
      plan_.cyclicTypes.insert(type);
      if (type->isObject() || type->isDict()) {
        plan_.anyCyclicObject = true;
      }
      if (type->isArray()) {
        plan_.cyclicArrayReps.insert(representationOf(type));
      }
      plan_.hasCycles = true;
      break;
    }
    case Kind::EnvNode:
      plan_.cyclicEnvs.insert(node.key);
      plan_.hasCycles = true;
      break;
    case Kind::BoxNode:
      plan_.cyclicBoxes.insert(static_cast<const Type *>(node.key));
      plan_.hasCycles = true;
      break;
    default:
      break;
    }
  }
}

void CycleFinder::markFunctions() {
  // The function types whose values may hold a member: the environment of a cyclic closure, or
  // a cyclic object whose method it is, through the types converted to them too.
  for (unsigned v = 0, count = unsigned(graph.nodes.size()); v < count; ++v) {
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
        } else if ((node.kind == Kind::EnvNode && plan_.cyclicEnvs.contains(node.key)) ||
                   (node.kind == Kind::TypeNode &&
                    plan_.cyclicTypes.contains(static_cast<const Type *>(node.key)))) {
          member = true;
        }
      }
    }
    auto *type = static_cast<const Type *>(graph.nodes[v].key);
    plan_.fnMayBeMember = plan_.fnMayBeMember || member;
    // `resolve` of `new Promise` has its promise as its environment, which may be a member in a
    // program with cycles (promiseIsMember).
    plan_.refMember[type] = member || plan_.hasCycles;
  }
}

} // namespace tz
