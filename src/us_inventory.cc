#include "us_inventory.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>

#include "toolkit/mat_query.h"  // optional, but sometimes useful

USInventory::USInventory(cyclus::Context* ctx) : cyclus::Facility(ctx) {}

USInventory::~USInventory() {}

std::string USInventory::str() {
  std::stringstream ss;
  ss << cyclus::Facility::str()
     << " USInventory(outcommod=" << outcommod
     << ", bins=" << bins_.size() << ")";
  return ss.str();
}

void USInventory::EnterNotify() {
  cyclus::Facility::EnterNotify();

  if (outcommod.empty()) {
    throw std::runtime_error("USInventory: outcommod is required.");
  }
  if (assemblies_file.empty() || composition_file.empty()) {
    throw std::runtime_error("USInventory: assemblies_file and composition_file are required.");
  }

  bins_.clear();
  idx_.clear();

  LoadAssembliesCSV_(assemblies_file);
  LoadCompositionCSV_(composition_file);

  // Sanity check: every bin must have a composition
  for (const auto& b : bins_) {
    if (b.comp == nullptr) {
      throw std::runtime_error("USInventory: missing composition for assembly_id=" + b.assembly_id);
    }
  }
}

// ------------------------- DRE Methods -------------------------

void USInventory::GetMatlBids(
    cyclus::CommodMap<cyclus::Material>::type& commod_requests,
    cyclus::BidPortfolio<cyclus::Material>::type& bids) {

  // Only respond to our commodity
  auto it = commod_requests.find(outcommod);
  if (it == commod_requests.end()) {
    return;
  }

  // Total available in all bins
  double total_avail = 0.0;
  for (const auto& b : bins_) total_avail += b.available_kg;

  if (total_avail <= 0.0) return;

  // Total requested mass
  double total_req = 0.0;
  for (auto& req : it->second) {
    total_req += req->target()->quantity();
  }
  if (total_req <= 0.0) return;

  // Throughput limit per timestep
  double can_supply = std::min(total_avail, throughput_kg);

  // Create bids: simplest strategy = bid on every request with same “capacity”
  // Cyclus will allocate trades; we finalize exact masses in GetMatlTrades.
  std::vector<cyclus::Request<cyclus::Material>*>& reqs = it->second;

  for (auto* r : reqs) {
    // Bid some positive quantity. We'll honor final trade mass later.
    // Use a dummy material for the bid with any valid comp (use first non-empty bin).
    cyclus::Composition::Ptr c = nullptr;
    for (const auto& b : bins_) {
      if (b.available_kg > 0.0 && b.comp != nullptr) { c = b.comp; break; }
    }
    if (c == nullptr) return;

    double q = std::min(r->target()->quantity(), can_supply);
    if (q <= 0.0) continue;

    auto offer = cyclus::Material::Create(this->context(), q, c);
    bids.AddBid(r, offer, this, outcommod);
  }
}

void USInventory::GetMatlTrades(
    const std::vector< cyclus::Trade<cyclus::Material> >& trades,
    std::vector< std::pair< cyclus::Trade<cyclus::Material>,
                            cyclus::Material::Ptr > >& responses) {

  double remaining_throughput = throughput_kg;

  for (const auto& tr : trades) {
    double q = tr.amt;  // requested trade quantity (kg)
    if (q <= 0.0) continue;

    if (!allow_partial) {
      // If partial not allowed, you can reject trades not fully satisfiable.
      // Here we simply skip if we can't supply fully.
      double total_avail = 0.0;
      for (const auto& b : bins_) total_avail += b.available_kg;
      if (q > total_avail || q > remaining_throughput) continue;
    }

    // Cap by throughput
    double send = std::min(q, remaining_throughput);
    if (send <= 0.0) break;

    // Withdraw mass from bins FIFO
    // If you want “closest match” by burnup/enr later, this is where you’d choose bins differently.
    cyclus::Composition::Ptr chosen_comp = nullptr;
    double to_make = send;

    // We may need to split across bins with different compositions.
    // For now: simplest policy = take from the first bin with enough mass.
    // If not enough, we’ll take what we can from a bin and keep going,
    // BUT composition would change. To keep it consistent per trade,
    // we instead enforce: fulfill each trade from a single bin.
    size_t chosen_i = bins_.size();
    for (size_t i = 0; i < bins_.size(); ++i) {
      if (bins_[i].available_kg > 0.0) {
        chosen_i = i;
        break;
      }
    }
    if (chosen_i == bins_.size()) continue;

    Bin& b = bins_[chosen_i];
    chosen_comp = b.comp;

    double max_from_bin = std::min(b.available_kg, remaining_throughput);
    double actual = allow_partial ? std::min(to_make, max_from_bin) : to_make;

    if (actual <= 0.0) continue;

    // Decrement inventory
    b.available_kg -= actual;
    remaining_throughput -= actual;

    // Create the material response
    auto mat = cyclus::Material::Create(this->context(), actual, chosen_comp);
    responses.push_back(std::make_pair(tr, mat));
  }
}

// ------------------------- CSV Loading -------------------------

static std::vector<std::string> SplitCSVLine(const std::string& line) {
  // Simple CSV splitter (no quoted commas). Good enough if your files are simple.
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    // trim whitespace
    item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch){ return !std::isspace(ch); }));
    item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), item.end());
    out.push_back(item);
  }
  return out;
}

void USInventory::LoadAssembliesCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) throw std::runtime_error("USInventory: cannot open " + path);

  std::string header;
  if (!std::getline(f, header)) throw std::runtime_error("USInventory: empty file " + path);
  auto cols = SplitCSVLine(header);

  auto col_index = [&](const std::string& name) -> int {
    for (size_t i = 0; i < cols.size(); ++i) if (cols[i] == name) return (int)i;
    return -1;
  };

  int i_id = col_index("assembly_id");
  int i_mass = col_index("total_mass_kg");
  int i_count = col_index("count");  // optional

  if (i_id < 0 || i_mass < 0) {
    throw std::runtime_error("USInventory: assemblies.csv must contain assembly_id and total_mass_kg");
  }

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto v = SplitCSVLine(line);
    if ((int)v.size() <= std::max(i_id, i_mass)) continue;

    Bin b;
    b.assembly_id = v[i_id];

    double mass = std::stod(v[i_mass]);
    double count = 1.0;
    if (i_count >= 0 && i_count < (int)v.size() && !v[i_count].empty()) {
      count = std::stod(v[i_count]);
    }
    b.available_kg = mass * count;

    idx_[b.assembly_id] = bins_.size();
    bins_.push_back(b);
  }

  if (bins_.empty()) {
    throw std::runtime_error("USInventory: no rows loaded from " + path);
  }
}

void USInventory::LoadCompositionCSV_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) throw std::runtime_error("USInventory: cannot open " + path);

  std::string header;
  if (!std::getline(f, header)) throw std::runtime_error("USInventory: empty file " + path);
  auto cols = SplitCSVLine(header);

  auto col_index = [&](const std::string& name) -> int {
    for (size_t i = 0; i < cols.size(); ++i) if (cols[i] == name) return (int)i;
    return -1;
  };

  int i_id = col_index("assembly_id");
  int i_nuc = col_index("nuclide");
  int i_frac = col_index("mass_fraction");

  if (i_id < 0 || i_nuc < 0 || i_frac < 0) {
    throw std::runtime_error("USInventory: composition.csv must contain assembly_id, nuclide, mass_fraction");
  }

  // Build comp maps per assembly_id
  std::unordered_map<std::string, cyclus::CompMap> compmaps;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    auto v = SplitCSVLine(line);
    if ((int)v.size() <= std::max({i_id, i_nuc, i_frac})) continue;

    std::string aid = v[i_id];
    std::string nuc = v[i_nuc];
    double frac = std::stod(v[i_frac]);
    if (frac <= 0.0) continue;

    int nid = NucIdFromString_(nuc);
    compmaps[aid][nid] += frac;  // += in case of duplicates
  }

  // Create cyclus compositions and assign to bins
  for (auto& kv : compmaps) {
    const std::string& aid = kv.first;
    auto it = idx_.find(aid);
    if (it == idx_.end()) continue; // composition for an assembly not in assemblies.csv (skip)
    // CreateFromMass expects mass fractions or masses; relative values are fine.
    bins_[it->second].comp = cyclus::Composition::CreateFromMass(kv.second);
  }
}

// ------------------------- Nuclide Parsing -------------------------

int USInventory::NucIdFromString_(const std::string& s) const {
  // Minimal parser for strings like "U235", "Pu239", "Cs137".
  // Produces zzaaam * 10? Cyclus commonly uses zzaaam format (e.g., 922350).
  // We'll return zzaaam where m=0.
  // If your dataset uses other forms (e.g., "U-235" or "92235"), tell me and I’ll adjust.

  std::string t = s;
  // Remove '-' and spaces
  t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c){ return c=='-' || std::isspace(c); }), t.end());
  if (t.empty()) throw std::runtime_error("Bad nuclide string: '" + s + "'");

  // Split leading letters (element) + trailing digits (A)
  size_t pos = 0;
  while (pos < t.size() && std::isalpha((unsigned char)t[pos])) pos++;
  if (pos == 0 || pos == t.size()) throw std::runtime_error("Bad nuclide string: '" + s + "'");

  std::string sym = t.substr(0, pos);
  std::string a_str = t.substr(pos);

  // Normalize symbol: first uppercase, rest lowercase
  sym[0] = std::toupper((unsigned char)sym[0]);
  for (size_t i = 1; i < sym.size(); ++i) sym[i] = std::tolower((unsigned char)sym[i]);

  int A = std::stoi(a_str);
  if (A <= 0) throw std::runtime_error("Bad mass number in nuclide: '" + s + "'");

  // Small element->Z map (extend as needed). Tell me if you have lots of elements beyond these.
  static const std::unordered_map<std::string, int> Z = {
    {"H",1},{"He",2},{"Li",3},{"Be",4},{"B",5},{"C",6},{"N",7},{"O",8},{"F",9},{"Ne",10},
    {"Na",11},{"Mg",12},{"Al",13},{"Si",14},{"P",15},{"S",16},{"Cl",17},{"Ar",18},
    {"K",19},{"Ca",20},{"Sc",21},{"Ti",22},{"V",23},{"Cr",24},{"Mn",25},{"Fe",26},{"Co",27},{"Ni",28},{"Cu",29},{"Zn",30},
    {"Ga",31},{"Ge",32},{"As",33},{"Se",34},{"Br",35},{"Kr",36},
    {"Rb",37},{"Sr",38},{"Y",39},{"Zr",40},{"Nb",41},{"Mo",42},{"Tc",43},{"Ru",44},{"Rh",45},{"Pd",46},{"Ag",47},{"Cd",48},
    {"In",49},{"Sn",50},{"Sb",51},{"Te",52},{"I",53},{"Xe",54},
    {"Cs",55},{"Ba",56},{"La",57},{"Ce",58},{"Pr",59},{"Nd",60},{"Pm",61},{"Sm",62},{"Eu",63},{"Gd",64},{"Tb",65},{"Dy",66},
    {"Ho",67},{"Er",68},{"Tm",69},{"Yb",70},{"Lu",71},{"Hf",72},{"Ta",73},{"W",74},{"Re",75},{"Os",76},{"Ir",77},{"Pt",78},{"Au",79},{"Hg",80},
    {"Tl",81},{"Pb",82},{"Bi",83},{"Po",84},{"At",85},{"Rn",86},
    {"Fr",87},{"Ra",88},{"Ac",89},{"Th",90},{"Pa",91},{"U",92},{"Np",93},{"Pu",94},{"Am",95},{"Cm",96}
  };

  auto it = Z.find(sym);
  if (it == Z.end()) throw std::runtime_error("Unknown element symbol in nuclide: '" + s + "' (parsed '" + sym + "')");

  int z = it->second;
  int zzaaam = z*10000 + A*10 + 0; // m=0
  return zzaaam;
}

// ---------- Cyclus boilerplate ----------
#pragma cyclus impl