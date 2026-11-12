#pragma once

#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <vector>

namespace tz {

template <class Node> using NodeChildren = llvm::SmallVector<Node, 4>;

class ChildrenFirst {
public:
  template <class Node>
  static std::vector<Node> order(Node root, NodeChildren<Node> (*childrenOf)(Node)) {
    auto nodes = std::vector<Node>();
    auto stack = std::vector<Node>{root};
    while (!stack.empty()) {
      Node next = stack.back();
      stack.pop_back();
      nodes.push_back(next);
      auto children = childrenOf(next);
      stack.insert(stack.end(), children.begin(), children.end());
    }
    std::reverse(nodes.begin(), nodes.end());

    return nodes;
  }

  template <class Node, class Owner>
  static std::vector<Node> order(Node root, const Owner *owner,
                                 NodeChildren<Node> (Owner::*childrenOf)(Node) const) {
    auto nodes = std::vector<Node>();
    auto stack = std::vector<Node>{root};
    while (!stack.empty()) {
      Node next = stack.back();
      stack.pop_back();
      nodes.push_back(next);
      auto children = (owner->*childrenOf)(next);
      stack.insert(stack.end(), children.begin(), children.end());
    }
    std::reverse(nodes.begin(), nodes.end());

    return nodes;
  }

  template <class Answer> static Answer takeAnswer(std::vector<Answer> &answers) {
    auto answer = std::move(answers.back());
    answers.pop_back();

    return answer;
  }

  template <class Answer>
  static std::vector<Answer> takeAnswers(std::vector<Answer> &answers, size_t count) {
    auto first = answers.end() - static_cast<std::ptrdiff_t>(count);
    auto taken =
        std::vector<Answer>(std::make_move_iterator(first), std::make_move_iterator(answers.end()));
    answers.erase(first, answers.end());

    return taken;
  }
};

}
