#ifndef EINSTEIN_SRC_US_INVENTORY_H_
#define EINSTEIN_SRC_US_INVENTORY_H_

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "cyclus.h"

#pragma cyclus exec from cyclus.system import CY_LARGE_DOUBLE

namespace einstein {

/// This facility represents a supply inventory of used nuclear fuel assemblies.
/// It reads assembly masses and isotopic compositions from CSV files, stores
/// them internally as inventory bins, and offers material on a configured
/// output commodity. Material is supplied from the available bins subject to a
/// per-timestep throughput limit.
class USInventory : public cyclus::Facility,
    public cyclus::toolkit::CommodityProducer,
    public cyclus::toolkit::Position {

 public:
  USInventory(cyclus::Context* ctx);
  virtual ~USInventory();

  #pragma cyclus note { \
    "doc": "This facility represents a supply inventory of used nuclear fuel " \
           "assemblies. It reads assembly masses and isotopic compositions " \
           "from CSV files, stores them internally as inventory bins, and " \
           "offers material on a configured output commodity. Material is " \
           "supplied from the available bins subject to a per-timestep " \
           "throughput limit.", \
  }

  #pragma cyclus def clone
  #pragma cyclus def schema
  #pragma cyclus def annotations
  #pragma cyclus def infiletodb
  #pragma cyclus def snapshot
  #pragma cyclus def snapshotinv
  #pragma cyclus def initinv

  virtual void InitFrom(USInventory* m);
  virtual void InitFrom(cyclus::QueryableBackend* b);

  virtual void Tick() {};

  virtual void Tock() {};

  virtual std::string str();
  virtual void EnterNotify();

  virtual std::set<cyclus::BidPortfolio<cyclus::Material>::Ptr>
  GetMatlBids(cyclus::CommodMap<cyclus::Material>::type& commod_requests);

  virtual void GetMatlTrades(
      const std::vector<cyclus::Trade<cyclus::Material> >& trades,
      std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                            cyclus::Material::Ptr> >& responses);

 private:
  // Cyclus state variables

  #pragma cyclus var { \
    "tooltip": "Commodity this facility supplies.", \
    "doc": "Output commodity on which the USInventory facility offers " \
           "used nuclear fuel material.", \
    "uilabel": "Output Commodity", \
    "uitype": "outcommodity", \
  }
  std::string outcommod;

  #pragma cyclus var { \
    "tooltip": "Path to assemblies CSV file.", \
    "doc": "Path to a CSV file containing assembly IDs and total available " \
           "masses. Required columns: assembly_id, total_mass_kg. " \
           "Optional columns: count, discharge_date, burnup, enrichment.", \
    "uilabel": "Assemblies File", \
  }
  std::string assemblies_file;

  #pragma cyclus var { \
    "tooltip": "Path to composition CSV file.", \
    "doc": "Path to a CSV file containing isotopic mass fractions for each " \
           "assembly. Required columns: assembly_id, nuclide, mass_fraction.", \
    "uilabel": "Composition File", \
  }
  std::string composition_file;

  #pragma cyclus var { \
    "default": CY_LARGE_DOUBLE, \
    "tooltip": "Maximum material mass this facility can supply per time step.", \
    "units": "kg/(time step)", \
    "uilabel": "Maximum Throughput", \
    "uitype": "range", \
    "range": [0.0, CY_LARGE_DOUBLE], \
    "doc": "Amount of commodity that can be supplied at each time step.", \
  }
  double throughput_kg;

  #pragma cyclus var { \
    "default": True, \
    "tooltip": "Allow partial fulfillment of material requests.", \
    "doc": "If true, the facility may partially satisfy a material trade when " \
           "the requested mass exceeds the available mass or remaining " \
           "throughput. If false, trades are only fulfilled when the full " \
           "requested quantity can be supplied.", \
    "uilabel": "Allow Partial Fulfillment", \
  }
  bool allow_partial;

  #pragma cyclus var { \
    "default": "first", \
    "tooltip": "Policy used to select which bin to draw from.", \
    "doc": "Controls which assembly bin is chosen when fulfilling a trade. " \
           "Options: " \
           "'first' - always pick the earliest bin with available material; " \
           "'older' - prefer the bin with the smallest discharge_date; " \
           "'newer' - prefer the bin with the largest discharge_date; " \
           "'highest_burnup' - prefer the bin with the highest burnup; " \
           "'lowest_burnup' - prefer the bin with the lowest burnup; " \
           "'highest_enrichment' - prefer the bin with the highest initial enrichment; " \
           "'lowest_enrichment' - prefer the bin with the lowest initial enrichment.", \
    "uilabel": "Bin Selection Policy", \
  }
  std::string selection_policy;

  #pragma cyclus var { \
    "default": [], \
    "doc": "Persisted remaining mass (kg) for each assembly bin. " \
           "Managed internally — do not set by hand.", \
    "uilabel": "Remaining Masses (internal)", \
  }
  std::vector<double> remaining_kg_;

  // ---------------------------------------------------------------------------
  // Internal (non-persisted) data structures
  // ---------------------------------------------------------------------------

  /// One entry per assembly (or assembly group) read from the CSV.
  struct Bin {
    std::string assembly_id;
    double available_kg   = 0.0;
    double discharge_date = 0.0;  // optional: used by older/newer policy
    double burnup         = 0.0;  // optional: used by highest/lowest_burnup
    double enrichment     = 0.0;  // optional: used by highest/lowest_enrichment
    cyclus::Composition::Ptr comp;
  };

  std::vector<Bin> bins_;
  std::unordered_map<std::string, size_t> idx_;  // assembly_id -> bins_ index
  double total_inventory_kg_;                     // running total for fast checks

  // ---------------------------------------------------------------------------
  // Private helpers
  // ---------------------------------------------------------------------------

  /// Return the index of the best bin for a trade of req_qty kg according to
  /// selection_policy. If full_only is true only bins that can fully satisfy
  /// req_qty are considered. Returns bins_.size() if no suitable bin is found.
  size_t ChooseBin_(double req_qty, bool full_only) const;

  /// Build a Composition that is a mass-weighted blend of the bins drawn from.
  /// draw_kg[i] is the mass drawn from bins_[i].
  cyclus::Composition::Ptr BlendedComp_(
      const std::vector<double>& draw_kg) const;

  void LoadAssembliesCSV_(const std::string& path);
  void LoadCompositionCSV_(const std::string& path);

  /// Convert a nuclide string (e.g. "U-235", "U235", "92235") to a PyNE id.
  int NucIdFromString_(const std::string& s) const;
};

}  // namespace einstein


#endif  // EINSTEIN_SRC_US_INVENTORY_H_

