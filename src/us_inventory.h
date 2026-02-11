#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "cyclus.h"
#include "cyclus_facility.h"

class USInventory : public cyclus::Facility {
 public:
  USInventory(cyclus::Context* ctx);
  virtual ~USInventory();

  // --- Cyclus hooks ---
  virtual void EnterNotify();
  virtual std::string str();

  // --- DRE (Material supplier) ---
  virtual void GetMatlBids(cyclus::CommodMap<cyclus::Material>::type& commod_requests,
                           cyclus::BidPortfolio<cyclus::Material>::type& bids);

  virtual void GetMatlTrades(const std::vector< cyclus::Trade<cyclus::Material> >& trades,
                             std::vector< std::pair< cyclus::Trade<cyclus::Material>,
                                                    cyclus::Material::Ptr > >& responses);

  // --- Configurable parameters (set via XML) ---
  #pragma cyclus var {"tooltip":"Commodity this facility supplies (e.g., pwr_snf)."}
  std::string outcommod;

  #pragma cyclus var {"tooltip":"Path to assemblies.csv"}
  std::string assemblies_file;

  #pragma cyclus var {"tooltip":"Path to composition.csv"}
  std::string composition_file;

  #pragma cyclus var {"default":1e99, "tooltip":"Max kg this facility can supply per timestep."}
  double throughput_kg;

  #pragma cyclus var {"default":true, "tooltip":"Allow partial fulfillment (mass-based)."}
  bool allow_partial;

 private:
  struct Bin {
    std::string assembly_id;
    double available_kg = 0.0;  // remaining mass
    cyclus::Composition::Ptr comp;
  };

  // Storage: bins in FIFO order
  std::vector<Bin> bins_;

  // Fast lookup: assembly_id -> index in bins_
  std::unordered_map<std::string, size_t> idx_;

  // Helper: CSV loading
  void LoadAssembliesCSV_(const std::string& path);
  void LoadCompositionCSV_(const std::string& path);

  // Helper: Convert nuclide string (e.g., "U235") to nuc id (zzaaam)
  int NucIdFromString_(const std::string& s) const;
};