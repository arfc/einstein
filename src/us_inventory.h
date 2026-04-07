#ifndef EINSTEIN_US_INVENTORY_H_
#define EINSTEIN_US_INVENTORY_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "cyclus.h"
#include "cyclus_facility.h"
#include "einstein_version.h"

namespace einstein {

class USInventory : public cyclus::Facility {
 public:
  explicit USInventory(cyclus::Context* ctx);
  virtual ~USInventory();

  #pragma cyclus decl

  virtual void InitFrom(USInventory* m);
  virtual void InitFrom(cyclus::QueryableBackend* b);

  /// Cyclus hooks
  virtual void EnterNotify();
  virtual std::string str();
  virtual void Tick();
  virtual void Tock();
  virtual std::string version() { return EINSTEIN_VERSION; }

  /// Material supplier interface
  virtual void GetMatlBids(
      cyclus::CommodMap<cyclus::Material>::type& commod_requests,
      cyclus::BidPortfolio<cyclus::Material>::type& bids);

  virtual void GetMatlTrades(
      const std::vector<cyclus::Trade<cyclus::Material> >& trades,
      std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                            cyclus::Material::Ptr> >& responses);

  #pragma cyclus note {"doc": "USInventory is a source-like facility that loads "
                              "spent nuclear fuel inventory data at initialization "
                              "and supplies material to other facilities on request. "
                              "It does not accept incoming commodities."}

  /// Output commodity
  #pragma cyclus var {"tooltip":"Commodity supplied by this facility.", \
                      "doc":"Commodity name offered to requesting facilities.", \
                      "uilabel":"Output Commodity", \
                      "uitype":"outcommodity"}
  std::string outcommod;

  /// File containing assembly inventory data
  #pragma cyclus var {"tooltip":"Path to assemblies CSV file."}
  std::string assemblies_file;

  /// File containing isotopic composition data
  #pragma cyclus var {"tooltip":"Path to composition CSV file."}
  std::string composition_file;

  /// Maximum mass supplied per timestep
  #pragma cyclus var {"default": 1e99, \
                      "tooltip":"Maximum mass supplied per timestep (kg).", \
                      "units":"kg"}
  double throughput_kg;

  /// Whether partial requests may be fulfilled
  #pragma cyclus var {"default": true, \
                      "tooltip":"Allow partial fulfillment of requests."}
  bool allow_partial;

  /// Bin selection policy
  #pragma cyclus var {"default":"first", \
                      "tooltip":"Bin selection policy: first, older, newer, highest_burnup, lowest_burnup, highest_enrichment, lowest_enrichment."}
  std::string selection_policy;

 protected:
  struct Bin {
    std::string assembly_id;
    double available_kg;
    cyclus::Composition::Ptr comp;

    int discharge_date;
    double burnup;
    double enrichment;

    Bin()
        : assembly_id(""),
          available_kg(0.0),
          comp(),
          discharge_date(0),
          burnup(0.0),
          enrichment(0.0) {}
  };

  /// Inventory bins loaded from the database
  std::vector<Bin> bins_;

  /// Fast lookup from assembly id to bin index
  std::unordered_map<std::string, size_t> idx_;

  /// Total available mass in all bins
  double total_inventory_kg_;

  /// Helper methods for loading data
  void LoadAssembliesCSV_(const std::string& path);
  void LoadCompositionCSV_(const std::string& path);

  /// Helper for choosing a bin according to policy
  size_t ChooseBin_(double req_qty, bool full_only) const;

  /// Helper for nuclide-name parsing
  int NucIdFromString_(const std::string& s) const;

  friend class USInventoryTest;

 private:
  // Code Injection
  #include "toolkit/matl_sell_policy.cycpp.h"
  #include "toolkit/position.cycpp.h"
};

}  // namespace einstein

#endif  // EINSTEIN_US_INVENTORY_H_