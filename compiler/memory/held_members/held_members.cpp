#include "compiler/memory/held_members/held_members.h"

namespace tz {

using Kind = CycleGraph::Kind;

HeldMembers::HeldMembers(MemoryPlan &plan, const CycleScanner &scan, CycleFinder &finder)
    : plan(plan),
      scan(scan),
      finder(finder) {}

bool HeldMembers::isMemberNode(unsigned v) const {
  const CycleGraph::Node &node = this->finder.graph.nodes[v];
  switch (node.kind) {
  case Kind::TypeNode:
    return this->plan.cyclicTypes.contains(static_cast<const Type *>(node.key));
  case Kind::EnvNode:
    return this->plan.cyclicEnvs.contains(node.key);
  case Kind::BoxNode:
    return this->plan.cyclicBoxes.contains(static_cast<const Type *>(node.key));
  default:
    return false;
  }
}

bool HeldMembers::passesOn(unsigned v) const {
  Kind kind = this->finder.graph.nodes[v].kind;

  return kind == Kind::FnNode || kind == Kind::AnyObjectNode || kind == Kind::UnknownNode;
}

void HeldMembers::findHeld() {
  const CycleGraph &graph = this->finder.graph;
  this->held.assign(graph.nodes.size(), false);
  std::vector<unsigned> work;
  std::vector<bool> seen(graph.nodes.size(), false);
  auto reach = [&](unsigned v) {
    this->held[v] = true;
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
  for (unsigned n : this->callbacks) {
    reach(n);
  }
  for (const auto &[value, to] : this->promised) {
    if (this->plan.promiseIsMember(value)) {
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
  int v = this->finder.nodeOf(kind, key);

  return v >= 0 && this->held[unsigned(v)];
}

bool HeldMembers::add(llvm::DenseSet<const Type *> &set, const Type *t) {
  if (!set.insert(t).second) {
    return false;
  }
  this->plan.refMember.clear();

  return true;
}

bool HeldMembers::addType(const Type *t) {
  if (this->plan.cyclicTypes.contains(t) || t->isWeakRef() || t->isAnyObject() ||
      t->isRuntimeObject() || t->isBytes() || t->isCrypto() || t->isIntl() || t->isDate() ||
      t->isRegExp() || t->isMatch() || t->isTimer()) {
    return false;
  }
  if (!(t->isObject() || t->isArray() || t->isDict()) || !isHeld(Kind::TypeNode, t)) {
    return false;
  }
  bool holds = false;
  if (t->isObject()) {
    for (const Field &f : t->fields()) {
      holds = holds || this->plan.mayHoldMember(f.type);
    }
  } else if (t->isArray()) {
    holds = this->plan.mayHoldMember(t->inner());
  } else {
    holds = this->plan.mayHoldMember(t->keyType()) ||
            (!t->isSet() && this->plan.mayHoldMember(t->valueType()));
  }
  if (!holds) {
    return false;
  }
  bool changed = add(this->plan.cyclicTypes, t);
  if (t->isObject() || t->isDict()) {
    this->plan.anyCyclicObject = true;
  }
  if (t->isArray()) {
    this->plan.cyclicArrayReps.insert(CycleFinder::representationOf(t));
  }

  return changed;
}

void HeldMembers::run() {
  for (const Type *t : this->scan.reactionCallbacks) {
    this->callbacks.push_back(this->finder.fnNodeOf(t));
  }
  for (const Type *t : this->scan.types) {
    if (t->isPromise()) {
      this->promised.push_back({t->promised(), {}});
      this->finder.refers(t->promised(), this->promised.back().second);
    }
  }
  for (bool changed = true; changed;) {
    changed = false;
    findHeld();
    for (const Type *t : this->scan.types) {
      if (addType(t)) {
        changed = true;
      }
    }
    for (const auto &[key, captures] : this->scan.envs) {
      if (this->plan.cyclicEnvs.contains(key) || !isHeld(Kind::EnvNode, key)) {
        continue;
      }
      bool holds = false;
      for (const VarSymbol *var : captures) {
        const Type *type = CycleFinder::valueTypeOf(var);
        holds =
            holds || (var->boxed() ? this->plan.isCyclicBox(type) : this->plan.mayHoldMember(type));
      }
      if (holds && this->plan.cyclicEnvs.insert(key).second) {
        this->plan.refMember.clear();
        changed = true;
      }
    }
    for (const VarSymbol *var : this->scan.boxed) {
      const Type *type = CycleFinder::valueTypeOf(var);
      if (isHeld(Kind::BoxNode, type) && this->plan.mayHoldMember(type) &&
          add(this->plan.cyclicBoxes, type)) {
        changed = true;
      }
    }
  }
}

}
