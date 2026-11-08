#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

void ParallelModel::merge(Active &into, llvm::ArrayRef<unsigned> from) {
  for (unsigned s : from) {
    auto it = std::lower_bound(into.begin(), into.end(), s);
    if (it == into.end() || *it != s) {
      into.insert(it, s);
    }
  }
}

CallGraph::CallGraph(int functionCount)
    : outgoingEdges(functionCount),
      incomingEdges(functionCount) {}

int CallGraph::add(const CallEdge &edge) {
  int index = int(this->edges.size());
  this->edges.push_back(edge);
  this->outgoingEdges[edge.from].push_back(index);
  this->incomingEdges[edge.to].push_back(index);

  return index;
}

CallEdge &CallGraph::edge(int index) {
  return this->edges[index];
}

const CallEdge &CallGraph::edge(int index) const {
  return this->edges[index];
}

const std::vector<int> &CallGraph::outgoing(int function) const {
  return this->outgoingEdges[function];
}

const std::vector<int> &CallGraph::incoming(int function) const {
  return this->incomingEdges[function];
}

std::vector<CallEdge>::iterator CallGraph::begin() {
  return this->edges.begin();
}

std::vector<CallEdge>::iterator CallGraph::end() {
  return this->edges.end();
}

}
