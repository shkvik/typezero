#include "compiler/semantic/semantic_state/v2/declaration/declaration_order_context/declaration_order_context.h"

namespace tz {

unsigned DeclarationOrderContext::takeSequenceNumber() {
  this->sequenceNumber += 1;

  return this->sequenceNumber;
}

unsigned DeclarationOrderContext::getSequenceNumber() const {
  return this->sequenceNumber;
}

}
