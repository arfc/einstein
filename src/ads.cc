#include "ads.h"

namespace einstein {

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
ads::ads(cyclus::Context* ctx) : cyclus::Facility(ctx) {}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
std::string ads::str() {
  return Facility::str();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void ads::Tick() {}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
void ads::Tock() {}

// WARNING! Do not change the following this function!!! This enables your
// archetype to be dynamically loaded and any alterations will cause your
// archetype to fail.
extern "C" cyclus::Agent* Constructads(cyclus::Context* ctx) {
  return new ads(ctx);
}

}  // namespace einstein
