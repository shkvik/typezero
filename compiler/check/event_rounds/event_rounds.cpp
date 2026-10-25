#include "compiler/check/event_rounds/event_rounds.h"

namespace tz {

namespace {

constexpr unsigned kRounds = 8;

} // namespace

bool EventRounds::again() {
  if (!inference_.open || inference_.final) {
    return false;
  }
  if (!inference_.changed || round_ + 2 >= kRounds) {
    inference_.final = true;
  }
  inference_.changed = inference_.open = false;
  ++round_;

  return true;
}

} // namespace tz
