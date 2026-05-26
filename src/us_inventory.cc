#include "us_inventory.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

//#include "pyne/nucname.h"

namespace einstein {

us_inventory::us_inventory(cyclus::Context* ctx)
    : cyclus::Facility(ctx),
      total_inventory_kg_(0.0) {}

us_inventory::~us_inventory() {}

void us_inventory::InitFrom(us_inventory* m) {
  #pragma cyclus impl initfromcopy einstein::us_inventory
  cyclus::toolkit::CommodityProducer::Copy(m);
}

void us_inventory::InitFrom(cyclus::QueryableBackend* b) {
  #pragma cyclus impl initfromdb einstein::us_inventory
  namespace tk = cyclus::toolkit;
  tk::CommodityProducer::Add(tk::Commodity(outcommod),
                             tk::CommodInfo(throughput_kg, throughput_kg));
}

std::string us_inventory::str() {
  std::stringstream ss;
  ss << cyclus::Facility::str()
     << " us_inventory(outcommod=" << outcommod
     << ", bins=" << bins_.size()
     << ", total_inventory_kg=" << total_inventory_kg_
     << ", throughput_kg=" << throughput_kg
     << ", selection_policy=" << selection_policy
     << ")";
  return ss.str();
}

void us_inventory::EnterNotify() {
  cyclus::Facility::EnterNotify();

  if (outcommod.empty()) {
    throw cyclus::ValueError("us_inventory: outcommod is required.");
  }
  if (assemblies_file.empty() || composition_file.empty()) {
    throw cyclus::ValueError(
        "us_inventory: assemblies_file and composition_file are required.");
  }

  // Validating the selection_policy early so the user gets a clear error.
  static const char* valid_policies[] = {
      "first", "older", "newer",
      "highest_burnup", "lowest_burnup",
      "highest_enrichment", "lowest_enrichment"};
  bool policy_ok = false;
  for (size_t i = 0; i < 7; ++i) {
    if (selection_policy == valid_policies[i]) { policy_ok = true; break; }
  }
  if (!policy_ok) {
    throw cyclus::ValueError(
        "us_inventory: unknown selection_policy '" + selection_policy + "'. "
        "Valid options: first, older, newer, highest_burnup, lowest_burnup, "
        "highest_enrichment, lowest_enrichment.");
  }

  bins_.clear();
  idx_.clear();
  total_inventory_kg_ = 0.0;

  LoadAssembliesCSV_(assemblies_file);
  LoadCompositionCSV_(composition_file);


  if (!remaining_kg_.empty()) {
    if (remaining_kg_.size() != bins_.size()) {
      throw cyclus::ValueError(
          "us_inventory: persisted remaining_kg_ length does not match "
          "the number of rows in assemblies_file. Did the CSV change "
          "after a checkpoint?");
    }
    total_inventory_kg_ = 0.0;
    for (size_t i = 0; i < bins_.size(); ++i) {
      bins_[i].available_kg = remaining_kg_[i];
      total_inventory_kg_ += remaining_kg_[i];
    }
  } else {
    // First build: snapshot the initial CSV masses.
    remaining_kg_.resize(bins_.size());
    for (size_t i = 0; i < bins_.size(); ++i) {
      remaining_kg_[i] = bins_[i].available_kg;
    }
  }

  for (size_t i = 0; i < bins_.size(); ++i) {
    if (bins_[i].comp == NULL) {
      throw cyclus::ValueError(
          "us_inventory: missing composition for assembly_id=" +
          bins_[i].assembly_id);
    }
  }
}

std::set<cyclus::BidPortfolio<cyclus::Material>::Ptr>
us_inventory::GetMatlBids(
    cyclus::CommodMap<cyclus::Material>::type& commod_requests) {
  using cyclus::BidPortfolio;
  using cyclus::CapacityConstraint;
  using cyclus::Material;
  using cyclus::Request;

  std::set<BidPortfolio<Material>::Ptr> ports;

  if (commod_requests.count(outcommod) == 0) {
    return ports;
  }

  double max_qty = std::min(total_inventory_kg_, throughput_kg);
  if (max_qty <= cyclus::eps()) {
    return ports;
  }

  cyclus::CompMap blended;
  double blend_total = 0.0;
  for (size_t i = 0; i < bins_.size(); ++i) {
    const Bin& b = bins_[i];
    if (b.available_kg <= cyclus::eps() || b.comp == NULL) continue;
    const cyclus::CompMap& cm = b.comp->mass();
    for (cyclus::CompMap::const_iterator it = cm.begin(); it != cm.end(); ++it) {
      blended[it->first] += it->second * b.available_kg;
    }
    blend_total += b.available_kg;
  }

  if (blend_total <= cyclus::eps()) {
    return ports;
  }

  for (cyclus::CompMap::iterator it = blended.begin();
       it != blended.end(); ++it) {
    it->second /= blend_total;
  }

  cyclus::Composition::Ptr bid_comp =
      cyclus::Composition::CreateFromMass(blended);

  BidPortfolio<Material>::Ptr port(new BidPortfolio<Material>());
  std::vector<Request<Material>*>& requests = commod_requests[outcommod];

  for (size_t i = 0; i < requests.size(); ++i) {
    Request<Material>* req = requests[i];
    double qty = std::min(req->target()->quantity(), max_qty);
    if (qty <= cyclus::eps()) continue;

    // When partial fulfillment is forbidden, only bid if we can satisfy the
    // full request.
    if (!allow_partial && qty < req->target()->quantity()) continue;

    Material::Ptr offer = Material::CreateUntracked(qty, bid_comp);
    port->AddBid(req, offer, this);
  }

  CapacityConstraint<Material> cc(max_qty);
  port->AddConstraint(cc);
  ports.insert(port);
  return ports;
}

// Choosing a Bin/ SNF assembly


size_t us_inventory::ChooseBin_(double req_qty, bool full_only) const {
  size_t best = bins_.size();

  for (size_t i = 0; i < bins_.size(); ++i) {
    const Bin& b = bins_[i];

    if (b.comp == NULL || b.available_kg <= cyclus::eps()) continue;
    if (full_only && b.available_kg < req_qty) continue;

    // First eligible bin — always accept it as the initial candidate.
    if (best == bins_.size()) {
      best = i;
      continue;
    }

    const Bin& cur = bins_[best];

    if (selection_policy == "first") {
      // Keep the first eligible bin; no further comparison needed.
      continue;
    } else if (selection_policy == "older") {
      if (b.discharge_date < cur.discharge_date) best = i;
    } else if (selection_policy == "newer") {
      if (b.discharge_date > cur.discharge_date) best = i;
    } else if (selection_policy == "highest_burnup") {
      if (b.burnup > cur.burnup) best = i;
    } else if (selection_policy == "lowest_burnup") {
      if (b.burnup < cur.burnup) best = i;
    } else if (selection_policy == "highest_enrichment") {
      if (b.enrichment > cur.enrichment) best = i;
    } else if (selection_policy == "lowest_enrichment") {
      if (b.enrichment < cur.enrichment) best = i;
    }
  }

  return best;
}


void us_inventory::GetMatlTrades(
    const std::vector<cyclus::Trade<cyclus::Material> >& trades,
    std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                          cyclus::Material::Ptr> >& responses) {
  double remaining_throughput = throughput_kg;

  for (size_t t = 0; t < trades.size(); ++t) {
    const cyclus::Trade<cyclus::Material>& tr = trades[t];
    double req_qty = tr.amt;

    if (req_qty <= cyclus::eps()) continue;
    if (remaining_throughput <= cyclus::eps()) break;
    if (total_inventory_kg_ <= cyclus::eps()) break;

    // --- Determine how much to send and which bin to draw from -------------

    // First try to find a single bin that can satisfy the full request.
    size_t chosen_i = ChooseBin_(req_qty, /*full_only=*/true);
    double actual = 0.0;

    if (chosen_i != bins_.size()) {
      // A bin with enough material was found.
      actual = std::min(req_qty, remaining_throughput);
      if (!allow_partial && actual < req_qty) continue;
    } else {
      // No single bin can fully satisfy the request.
      if (!allow_partial) continue;

      // Partial mode: pick the best bin that has anything at all.
      chosen_i = ChooseBin_(req_qty, /*full_only=*/false);
      if (chosen_i == bins_.size()) continue;

      actual = std::min({req_qty,
                         bins_[chosen_i].available_kg,
                         remaining_throughput});
    }

    if (actual <= cyclus::eps()) continue;

    // --- Draw from the chosen bin 
    std::vector<double> draw_kg(bins_.size(), 0.0);
    draw_kg[chosen_i] = actual;

    Bin& b = bins_[chosen_i];
    b.available_kg      -= actual;
    total_inventory_kg_ -= actual;
    remaining_throughput -= actual;

    remaining_kg_[chosen_i] = b.available_kg;

    cyclus::Composition::Ptr comp = BlendedComp_(draw_kg);
    cyclus::Material::Ptr mat =
        cyclus::Material::CreateUntracked(actual, comp);

    responses.push_back(std::make_pair(tr, mat));

    LOG(cyclus::LEV_INFO5, "us_inventory")
        << prototype() << " sent " << actual << " kg of " << outcommod
        << " from bin " << b.assembly_id
        << " (policy=" << selection_policy << ")";
  }
}

// BlendedComp_

cyclus::Composition::Ptr us_inventory::BlendedComp_(
    const std::vector<double>& draw_kg) const {
  cyclus::CompMap blended;
  double total = 0.0;

  for (size_t i = 0; i < bins_.size(); ++i) {
    if (draw_kg[i] <= cyclus::eps() || bins_[i].comp == NULL) continue;
    const cyclus::CompMap& cm = bins_[i].comp->mass();
    for (cyclus::CompMap::const_iterator it = cm.begin();
         it != cm.end(); ++it) {
      blended[it->first] += it->second * draw_kg[i];
    }
    total += draw_kg[i];
  }

  if (total > cyclus::eps()) {
    for (cyclus::CompMap::iterator it = blended.begin();
         it != blended.end(); ++it) {
      it->second /= total;
    }
  }

  return cyclus::Composition::CreateFromMass(blended);
}

// CSV helpers

namespace {

std::vector<std::string> SplitCSVLine(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item.erase(item.begin(),
               std::find_if(item.begin(), item.end(),
                            [](unsigned char ch) {
                              return !std::isspace(ch);
                            }));
    item.erase(std::find_if(item.rbegin(), item.rend(),
                            [](unsigned char ch) {
                              return !std::isspace(ch);
                            }).base(),
               item.end());
    out.push_back(item);
  }
  return out;
}

}  

void us_inventory::LoadAssembliesCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) throw cyclus::ValueError("us_inventory: cannot open " + path);

  std::string header;
  if (!std::getline(f, header))
    throw cyclus::ValueError("us_inventory: empty file " + path);

  std::vector<std::string> cols = SplitCSVLine(header);

  // Map column names to indices
  int i_id    = -1;
  int i_mass  = -1;
  int i_count = -1;
  int i_date  = -1;
  int i_bu    = -1;
  int i_enr   = -1;

  for (size_t i = 0; i < cols.size(); ++i) {
    if      (cols[i] == "assembly_id")   i_id    = static_cast<int>(i);
    else if (cols[i] == "total_mass_kg") i_mass  = static_cast<int>(i);
    else if (cols[i] == "count")         i_count = static_cast<int>(i);
    else if (cols[i] == "discharge_date")i_date  = static_cast<int>(i);
    else if (cols[i] == "burnup")        i_bu    = static_cast<int>(i);
    else if (cols[i] == "enrichment")    i_enr   = static_cast<int>(i);
  }

  if (i_id < 0 || i_mass < 0) {
    throw cyclus::ValueError(
        "us_inventory: assemblies.csv must contain assembly_id and "
        "total_mass_kg columns.");
  }

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::vector<std::string> v = SplitCSVLine(line);
    if (static_cast<int>(v.size()) <= std::max(i_id, i_mass)) continue;

    Bin b;
    b.assembly_id = v[i_id];
    double mass   = std::stod(v[i_mass]);
    double count  = 1.0;

    if (i_count >= 0 && i_count < static_cast<int>(v.size()) &&
        !v[i_count].empty()) {
      count = std::stod(v[i_count]);
    }

    b.available_kg = mass * count;

    if (i_date >= 0 && i_date < static_cast<int>(v.size()) &&
        !v[i_date].empty()) {
      b.discharge_date = std::stod(v[i_date]);
    }

    if (i_bu >= 0 && i_bu < static_cast<int>(v.size()) &&
        !v[i_bu].empty()) {
      b.burnup = std::stod(v[i_bu]);
    }

    if (i_enr >= 0 && i_enr < static_cast<int>(v.size()) &&
        !v[i_enr].empty()) {
      b.enrichment = std::stod(v[i_enr]);
    }

    idx_[b.assembly_id] = bins_.size();
    bins_.push_back(b);
    total_inventory_kg_ += b.available_kg;
  }

  if (bins_.empty())
    throw cyclus::ValueError("us_inventory: no rows loaded from " + path);
}

void us_inventory::LoadCompositionCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) throw cyclus::ValueError("us_inventory: cannot open " + path);

  std::string header;
  if (!std::getline(f, header))
    throw cyclus::ValueError("us_inventory: empty file " + path);

  std::vector<std::string> cols = SplitCSVLine(header);

  int i_id   = -1;
  int i_nuc  = -1;
  int i_frac = -1;

  for (size_t i = 0; i < cols.size(); ++i) {
    if      (cols[i] == "assembly_id")   i_id   = static_cast<int>(i);
    else if (cols[i] == "nuclide")       i_nuc  = static_cast<int>(i);
    else if (cols[i] == "mass_fraction") i_frac = static_cast<int>(i);
  }

  if (i_id < 0 || i_nuc < 0 || i_frac < 0) {
    throw cyclus::ValueError(
        "us_inventory: composition.csv must contain assembly_id, nuclide, "
        "and mass_fraction columns.");
  }

  std::unordered_map<std::string, cyclus::CompMap> compmaps;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::vector<std::string> v = SplitCSVLine(line);
    if (static_cast<int>(v.size()) <=
        std::max(i_id, std::max(i_nuc, i_frac))) continue;

    std::string aid  = v[i_id];
    std::string nuc  = v[i_nuc];
    double      frac = std::stod(v[i_frac]);
    if (frac <= 0.0) continue;

    int nid = NucIdFromString_(nuc);
    compmaps[aid][nid] += frac;
  }

  for (std::unordered_map<std::string, cyclus::CompMap>::iterator it =
           compmaps.begin(); it != compmaps.end(); ++it) {
    std::unordered_map<std::string, size_t>::iterator idx_it =
        idx_.find(it->first);
    if (idx_it == idx_.end()) continue;
    bins_[idx_it->second].comp =
        cyclus::Composition::CreateFromMass(it->second);
  }
}

// ---------------------------------------------------------------------------
// Nuclide parsing — I should try using PyNE instead of ZZAAAM.
// ---------------------------------------------------------------------------

int us_inventory::NucIdFromString_(const std::string& s) const {
  std::string t = s;

  t.erase(
      std::remove_if(t.begin(), t.end(),
                     [](unsigned char c) {
                       return c == '-' || std::isspace(c);
                     }),
      t.end());

  if (t.empty()) {
    throw std::runtime_error("Bad nuclide string: '" + s + "'");
  }

  size_t pos = 0;
  while (pos < t.size() && std::isalpha(static_cast<unsigned char>(t[pos]))) {
    pos++;
  }

  if (pos == 0 || pos == t.size()) {
    throw std::runtime_error("Bad nuclide string: '" + s + "'");
  }

  std::string sym = t.substr(0, pos);
  std::string a_str = t.substr(pos);

  sym[0] = std::toupper(static_cast<unsigned char>(sym[0]));
  for (size_t i = 1; i < sym.size(); ++i) {
    sym[i] = std::tolower(static_cast<unsigned char>(sym[i]));
  }

  int A = std::stoi(a_str);

  if (A <= 0) {
    throw std::runtime_error("Bad mass number in nuclide: '" + s + "'");
  }

  static const std::unordered_map<std::string, int> Z = {
      {"H", 1},   {"He", 2},  {"Li", 3},  {"Be", 4},  {"B", 5},
      {"C", 6},   {"N", 7},   {"O", 8},   {"F", 9},   {"Ne", 10},
      {"Na", 11}, {"Mg", 12}, {"Al", 13}, {"Si", 14}, {"P", 15},
      {"S", 16},  {"Cl", 17}, {"Ar", 18}, {"K", 19},  {"Ca", 20},
      {"Sc", 21}, {"Ti", 22}, {"V", 23},  {"Cr", 24}, {"Mn", 25},
      {"Fe", 26}, {"Co", 27}, {"Ni", 28}, {"Cu", 29}, {"Zn", 30},
      {"Ga", 31}, {"Ge", 32}, {"As", 33}, {"Se", 34}, {"Br", 35},
      {"Kr", 36}, {"Rb", 37}, {"Sr", 38}, {"Y", 39},  {"Zr", 40},
      {"Nb", 41}, {"Mo", 42}, {"Tc", 43}, {"Ru", 44}, {"Rh", 45},
      {"Pd", 46}, {"Ag", 47}, {"Cd", 48}, {"In", 49}, {"Sn", 50},
      {"Sb", 51}, {"Te", 52}, {"I", 53},  {"Xe", 54}, {"Cs", 55},
      {"Ba", 56}, {"La", 57}, {"Ce", 58}, {"Pr", 59}, {"Nd", 60},
      {"Pm", 61}, {"Sm", 62}, {"Eu", 63}, {"Gd", 64}, {"Tb", 65},
      {"Dy", 66}, {"Ho", 67}, {"Er", 68}, {"Tm", 69}, {"Yb", 70},
      {"Lu", 71}, {"Hf", 72}, {"Ta", 73}, {"W", 74},  {"Re", 75},
      {"Os", 76}, {"Ir", 77}, {"Pt", 78}, {"Au", 79}, {"Hg", 80},
      {"Tl", 81}, {"Pb", 82}, {"Bi", 83}, {"Po", 84}, {"At", 85},
      {"Rn", 86}, {"Fr", 87}, {"Ra", 88}, {"Ac", 89}, {"Th", 90},
      {"Pa", 91}, {"U", 92},  {"Np", 93}, {"Pu", 94}, {"Am", 95},
      {"Cm", 96}};

  auto it = Z.find(sym);

  if (it == Z.end()) {
    throw std::runtime_error(
        "Unknown element symbol in nuclide: '" + s + "' parsed as '" +
        sym + "'");
  }

  int z = it->second;
  int zzaaam = z * 10000 + A * 10;

  return zzaaam;
}

extern "C" cyclus::Agent* Constructus_inventory(cyclus::Context* ctx) {
  return new us_inventory(ctx);
}

}  // namespace einstein
