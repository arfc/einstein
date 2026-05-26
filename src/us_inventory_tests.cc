#include <gtest/gtest.h>

#include "us_inventory.h"

#include "agent_tests.h"
#include "context.h"
#include "facility_tests.h"
#include "pyhooks.h"

using einstein::us_inventory;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
class us_inventoryTest : public ::testing::Test {
 protected:
  cyclus::TestContext tc;
  us_inventory* facility;

  virtual void SetUp() {
    cyclus::PyStart();
    facility = new us_inventory(tc.get());
  }

  virtual void TearDown() {
    delete facility;
    cyclus::PyStop();
  }
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(us_inventoryTest, InitialState) {
  // Test things about the initial state of the facility here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(us_inventoryTest, Print) {
  EXPECT_NO_THROW(std::string s = facility->str());
  // Test us_inventory specific aspects of the print method here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(us_inventoryTest, Tick) {
  ASSERT_NO_THROW(facility->Tick());
  // Test us_inventory specific behaviors of the Tick function here
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TEST_F(us_inventoryTest, Tock) {
  EXPECT_NO_THROW(facility->Tock());
  // Test us_inventory specific behaviors of the Tock function here
}


// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// Do Not Touch! Below section required for connection with Cyclus
cyclus::Agent* us_inventoryConstructor(cyclus::Context* ctx) {
  return new us_inventory(ctx);
}
// Required to get functionality in cyclus agent unit tests library
#ifndef CYCLUS_AGENT_TESTS_CONNECTED
int ConnectAgentTests();
static int cyclus_agent_tests_connected = ConnectAgentTests();
#define CYCLUS_AGENT_TESTS_CONNECTED cyclus_agent_tests_connected
#endif  // CYCLUS_AGENT_TESTS_CONNECTED
INSTANTIATE_TEST_SUITE_P(us_inventory, FacilityTests,
                        ::testing::Values(&us_inventoryConstructor));
INSTANTIATE_TEST_SUITE_P(us_inventory, AgentTests,
                        ::testing::Values(&us_inventoryConstructor));
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
