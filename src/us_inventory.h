#ifndef EINSTEIN_SRC_US_INVENTORY_H_
#define EINSTEIN_SRC_US_INVENTORY_H_
 
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
 
#include "cyclus.h"
 
#pragma cyclus exec from cyclus.system import CY_LARGE_DOUBLE
 
namespace einstein {
 
/// This facility represents a supply inventory of used nuclear fuel assemblies.
/// It reads assembly metadata and isotopic compositions from a folder of
/// Standards 5.0.1 JSONL files (one file per facility), stores them internally
/// as inventory bins, attaches geographic coordinates to each assembly from a
/// coordinates database, and offers material on a configured output commodity.
/// Material is supplied from the available bins subject to a per-timestep
/// throughput limit and an optional facility/storage-type filter.
class us_inventory : public cyclus::Facility,
    public cyclus::toolkit::CommodityProducer {
 
 public:
  us_inventory(cyclus::Context* ctx);
  virtual ~us_inventory();
 
  #pragma cyclus note { \
    "doc": "This facility represents a supply inventory of used nuclear fuel " \
           "assemblies. It reads assembly metadata and isotopic compositions " \
           "from a folder of Standards 5.0.1 JSONL files, stores them " \
           "internally as inventory bins, attaches geographic coordinates to " \
           "each assembly, and offers material on a configured output " \
           "commodity. Material is supplied from the available bins subject to " \
           "a per-timestep throughput limit and an optional " \
           "facility/storage-type filter.", \
  }
 
  #pragma cyclus def clone
  #pragma cyclus def schema
  #pragma cyclus def annotations
  #pragma cyclus def infiletodb
  #pragma cyclus def snapshot
  #pragma cyclus def snapshotinv
  #pragma cyclus def initinv
 
  virtual void InitFrom(us_inventory* m);
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
    "doc": "Output commodity on which the us_inventory facility offers " \
           "used nuclear fuel material.", \
    "uilabel": "Output Commodity", \
    "uitype": "outcommodity", \
  }
  std::string outcommod;
 
  #pragma cyclus var { \
    "tooltip": "Folder holding the Standards 5.0.1 JSONL files.", \
    "doc": "Path to a folder. Every *.txt / *.jsonl file in it is read into " \
           "one aggregate inventory. Each line of each file is one assembly " \
           "with its metadata and grams composition.", \
    "uilabel": "Assembly Data Folder", \
  }
  std::string data_dir;
 
  #pragma cyclus var { \
    "default": "", \
    "tooltip": "Path to coordinates.sqlite.", \
    "doc": "Optional. If set, latitude/longitude are looked up by plant name " \
           "from the 'reactors_coordinates' table and attached to every " \
           "assembly. If empty, coordinates are left at (0,0).", \
    "uilabel": "Coordinates Database", \
  }
  std::string coordinates_file;
 
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
           "'first' - earliest bin with available material; " \
           "'older'/'newer' - smallest/largest discharge year; " \
           "'highest_burnup'/'lowest_burnup'; " \
           "'highest_enrichment'/'lowest_enrichment'; " \
           "'highest_initial_uranium'/'lowest_initial_uranium'; " \
           "'highest_fissile'/'lowest_fissile' (U233+U235+Pu239+Pu241 fraction); " \
           "'highest_plutonium'/'lowest_plutonium' (Pu mass fraction); " \
           "'highest_minor_actinide'/'lowest_minor_actinide' (Np+Am+Cm fraction).", \
    "uilabel": "Bin Selection Policy", \
  }
  std::string selection_policy;
 
  #pragma cyclus var { \
    "default": "", \
    "tooltip": "Only supply assemblies from this facility (optional).", \
    "doc": "If set, only assemblies whose facility_name matches are eligible. " \
           "If no eligible assembly can fill a trade, behavior depends on " \
           "strict_filters. Leave empty to consider all facilities.", \
    "uilabel": "Preferred Facility", \
  }
  std::string preferred_facility;
 
  #pragma cyclus var { \
    "default": "", \
    "tooltip": "Only supply assemblies of this storage type (optional).", \
    "doc": "If set, only assemblies whose storage_type_name matches are " \
           "eligible. Must be 'wet' or 'dry'. Leave empty to consider both.", \
    "uilabel": "Storage Type Filter", \
  }
  std::string storage_type;
 
  #pragma cyclus var { \
    "default": False, \
    "tooltip": "Treat preferred_facility/storage_type as hard filters.", \
    "doc": "If false (default, 'soft'), the facility prefers assemblies that " \
           "match the filters but falls back to the rest of the inventory " \
           "when no matching assembly can fill a trade (relaxing all filters " \
           "at once). If true ('strict'), only matching assemblies are ever " \
           "supplied; if none match, the facility supplies nothing and bids " \
           "only the matching inventory.", \
    "uilabel": "Strict Filters", \
  }
  bool strict_filters;
 
  #pragma cyclus var { \
    "default": [], \
    "doc": "Persisted remaining mass (kg) for each assembly bin. " \
           "Managed internally — do not set by hand.", \
    "uilabel": "Remaining Masses (internal)", \
  }
  std::vector<double> remaining_kg_;
 
  // ---------------------------------------------------------------------------
  // Internal data structures
  // ---------------------------------------------------------------------------
 
  /// One entry per assembly read from the JSONL data.
  struct Bin {
    std::string assembly_id;        // unique internal key (facility|loc|id|index)
    std::string facility;           // facility_name
    std::string location;           // storage_location_name
    std::string storage_type;       // storage_type_name ('wet' / 'dry')
    std::string orig_id;            // raw assembly_identifier
 
    double available_kg       = 0.0;
    double discharge_date     = 0.0;  // discharge_year; used by older/newer
    double burnup             = 0.0;  // used by highest/lowest_burnup
    double enrichment         = 0.0;  // used by highest/lowest_enrichment
    double initial_uranium_kg = 0.0;  // used by highest/lowest_initial_uranium
 
    // Precomputed composition fractions (computed once at load time).
    double fissile_frac = 0.0;        // (U233+U235+Pu239+Pu241) / total mass
    double pu_frac      = 0.0;        // all Pu / total mass
    double ma_frac      = 0.0;        // (Np+Am+Cm) / total mass
 
    // Geographic coordinates, attached from the coordinates database by plant.
    double latitude  = 0.0;
    double longitude = 0.0;
 
    cyclus::Composition::Ptr comp;
  };
 
  std::vector<Bin> bins_;
  std::unordered_map<std::string, size_t> idx_;  // assembly_id -> bins_ index
  double total_inventory_kg_;                     // running total for fast checks
 
  // Logged-once-per-run flag so soft fallback is visible but not flooding.
  bool fallback_warned_ = false;
 
  // ---------------------------------------------------------------------------
  // Private helpers
  // ---------------------------------------------------------------------------
 
  /// True if bin b satisfies the active preferred_facility/storage_type filters.
  bool PassesFilters_(const Bin& b) const;
 
  /// Return the index of the best bin for a trade of req_qty kg according to
  /// selection_policy. If full_only is true, only bins that can fully satisfy
  /// req_qty are considered. If apply_filters is true, only bins passing
  /// PassesFilters_ are considered. Returns bins_.size() if none is found.
  size_t ChooseBin_(double req_qty, bool full_only, bool apply_filters) const;
 
  /// Choose a bin for one trade within a pool (filtered or not), trying a
  /// fully-satisfying bin first and then (if allow_partial) a partial one.
  /// Sets *full to true if the chosen bin fully satisfies req_qty.
  size_t ChooseInPool_(double req_qty, bool pool_filtered, bool* full) const;
 
  /// Build a Composition that is a mass-weighted blend of the bins drawn from.
  /// draw_kg[i] is the mass drawn from bins_[i].
  cyclus::Composition::Ptr BlendedComp_(
      const std::vector<double>& draw_kg) const;
 
  void LoadAllData_(const std::string& dir);
  void LoadDataJSONL_(const std::string& path);
  void AttachCoordinates_();
  void RecordSupply_(const Bin& b, double qty);
 
  /// Convert a nuclide string (e.g. "U-235", "u-235", "92235") to a ZZAAAM id.
  int NucIdFromString_(const std::string& s) const;
};
 
}  // namespace einstein
 
 
#endif  // EINSTEIN_SRC_US_INVENTORY_H_
