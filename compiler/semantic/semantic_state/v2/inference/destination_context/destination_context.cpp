#include "compiler/semantic/semantic_state/v2/inference/destination_context/destination_context.h"

namespace tz {

void DestinationContext::rememberDestinations(const VarSymbol *variable, DestinationPlaces places) {
  this->destinations[variable] = std::move(places);
}

DestinationPlaces DestinationContext::getDestinations(const VarSymbol *variable) const {
  auto found = this->destinations.find(variable);
  if (found == this->destinations.end()) {
    return {};
  }

  return found->second;
}

}
