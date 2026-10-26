#include "compiler/memory/held_members/held_members.h"

namespace tz {

using Kind = CycleGraph::Kind;

HeldMembers::HeldMembers(MemoryPlan &plan, const CycleScanner &scan, CycleFinder &finder)
    : plan_(plan), scan_(scan), finder_(finder) {}

bool HeldMembers::isMemberNode(unsigned v) const {
  const CycleGraph::Node &node = finder_.graph.nodes[v];
  switch (node.kind) {
  case Kind::TypeNode:
    return plan_.cyclicTypes.contains(static_cast<const Type *>(node.key));
  case Kind::EnvNode:
    return plan_.cyclicEnvs.contains(node.key);
  case Kind::BoxNode:
    return plan_.cyclicBoxes.contains(static_cast<const Type *>(node.key));
  default:
    return false;
  }
}

bool HeldMembers::passesOn(unsigned v) const {
  Kind kind = finder_.graph.nodes[v].kind;

  return kind == Kind::FnNode || kind == Kind::AnyObjectNode || kind == Kind::UnknownNode;
}

void HeldMembers::findHeld() {
  const CycleGraph &graph = finder_.graph;
  held_.assign(graph.nodes.size(), false);
  std::vector<unsigned> work;
  std::vector<bool> seen(graph.nodes.size(), false);
  auto reach = [&](unsigned v) {
    held_[v] = true;
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
  for (unsigned n : callbacks_) {
    reach(n);
  }
  for (const auto &[value, to] : promised_) {
    if (plan_.promiseIsMember(value)) {
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
}

bool HeldMembers::isHeld(Kind kind, const void *key) const {
  int v = finder_.nodeOf(kind, key);

  return v >= 0 && held_[unsigned(v)];
}

bool HeldMembers::add(llvm::DenseSet<const Type *> &set, const Type *t) {
  if (!set.insert(t).second) {
    return false;
  }
  plan_.refMember.clear();

  return true;
}

bool HeldMembers::addType(const Type *t) {
  if (plan_.cyclicTypes.contains(t) || t->isWeakRef() || t->isAnyObject() || t->isRuntimeObject() ||
      t->isBytes() || t->isCrypto() || t->isIntl() || t->isDate() || t->isRegExp() ||
      t->isMatch() || t->isTimer()) {
    return false;
  }
  if (!(t->isObject() || t->isArray() || t->isDict()) || !isHeld(Kind::TypeNode, t)) {
    return false;
  }
  bool holds = false;
  if (t->isObject()) {
    for (const Field &f : t->fields()) {
      holds = holds || plan_.mayHoldMember(f.type);
    }
  } else if (t->isArray()) {
    holds = plan_.mayHoldMember(t->inner());
  } else {
    holds =
        plan_.mayHoldMember(t->keyType()) || (!t->isSet() && plan_.mayHoldMember(t->valueType()));
  }
  if (!holds) {
    return false;
  }
  bool changed = add(plan_.cyclicTypes, t);
  if (t->isObject() || t->isDict()) {
    plan_.anyCyclicObject = true;
  }
  if (t->isArray()) {
    plan_.cyclicArrayReps.insert(CycleFinder::representationOf(t));
  }

  return changed;
}

void HeldMembers::run() {
  for (const Type *t : scan_.reactionCallbacks) {
    callbacks_.push_back(finder_.fnNodeOf(t));
  }
  for (const Type *t : scan_.types) {
    if (t->isPromise()) {
      promised_.push_back({t->promised(), {}});
      finder_.refers(t->promised(), promised_.back().second);
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    findHeld();
    for (const Type *t : scan_.types) {
      if (addType(t)) {
        changed = true;
      }
    }
    for (const auto &[key, captures] : scan_.envs) {
      if (plan_.cyclicEnvs.contains(key) || !isHeld(Kind::EnvNode, key)) {
        continue;
      }
      bool holds = false;
      for (const VarSymbol *var : captures) {
        const Type *type = CycleFinder::valueTypeOf(var);
        holds = holds || (var->boxed() ? plan_.isCyclicBox(type) : plan_.mayHoldMember(type));
      }
      if (holds && plan_.cyclicEnvs.insert(key).second) {
        plan_.refMember.clear();
        changed = true;
      }
    }
    for (const VarSymbol *var : scan_.boxed) {
      const Type *type = CycleFinder::valueTypeOf(var);
      if (isHeld(Kind::BoxNode, type) && plan_.mayHoldMember(type) &&
          add(plan_.cyclicBoxes, type)) {
        changed = true;
      }
    }
  }
}

}
