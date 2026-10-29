#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include <optional>
#include <string>

namespace tz {

struct KnowledgeRequest {
  std::string command;
  std::string kind;
  std::optional<std::string> target;
  size_t budget = 1500;
  bool json = false;
  bool library = false;
  bool brief = false;
};

class KnowledgeQuestion {
public:
  static bool answer(const Knowledge &knowledge, const KnowledgeRequest &request, std::string &out,
                     std::string &error);
};

}
