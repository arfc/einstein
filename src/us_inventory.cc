#include "us_inventory.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cycamore {

USInventory::USInventory(cyclus::Context* ctx)
    : cyclus::Facility(ctx),
      total_inventory_kg_(0.0) {}

USInventory::~USInventory() {}

// ----------------------------------------------------------------------
// Cyclus 
// ----------------------------------------------------------------------

#pragma cyclus def schema cycamore::USInventory
#pragma cyclus def annotations cycamore::USInventory
#pragma cyclus def initinv cycamore::USInventory
#pragma cyclus def snapshotinv cycamore::USInventory
#pragma cyclus def infiletodb cycamore::USInventory
#pragma cyclus def snapshot cycamore::USInventory
#pragma cyclus def clone cycamore::USInventory

void USInventory::InitFrom(USInventory* m) {
#pragma cyclus impl initfromcopy cycamore::USInventory
}

void USInventory::InitFrom(cyclus::QueryableBackend* b) {
#pragma cyclus impl initfromdb cycamore::USInventory
}

// ----------------------------------------------------------------------
// Basic facility methods
// ----------------------------------------------------------------------

std::string USInventory::str() {
  std::stringstream ss;
  ss << cyclus::Facility::str()
     << " USInventory(outcommod=" << outcommod
     << ", bins=" << bins_.size()
     << ", total_inventory_kg=" << total_inventory_kg_
     << ")";
  return ss.str();
}

void USInventory::EnterNotify() {
  cyclus::Facility::EnterNotify();

  if (outcommod.empty()) {
    throw cyclus::ValueError("USInventory: outcommod is required.");
  }

  if (assemblies_file.empty() || composition_file.empty()) {
    throw cyclus::ValueError(
        "USInventory: assemblies_file and composition_file are required.");
  }

  bins_.clear();
  idx_.clear();
  total_inventory_kg_ = 0.0;

  LoadAssembliesCSV_(assemblies_file);
  LoadCompositionCSV_(composition_file);

  // 
  for (size_t i = 0; i < bins_.size(); ++i) {
    if (bins_[i].comp == NULL) {
      throw cyclus::ValueError(
          "USInventory: missing composition for assembly_id=" +
          bins_[i].assembly_id);
    }
  }
}

void USInventory::Tick() {}

void USInventory::Tock() {}

// ----------------------------------------------------------------------
// DRE Methods
// ----------------------------------------------------------------------

void USInventory::GetMatlBids(
    cyclus::CommodMap<cyclus::Material>::type& commod_requests,
    cyclus::BidPortfolio<cyclus::Material>::type& bids) {

  cyclus::CommodMap<cyclus::Material>::type::iterator it =
      commod_requests.find(outcommod);

  if (it == commod_requests.end()) {
    return;
  }

  if (total_inventory_kg_ <= 0.0 || throughput_kg <= 0.0) {
    return;
  }

  // Pick one valid composition for bid offers.
  // This is just to create a valid offer object for the bid.
  cyclus::Composition::Ptr bid_comp = NULL;
  for (size_t i = 0; i < bins_.size(); ++i) {
    if (bins_[i].available_kg > 0.0 && bins_[i].comp != NULL) {
      bid_comp = bins_[i].comp;
      break;
    }
  }

  if (bid_comp == NULL) {
    return;
  }

  double remaining_bid_capacity = std::min(total_inventory_kg_, throughput_kg);

  std::vector<cyclus::Request<cyclus::Material>*>& reqs = it->second;
  for (size_t i = 0; i < reqs.size(); ++i) {
    cyclus::Request<cyclus::Material>* req = reqs[i];
    double req_qty = req->target()->quantity();

    if (req_qty <= 0.0 || remaining_bid_capacity <= 0.0) {
      continue;
    }

    double offer_qty = std::min(req_qty, remaining_bid_capacity);
    cyclus::Material::Ptr offer =
        cyclus::Material::CreateUntracked(offer_qty, bid_comp);

    bids.AddBid(req, offer, this, outcommod);

    // This prevents wildly overbidding across many requests.
    remaining_bid_capacity -= offer_qty;
  }
}

size_t USInventory::ChooseBin_(double req_qty, bool full_only) const {
  size_t best = bins_.size();

  for (size_t i = 0; i < bins_.size(); ++i) {
    const Bin& b = bins_[i];

    if (b.comp == NULL || b.available_kg <= 0.0) {
      continue;
    }

    if (full_only && b.available_kg < req_qty) {
      continue;
    }

    if (best == bins_.size()) {
      best = i;
      continue;
    }

    const Bin& cur = bins_[best];

    if (selection_policy == "first") {
      // keep the first eligible bin
      continue;
    } else if (selection_policy == "older") {
      if (b.discharge_date < cur.discharge_date) {
        best = i;
      }
    } else if (selection_policy == "newer") {
      if (b.discharge_date > cur.discharge_date) {
        best = i;
      }
    } else if (selection_policy == "highest_burnup") {
      if (b.burnup > cur.burnup) {
        best = i;
      }
    } else if (selection_policy == "lowest_burnup") {
      if (b.burnup < cur.burnup) {
        best = i;
      }
    } else if (selection_policy == "highest_enrichment") {
      if (b.enrichment > cur.enrichment) {
        best = i;
      }
    } else if (selection_policy == "lowest_enrichment") {
      if (b.enrichment < cur.enrichment) {
        best = i;
      }
    } else {
      // fallback if user gives unknown policy
      continue;
    }
  }

  return best;
}

void USInventory::GetMatlTrades(
    const std::vector<cyclus::Trade<cyclus::Material> >& trades,
    std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                          cyclus::Material::Ptr> >& responses) {

  double remaining_throughput = throughput_kg;

  for (size_t t = 0; t < trades.size(); ++t) {
    const cyclus::Trade<cyclus::Material>& tr = trades[t];
    double req_qty = tr.amt;

    if (req_qty <= 0.0) {
      continue;
    }

    if (remaining_throughput <= 0.0 || total_inventory_kg_ <= 0.0) {
      break;
    }

    size_t chosen_i = ChooseBin_(req_qty, true);
    double actual = 0.0;

    if (chosen_i != bins_.size()) {
      actual = std::min(req_qty, remaining_throughput);

      if (!allow_partial && actual < req_qty) {
        continue;
      }
    } else {
      if (!allow_partial) {
        continue;
      }

      chosen_i = ChooseBin_(req_qty, false);
      if (chosen_i == bins_.size()) {
        continue;
      }

      actual = std::min(req_qty,
                        std::min(bins_[chosen_i].available_kg,
                                 remaining_throughput));
    }

    if (chosen_i == bins_.size() || actual <= 0.0) {
      continue;
    }

    Bin& b = bins_[chosen_i];

    actual = std::min(actual, b.available_kg);
    actual = std::min(actual, remaining_throughput);

    if (actual <= 0.0) {
      continue;
    }

    b.available_kg -= actual;
    total_inventory_kg_ -= actual;
    remaining_throughput -= actual;

    cyclus::Material::Ptr mat =
        cyclus::Material::Create(context(), actual, b.comp);

    responses.push_back(std::make_pair(tr, mat));
  }
}

// ----------------------------------------------------------------------
// CSV Helpers
// ----------------------------------------------------------------------

namespace {

std::vector<std::string> SplitCSVLine(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;

  while (std::getline(ss, item, ',')) {
    item.erase(item.begin(),
               std::find_if(item.begin(), item.end(),
                            [](unsigned char ch) { return !std::isspace(ch); }));

    item.erase(std::find_if(item.rbegin(), item.rend(),
                            [](unsigned char ch) { return !std::isspace(ch); })
                   .base(),
               item.end());

    out.push_back(item);
  }

  return out;
}

}  // namespace

void USInventory::LoadAssembliesCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) {
    throw cyclus::ValueError("USInventory: cannot open " + path);
  }

  std::string header;
  if (!std::getline(f, header)) {
    throw cyclus::ValueError("USInventory: empty file " + path);
  }

  std::vector<std::string> cols = SplitCSVLine(header);

  int i_id = -1;
  int i_mass = -1;
  int i_count = -1;
  int i_date = -1;
  int i_bu = -1;
  int i_enr = -1;

  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i] == "assembly_id") {
      i_id = i;
    } else if (cols[i] == "total_mass_kg") {
      i_mass = i;
    } else if (cols[i] == "count") {
      i_count = i;
    } else if (cols[i] == "discharge_date") {
      i_date = i;
    } else if (cols[i] == "burnup") {
      i_bu = i;
    } else if (cols[i] == "enrichment") {
      i_enr = i;
    }
  }

  if (i_id < 0 || i_mass < 0) {
    throw cyclus::ValueError(
        "USInventory: assemblies.csv must contain assembly_id and total_mass_kg");
  }

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> v = SplitCSVLine(line);
    if ((int)v.size() <= std::max(i_id, i_mass)) {
      continue;
    }

    Bin b;
    b.assembly_id = v[i_id];

    double mass = std::stod(v[i_mass]);
    double count = 1.0;

    if (i_count >= 0 && i_count < (int)v.size() && !v[i_count].empty()) {
      count = std::stod(v[i_count]);
    }

    b.available_kg = mass * count;

    if (i_date >= 0 && i_date < (int)v.size() && !v[i_date].empty()) {
      b.discharge_date = std::stoi(v[i_date]);
    }

    if (i_bu >= 0 && i_bu < (int)v.size() && !v[i_bu].empty()) {
      b.burnup = std::stod(v[i_bu]);
    }

    if (i_enr >= 0 && i_enr < (int)v.size() && !v[i_enr].empty()) {
      b.enrichment = std::stod(v[i_enr]);
    }

    idx_[b.assembly_id] = bins_.size();
    bins_.push_back(b);
    total_inventory_kg_ += b.available_kg;
  }

  if (bins_.empty()) {
    throw cyclus::ValueError("USInventory: no rows loaded from " + path);
  }
}

void USInventory::LoadCompositionCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) {
    throw cyclus::ValueError("USInventory: cannot open " + path);
  }

  std::string header;
  if (!std::getline(f, header)) {
    throw cyclus::ValueError("USInventory: empty file " + path);
  }

  std::vector<std::string> cols = SplitCSVLine(header);

  int i_id = -1;
  int i_nuc = -1;
  int i_frac = -1;

  for (size_t i = 0; i < cols.size(); ++i) {
    if (cols[i] == "assembly_id") {
      i_id = i;
    } else if (cols[i] == "nuclide") {
      i_nuc = i;
    } else if (cols[i] == "mass_fraction") {
      i_frac = i;
    }
  }

  if (i_id < 0 || i_nuc < 0 || i_frac < 0) {
    throw cyclus::ValueError(
        "USInventory: composition.csv must contain assembly_id, nuclide, mass_fraction");
  }

  std::unordered_map<std::string, cyclus::CompMap> compmaps;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) {
      continue;
    }

    std::vector<std::string> v = SplitCSVLine(line);
    if ((int)v.size() <= std::max(i_id, std::max(i_nuc, i_frac))) {
      continue;
    }

    std::string aid = v[i_id];
    std::string nuc = v[i_nuc];
    double frac = std::stod(v[i_frac]);

    if (frac <= 0.0) {
      continue;
    }

    int nid = NucIdFromString_(nuc);
    compmaps[aid][nid] += frac;
  }

  for (std::unordered_map<std::string, cyclus::CompMap>::iterator it =
           compmaps.begin();
       it != compmaps.end(); ++it) {
    std::unordered_map<std::string, size_t>::iterator idx_it =
        idx_.find(it->first);

    if (idx_it == idx_.end()) {
      continue;
    }

    bins_[idx_it->second].comp =
        cyclus::Composition::CreateFromMass(it->second);
  }
}

// ----------------------------------------------------------------------
// Nuclide Parsing
// should use material class instead
// ----------------------------------------------------------------------

int USInventory::NucIdFromString_(const std::string& s) const {
  std::string t = s;

  t.erase(std::remove_if(t.begin(), t.end(),
                         [](unsigned char c) {
                           return c == '-' || std::isspace(c);
                         }),
          t.end());

  if (t.empty()) {
    throw cyclus::ValueError("Bad nuclide string: '" + s + "'");
  }

  size_t pos = 0;
  while (pos < t.size() && std::isalpha(static_cast<unsigned char>(t[pos]))) {
    ++pos;
  }

  if (pos == 0 || pos == t.size()) {
    throw cyclus::ValueError("Bad nuclide string: '" + s + "'");
  }

  std::string sym = t.substr(0, pos);
  std::string a_str = t.substr(pos);

  sym[0] = std::toupper(static_cast<unsigned char>(sym[0]));
  for (size_t i = 1; i < sym.size(); ++i) {
    sym[i] = std::tolower(static_cast<unsigned char>(sym[i]));
  }

  int A = std::stoi(a_str);
  if (A <= 0) {
    throw cyclus::ValueError("Bad mass number in nuclide: '" + s + "'");
  }
  // example of isotopes definision till I add the actual ones, or find out how to use materials class
  static const std::unordered_map<std::string, int> Z = {
      {"H", 1},   {"He", 2},  {"Li", 3},  {"Be", 4},  {"B", 5},   {"C", 6},
      };

  std::unordered_map<std::string, int>::const_iterator it = Z.find(sym);
  if (it == Z.end()) {
    throw cyclus::ValueError(
        "Unknown element symbol in nuclide: '" + s + "'");
  }

  int z = it->second;
  int zzaaam = z * 10000 + A * 10;
  return zzaaam;
}



extern "C" cyclus::Agent* ConstructUSInventory(cyclus::Context* ctx) {
  return new USInventory(ctx);
}

}  // namespace cycamore