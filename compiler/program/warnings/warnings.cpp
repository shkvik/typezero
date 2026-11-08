#include "compiler/program/warnings/warnings.h"

namespace tz {

void Warnings::add(Diagnostic diagnostic) {
  this->list.push_back(std::move(diagnostic));
}

std::vector<Diagnostic>::const_iterator Warnings::begin() const {
  return this->list.begin();
}

std::vector<Diagnostic>::const_iterator Warnings::end() const {
  return this->list.end();
}

}
