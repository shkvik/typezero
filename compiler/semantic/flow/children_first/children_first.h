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
    std::vector<Node> nodes;
    std::vector<Node> stack = {root};
    while (!stack.empty()) {
      Node next = stack.back();
      stack.pop_back();
      nodes.push_back(next);
      for (Node child : childrenOf(next)) {
        stack.push_back(child);
      }
    }
    std::reverse(nodes.begin(), nodes.end());

    return nodes;
  }

  template <class Answer> static Answer takeAnswer(std::vector<Answer> &answers) {
    Answer answer = std::move(answers.back());
    answers.pop_back();

    return answer;
  }

  template <class Answer>
  static std::vector<Answer> takeAnswers(std::vector<Answer> &answers, size_t count) {
    auto first = answers.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<Answer> taken(std::make_move_iterator(first),
                              std::make_move_iterator(answers.end()));
    answers.erase(first, answers.end());

    return taken;
  }
};

}
