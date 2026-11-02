#ifndef CYCLUS_EINSTEIN_ADS_H_
#define CYCLUS_EINSTEIN_ADS_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "cyclus.h"

#pragma cyclus exec from cyclus.system import CY_LARGE_DOUBLE, CY_LARGE_INT

namespace einstein {

/// @class ads
///
/// Accelerator-Driven System (ADS) / fast burner facility.
///
/// At each cycle the facility:
///   1. receives fuel into a feed buffer from one or more input commodities
///      (e.g. an inventory module and/or a separations facility),
///   2. loads one core-worth of feed and irradiates it for `cycle_time`,
///   3. transforms the core's composition by matching it (plus the configured
///      power level) against a library of pre-computed Serpent depletion cases
///      and linearly interpolating that case's composition to the irradiation
///      time implied by the cycle length, and
///   4. discharges the transmuted material on `outcommod`.
///
/// The transformed composition contains BOTH the surviving actinides and the
/// fission products generated during irradiation, both taken directly from the
/// Serpent depletion data. Mass is therefore conserved up to the small defect
/// carried off by escaping neutrons and released as energy (~3-4% of the
/// fissioned mass), which is recorded in the AdsEvents table.
class ads : public cyclus::Facility,
            public cyclus::toolkit::CommodityProducer {
 public:
  explicit ads(cyclus::Context* ctx);
  virtual ~ads();

  #pragma cyclus note {"doc": "Accelerator-Driven System (ADS) burner. " \
      "Receives fuel from an inventory and/or separations facility, irradiates " \
      "a core for a configured cycle time, transforms its composition using a " \
      "library of pre-computed Serpent depletion cases (nearest case, linearly " \
      "interpolated in irradiation time), and discharges the surviving " \
      "actinides together with the fission products produced during the cycle."}

  #pragma cyclus def clone
  #pragma cyclus def schema
  #pragma cyclus def annotations
  #pragma cyclus def infiletodb
  #pragma cyclus def snapshot
  #pragma cyclus def snapshotinv
  #pragma cyclus def initinv

  virtual void InitFrom(ads* m);
  virtual void InitFrom(cyclus::QueryableBackend* b);

  virtual std::string str();
  virtual void EnterNotify();

  virtual void Tick();
  virtual void Tock();

  virtual std::set<cyclus::RequestPortfolio<cyclus::Material>::Ptr>
  GetMatlRequests();

  virtual void AcceptMatlTrades(
      const std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                                  cyclus::Material::Ptr> >& responses);

  virtual std::set<cyclus::BidPortfolio<cyclus::Material>::Ptr>
  GetMatlBids(cyclus::CommodMap<cyclus::Material>::type& commod_requests);

  virtual void GetMatlTrades(
      const std::vector<cyclus::Trade<cyclus::Material> >& trades,
      std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                            cyclus::Material::Ptr> >& responses);

 private:
  // ---------------------------------------------------------------------------
  //  Cyclus state variables (input-file configurable)
  // ---------------------------------------------------------------------------

  #pragma cyclus var { \
    "tooltip": "Feed commodities to request fuel on.", \
    "doc": "Ordered list of input commodities the ADS requests fuel on. " \
           "Typically the inventory module's output and/or the separations " \
           "facility's product stream.", \
    "uilabel": "Feed Commodity List", \
    "uitype": ["oneormore", "incommodity"], \
  }
  std::vector<std::string> incommods;

  #pragma cyclus var { \
    "default": [], \
    "tooltip": "Request preference per feed commodity (same order).", \
    "doc": "Optional. Request preference for each feed commodity, in the same " \
           "order as incommods. Defaults to 1.0 for all if unspecified.", \
    "uilabel": "Feed Commodity Preferences", \
  }
  std::vector<double> incommod_prefs;

  #pragma cyclus var { \
    "default": "", \
    "tooltip": "Recipe name used in feed requests (optional).", \
    "doc": "Name of a recipe to attach to feed requests. Empty uses a blank " \
           "material so the ADS accepts whatever composition is offered.", \
    "uilabel": "Feed Request Recipe", \
    "uitype": "inrecipe", \
  }
  std::string feed_recipe;

  #pragma cyclus var { \
    "tooltip": "Output commodity for transmuted material.", \
    "doc": "Commodity on which discharged material (surviving actinides plus " \
           "fission products) is offered, destined for storage or " \
           "reprocessing.", \
    "uilabel": "Output Commodity", \
    "uitype": "outcommodity", \
  }
  std::string outcommod;

  // --- ADS design parameters -------------------------------------------------

  #pragma cyclus var { \
    "tooltip": "Core mass loaded and irradiated per cycle.", \
    "doc": "Mass of fuel loaded into the core for one irradiation cycle (kg). " \
           "Should match the initial actinide mass of the intended Serpent " \
           "case (the day-0 total of its actinide mass table).", \
    "uilabel": "Core Capacity", \
    "units": "kg", \
    "uitype": "range", \
    "range": [0.0, CY_LARGE_DOUBLE], \
  }
  double capacity;

  #pragma cyclus var { \
    "tooltip": "Irradiation cycle length in time steps.", \
    "doc": "Number of time steps a core is irradiated before discharge. " \
           "cycle_time * days_per_step is the irradiation time at which the " \
           "matched Serpent case is read.", \
    "uilabel": "Cycle Time", \
    "units": "time steps", \
  }
  int cycle_time;

  #pragma cyclus var { \
    "default": 0, \
    "tooltip": "Idle time steps between discharge and next load.", \
    "doc": "Number of time steps the core sits empty for refueling between " \
           "the discharge of one core and the loading of the next.", \
    "uilabel": "Refueling Time", \
    "units": "time steps", \
  }
  int refuel_time;

  #pragma cyclus var { \
    "tooltip": "Thermal power level used for case matching.", \
    "doc": "Thermal power level of the ADS (MW). Used as the primary key when " \
           "matching the core against the Serpent case library.", \
    "uilabel": "Power Level", \
    "units": "MW", \
    "uitype": "range", \
    "range": [0.0, CY_LARGE_DOUBLE], \
  }
  double power_level;

  #pragma cyclus var { \
    "default": 0.0, \
    "tooltip": "Accelerator beam rate (recorded; not used in the transform).", \
    "doc": "Beam rate design parameter. Recorded for output but does not drive " \
           "the composition transformation.", \
    "uilabel": "Beam Rate", \
  }
  double beam_rate;

  #pragma cyclus var { \
    "default": 0.0, \
    "tooltip": "Core volume (recorded; not used in the transform).", \
    "doc": "Core volume design parameter. Recorded for output but does not " \
           "drive the composition transformation.", \
    "uilabel": "Volume", \
  }
  double volume;

  // --- Depletion case library ------------------------------------------------

  #pragma cyclus var { \
    "tooltip": "Folder of Serpent depletion cases.", \
    "doc": "Path to a folder. Either the folder itself is one case, or each " \
           "immediate subfolder is one case. A case must contain an actinide " \
           "mass table (plot_data_15_actinide_masses_full.txt, or " \
           "plot_data_06_actinide_masses.txt) and should contain a fission " \
           "product mass table (plot_data_14_fission_product_masses.txt) on " \
           "the same time axis. A power file (plot_data_03_power_vs_time.txt) " \
           "is used for case matching if present.", \
    "uilabel": "Serpent Case Library Folder", \
  }
  std::string case_dir;

  #pragma cyclus var { \
    "default": -1.0, \
    "tooltip": "Irradiation days represented by one Cyclus time step.", \
    "doc": "Conversion factor between Cyclus time steps and the Serpent case " \
           "'Day [d]' axis: cycle_time steps -> cycle_time * days_per_step " \
           "days of irradiation, at which point the matched case composition " \
           "is read. If negative (default), it is derived from the simulation " \
           "time-step duration (context dt).", \
    "uilabel": "Days per Time Step", \
    "units": "d", \
  }
  double days_per_step;

  // ---------------------------------------------------------------------------
  //  Inventory buffers
  // ---------------------------------------------------------------------------

  #pragma cyclus var {"capacity": "capacity", \
    "tooltip": "Fresh feed awaiting core load."}
  cyclus::toolkit::ResBuf<cyclus::Material> feed;

  #pragma cyclus var {"capacity": "capacity", \
    "tooltip": "Material currently in core / being irradiated."}
  cyclus::toolkit::ResBuf<cyclus::Material> core;

  #pragma cyclus var { \
    "tooltip": "Discharged material awaiting trade out."}
  cyclus::toolkit::ResBuf<cyclus::Material> product;

  // ---------------------------------------------------------------------------
  //  Cycle bookkeeping (persisted)
  // ---------------------------------------------------------------------------

  #pragma cyclus var { \
    "default": 0, \
    "doc": "Steps elapsed in the current irradiation cycle. Managed " \
           "internally; do not set by hand.", \
    "internal": True, \
  }
  int cycle_step;

  #pragma cyclus var { \
    "default": 0, \
    "doc": "Steps elapsed in the current refueling (idle) period. Managed " \
           "internally; do not set by hand.", \
    "internal": True, \
  }
  int refuel_step;

  // ---------------------------------------------------------------------------
  //  In-memory Serpent case library (rebuilt at EnterNotify, not persisted)
  // ---------------------------------------------------------------------------

  /// One Serpent depletion case loaded from the library folder.
  struct DepletionCase {
    std::string name;
    double power_mw = 0.0;

    std::vector<double> time;   // irradiation day axis
    std::vector<double> keff;   // optional, recorded only

    /// Combined actinide + fission-product masses (kg) at each time index.
    std::vector<cyclus::CompMap> comp_series;

    /// Component totals (kg) at each time index, for mass bookkeeping.
    std::vector<double> actinide_kg;
    std::vector<double> fp_kg;
    std::vector<double> total_kg;

    // Fingerprint of the input (t=0) actinide vector, used for matching.
    double in_ma_frac = 0.0;   // (Np+Am+Cm) / actinide mass at t=0
    double in_pu_frac = 0.0;   // Pu / actinide mass at t=0
    double in_u_frac  = 0.0;   // U / actinide mass at t=0
  };

  std::vector<DepletionCase> library_;

  // --- helpers ---------------------------------------------------------------

  void LoadLibrary_(const std::string& dir);
  DepletionCase LoadCase_(const std::string& case_path,
                          const std::string& case_name);

  /// Read a "Step / Day [d] / <nuclide>_kg" table. Appends the day axis into
  /// *days and one CompMap (nuclide id -> kg) per row into *series. Columns
  /// ending in _delta_kg and unrecognized nuclides are skipped. Returns false
  /// if the file could not be read.
  bool ReadMassTable_(const std::string& path, const std::string& case_name,
                      std::vector<double>* days,
                      std::vector<cyclus::CompMap>* series);

  /// Index of the best-matching case for an input composition + power level.
  size_t MatchCase_(const cyclus::CompMap& input_mass, double power) const;

  /// Composition (actinides + fission products) the matched case holds after
  /// `day` days of irradiation, linearly interpolated on the case time axis.
  /// Sets *mass_ratio to total(day)/total(0) and *actinide_frac to the
  /// actinide mass fraction of the returned composition.
  cyclus::Composition::Ptr OutputComp_(const DepletionCase& c, double day,
                                       double* mass_ratio,
                                       double* actinide_frac) const;

  /// Pop the core, transform it, and push the result into the product buffer.
  void Discharge_();

  /// Irradiation days represented by one Cyclus time step.
  double DaysPerStep_() const;

  std::string BaseName_(const std::string& path) const;
  std::string FindFile_(const std::string& dir, const std::string& needle) const;
  bool EndsWith_(const std::string& s, const std::string& suf) const;

  void Record_(const std::string& event, const std::string& matched_case,
               double value);

  /// Parse a nuclide token ("Pu239", "Pu-239", "Am242m", ...) to a cyclus
  /// (ZZAAAM) nuclide id. Returns 0 if the token is not a valid nuclide.
  int NucId_(const std::string& tok) const;
};

}  // namespace einstein

#endif  // CYCLUS_EINSTEIN_ADS_H_
