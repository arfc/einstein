#include <gtest/gtest.h>

#include "ads.h"

#include "agent_tests.h"
#include "context.h"
#include "facility_tests.h"
#include "pyhooks.h"

using einstein::ads;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
class adsTest : public ::testing::Test {
 protected:
  cyclus::TestContext tc;
  ads* facility;

  virtual void SetUp() {
    cyclus::PyStart();
    facility = new ads(tc.get());
  }

  virtual void TearDown() {
    delete facility;
    cyclus::PyStop();
  }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(adsTest, InitialState) {
  // Test things about the initial state of the facility here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(adsTest, Print) {
  EXPECT_NO_THROW(std::string s = facility->str());
  // Test ads specific aspects of the print method here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(adsTest, Tick) {
  ASSERT_NO_THROW(facility->Tick());
  // Test ads specific behaviors of the Tick function here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(adsTest, Tock) {
  EXPECT_NO_THROW(facility->Tock());
  // Test ads specific behaviors of the Tock function here
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Do Not Touch! Below section required for connection with Cyclus
cyclus::Agent* adsConstructor(cyclus::Context* ctx) {
  return new ads(ctx);
}
// Required to get functionality in cyclus agent unit tests library
#ifndef CYCLUS_AGENT_TESTS_CONNECTED
int ConnectAgentTests();
static int cyclus_agent_tests_connected = ConnectAgentTests();
#define CYCLUS_AGENT_TESTS_CONNECTED cyclus_agent_tests_connected
#endif  // CYCLUS_AGENT_TESTS_CONNECTED
INSTANTIATE_TEST_SUITE_P(ads, FacilityTests,
                        ::testing::Values(&adsConstructor));
INSTANTIATE_TEST_SUITE_P(ads, AgentTests,
                        ::testing::Values(&adsConstructor));
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
