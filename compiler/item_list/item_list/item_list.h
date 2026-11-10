#pragma once

#include <vector>

namespace tz {

template <class T> class ItemList : public std::vector<T> {
public:
  using std::vector<T>::vector;

  void append(const std::vector<T> &more) {
    this->insert(this->end(), more.begin(), more.end());
  }
};

}
