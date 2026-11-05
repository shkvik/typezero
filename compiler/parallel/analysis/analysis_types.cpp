#include "compiler/parallel/analysis/analysis_types.h"

namespace tz::parallel {

bool Pos::operator<(const Pos &o) const {
  return this->buffer != o.buffer ? this->buffer < o.buffer : this->offset < o.offset;
}

}
