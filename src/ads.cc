#include "ads.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace einstein {

ads::ads(cyclus::Context* ctx)
    : cyclus::Facility(ctx),
      capacity(0.0),
      cycle_time(1),
      refuel_time(0),
      power_level(0.0),
      beam_rate(0.0),
      volume(0.0),
      days_per_step(-1.0),
      cycle_step(0),
      refuel_step(0) {}

ads::~ads() {}

void ads::InitFrom(ads* m) {
  #pragma cyclus impl initfromcopy einstein::ads
  cyclus::toolkit::CommodityProducer::Copy(m);
}

void ads::InitFrom(cyclus::QueryableBackend* b) {
  #pragma cyclus impl initfromdb einstein::ads
  namespace tk = cyclus::toolkit;
  tk::CommodityProducer::Add(tk::Commodity(outcommod),
                             tk::CommodInfo(power_level, power_level));
}

std::string ads::str() {
  std::stringstream ss;
  ss << cyclus::Facility::str()
     << " ads(outcommod=" << outcommod
     << ", n_feed_commods=" << incommods.size()
     << ", capacity=" << capacity
     << ", cycle_time=" << cycle_time
     << ", refuel_time=" << refuel_time
     << ", power_level=" << power_level
     << ", n_cases=" << library_.size()
     << ")";
  return ss.str();
}

void ads::EnterNotify() {
  cyclus::Facility::EnterNotify();

  if (incommods.empty()) {
    throw cyclus::ValueError("ads: at least one feed commodity is required.");
  }
  if (outcommod.empty()) {
    throw cyclus::ValueError("ads: outcommod is required.");
  }
  if (capacity <= 0.0) {
    throw cyclus::ValueError("ads: capacity (core mass) must be > 0.");
  }
  if (cycle_time <= 0) {
    throw cyclus::ValueError("ads: cycle_time must be >= 1 step.");
  }
  if (case_dir.empty()) {
    throw cyclus::ValueError("ads: case_dir is required.");
  }

  if (incommod_prefs.empty()) {
    for (size_t i = 0; i < incommods.size(); ++i) {
      incommod_prefs.push_back(cyclus::kDefaultPref);
    }
  } else if (incommod_prefs.size() != incommods.size()) {
    throw cyclus::ValueError(
        "ads: incommod_prefs length must match incommods length.");
  }

  library_.clear();
  LoadLibrary_(case_dir);
  if (library_.empty()) {
    throw cyclus::ValueError(
        "ads: no Serpent cases loaded from case_dir '" + case_dir + "'.");
  }

  LOG(cyclus::LEV_INFO2, "ads") << prototype() << ": loaded "
      << library_.size() << " Serpent case(s) from " << case_dir << ".";
}

// ===========================================================================
//  Cycle: request feed -> load core -> irradiate -> discharge -> trade out
// ===========================================================================

void ads::Tick() {
  bool was_loaded = core.count() > 0;

  if (was_loaded) {
    if (cycle_step >= cycle_time) {
      Discharge_();
      refuel_step = 0;
    }
  } else {
    if (capacity > cyclus::eps_rsrc() &&
        refuel_step >= refuel_time &&
        feed.quantity() >= capacity - cyclus::eps_rsrc()) {
      cyclus::Material::Ptr in = feed.Pop(capacity, cyclus::eps_rsrc());
      double loaded = in->quantity();
      core.Push(in);
      cycle_step = 0;
      Record_("Load", "", loaded);
      LOG(cyclus::LEV_INFO4, "ads") << prototype() << " loaded a "
          << loaded << " kg core.";
    }
  }
}

void ads::Discharge_() {
  using cyclus::Material;

  Material::Ptr m = core.Pop(core.quantity(), cyclus::eps_rsrc());
  double m_in = m->quantity();

  // 1) Match the core composition (+ power) to a Serpent case.
  cyclus::CompMap in_mass = m->comp()->mass();
  cyclus::compmath::Normalize(&in_mass, m_in);
  size_t ci = MatchCase_(in_mass, power_level);
  const DepletionCase& c = library_[ci];

  // 2) Map cycle length (steps) to irradiation days, read off the spent state.
  double dps = DaysPerStep_();
  double target_day = cycle_time * dps;
  double mass_ratio = 1.0;
  double actinide_frac = 1.0;
  cyclus::Composition::Ptr out_comp =
      OutputComp_(c, target_day, &mass_ratio, &actinide_frac);

  // 3) Transmute to the spent composition (actinides + fission products) and
  //    scale to the mass the case actually retains. The small remainder is the
  //    mass carried off by escaping neutrons and released as energy.
  m->Transmute(out_comp);
  double m_out = m_in * mass_ratio;
  Material::Ptr product_mat = m->ExtractQty(m_out);
  double mass_defect = m->quantity();  // leaves the simulation

  product.Push(product_mat);

  // Actinide bookkeeping: at t=0 the case is essentially all actinide, so the
  // destroyed mass is the actinide the core no longer holds at discharge.
  double actinide_in = m_in;
  if (!c.total_kg.empty() && c.total_kg.front() > 0.0) {
    actinide_in = m_in * (c.actinide_kg.front() / c.total_kg.front());
  }
  double actinide_out = m_out * actinide_frac;
  double destroyed = actinide_in - actinide_out;

  Record_("Discharge", c.name, m_in);
  if (destroyed > 0.0) {
    Record_("ActinideDestroyed", c.name, destroyed);
  }
  if (m_out - actinide_out > 0.0) {
    Record_("FissionProducts", c.name, m_out - actinide_out);
  }
  if (mass_defect > 0.0) {
    Record_("MassDefect", c.name, mass_defect);
  }

  LOG(cyclus::LEV_INFO4, "ads") << prototype() << " discharged " << m_out
      << " kg (case '" << c.name << "', target_day=" << target_day
      << ", actinide destroyed=" << destroyed
      << " kg, fission products=" << (m_out - actinide_out)
      << " kg, mass defect=" << mass_defect << " kg).";
}

void ads::Tock() {
  if (core.count() > 0) {
    cycle_step++;
  } else {
    refuel_step++;
  }
}

double ads::DaysPerStep_() const {
  if (days_per_step > 0.0) {
    return days_per_step;
  }
  double secs = static_cast<double>(context()->dt());
  if (secs > 0.0) {
    return secs / 86400.0;
  }
  return 365.25 / 12.0;  // fallback: average month
}

// ===========================================================================
//  Feed requests / acceptance
// ===========================================================================

std::set<cyclus::RequestPortfolio<cyclus::Material>::Ptr>
ads::GetMatlRequests() {
  using cyclus::Material;
  using cyclus::RequestPortfolio;
  using cyclus::Request;

  std::set<RequestPortfolio<Material>::Ptr> ports;

  double space = feed.space();
  if (space < cyclus::eps_rsrc()) {
    return ports;
  }

  RequestPortfolio<Material>::Ptr port(new RequestPortfolio<Material>());

  Material::Ptr m;
  if (feed_recipe.empty()) {
    m = cyclus::NewBlankMaterial(space);
  } else {
    m = Material::CreateUntracked(space, context()->GetRecipe(feed_recipe));
  }

  std::vector<Request<Material>*> reqs;
  for (size_t i = 0; i < incommods.size(); ++i) {
    reqs.push_back(
        port->AddRequest(m, this, incommods[i], incommod_prefs[i], false));
  }
  port->AddMutualReqs(reqs);
  ports.insert(port);
  return ports;
}

void ads::AcceptMatlTrades(
    const std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                                cyclus::Material::Ptr> >& responses) {
  std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                        cyclus::Material::Ptr> >::const_iterator it;
  for (it = responses.begin(); it != responses.end(); ++it) {
    feed.Push(it->second);
  }
}

// ===========================================================================
//  Product bids / trades
// ===========================================================================

std::set<cyclus::BidPortfolio<cyclus::Material>::Ptr> ads::GetMatlBids(
    cyclus::CommodMap<cyclus::Material>::type& commod_requests) {
  using cyclus::BidPortfolio;
  using cyclus::CapacityConstraint;
  using cyclus::Material;
  using cyclus::Request;
  using cyclus::toolkit::MatVec;

  std::set<BidPortfolio<Material>::Ptr> ports;

  if (commod_requests.count(outcommod) == 0) {
    return ports;
  }
  if (product.quantity() < cyclus::eps_rsrc()) {
    return ports;
  }

  std::vector<Request<Material>*>& reqs = commod_requests[outcommod];
  if (reqs.empty()) {
    return ports;
  }

  MatVec mats = product.PopN(product.count());
  product.Push(mats);

  BidPortfolio<Material>::Ptr port(new BidPortfolio<Material>());
  for (size_t j = 0; j < reqs.size(); ++j) {
    Request<Material>* req = reqs[j];
    double tot_bid = 0.0;
    for (size_t k = 0; k < mats.size(); ++k) {
      Material::Ptr mat = mats[k];
      if (mat->quantity() > cyclus::eps_rsrc()) {
        port->AddBid(req, mat, this, false);
      }
      tot_bid += mat->quantity();
      if (tot_bid >= req->target()->quantity()) {
        break;
      }
    }
  }

  CapacityConstraint<Material> cc(product.quantity());
  port->AddConstraint(cc);
  ports.insert(port);
  return ports;
}

void ads::GetMatlTrades(
    const std::vector<cyclus::Trade<cyclus::Material> >& trades,
    std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                          cyclus::Material::Ptr> >& responses) {
  using cyclus::Material;
  using cyclus::Trade;

  std::vector<Trade<Material> >::const_iterator it;
  for (it = trades.begin(); it != trades.end(); ++it) {
    double amt = std::min(product.quantity(), it->amt);
    Material::Ptr m = product.Pop(amt, cyclus::eps_rsrc());
    responses.push_back(std::make_pair(*it, m));
  }
}

// ===========================================================================
//  Case matching and output composition
// ===========================================================================

size_t ads::MatchCase_(const cyclus::CompMap& input_mass, double power) const {
  double act = 0.0, ma = 0.0, pu = 0.0, u = 0.0;
  for (cyclus::CompMap::const_iterator it = input_mass.begin();
       it != input_mass.end(); ++it) {
    int z = it->first / 10000000;
    double g = it->second;
    if (z < 89) continue;  // fission products in a recycled feed don't key it
    act += g;
    if (z == 94) pu += g;
    else if (z == 93 || z == 95 || z == 96) ma += g;  // Np, Am, Cm
    else if (z == 92) u += g;
  }
  double in_ma = act > 0 ? ma / act : 0.0;
  double in_pu = act > 0 ? pu / act : 0.0;
  double in_u  = act > 0 ? u  / act : 0.0;

  const double w_power = 10.0;  // power tier dominates; tune as needed

  size_t best = 0;
  double best_score = std::numeric_limits<double>::max();
  for (size_t i = 0; i < library_.size(); ++i) {
    const DepletionCase& c = library_[i];
    double pmax = std::max(power, c.power_mw);
    double power_term = pmax > 0 ? std::fabs(power - c.power_mw) / pmax : 0.0;
    double comp_term = std::fabs(in_ma - c.in_ma_frac) +
                       std::fabs(in_pu - c.in_pu_frac) +
                       std::fabs(in_u - c.in_u_frac);
    double score = w_power * power_term + comp_term;
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }
  return best;
}

cyclus::Composition::Ptr ads::OutputComp_(const DepletionCase& c, double day,
                                          double* mass_ratio,
                                          double* actinide_frac) const {
  const std::vector<double>& t = c.time;
  size_t n = t.size();
  double frac = 0.0;
  size_t lo = 0, hi = 0;
  if (day <= t.front()) {
    lo = hi = 0;
  } else if (day >= t.back()) {
    // Never extrapolate: depletion saturates, so a straight-line projection
    // past the data would overshoot. Clamp to the last simulated state.
    lo = hi = n - 1;
  } else {
    hi = 1;
    while (hi < n && t[hi] < day) hi++;
    lo = hi - 1;
    double span = t[hi] - t[lo];
    frac = span > 0 ? (day - t[lo]) / span : 0.0;
  }

  // Linear interpolation of every nuclide (actinides and fission products)
  // between the two bracketing snapshots.
  cyclus::CompMap out;
  const cyclus::CompMap& a = c.comp_series[lo];
  const cyclus::CompMap& b = c.comp_series[hi];
  for (cyclus::CompMap::const_iterator it = a.begin(); it != a.end(); ++it) {
    out[it->first] += (1.0 - frac) * it->second;
  }
  for (cyclus::CompMap::const_iterator it = b.begin(); it != b.end(); ++it) {
    out[it->first] += frac * it->second;
  }

  double m0 = c.total_kg.front();
  double mt = (1.0 - frac) * c.total_kg[lo] + frac * c.total_kg[hi];
  double at = (1.0 - frac) * c.actinide_kg[lo] + frac * c.actinide_kg[hi];

  *mass_ratio = (m0 > 0.0) ? std::min(1.0, mt / m0) : 1.0;
  *actinide_frac = (mt > 0.0) ? at / mt : 1.0;

  return cyclus::Composition::CreateFromMass(out);
}

// ===========================================================================
//  Library loading
// ===========================================================================

std::string ads::FindFile_(const std::string& dir,
                           const std::string& needle) const {
  DIR* d = opendir(dir.c_str());
  if (!d) return "";
  std::string found;
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    std::string n = ent->d_name;
    if (n == "." || n == "..") continue;
    bool is_txt = n.size() > 4 && n.substr(n.size() - 4) == ".txt";
    if (is_txt && n.find(needle) != std::string::npos) {
      found = dir + "/" + n;
      break;
    }
  }
  closedir(d);
  return found;
}

void ads::LoadLibrary_(const std::string& dir) {
  // (a) case_dir is itself one case (holds an actinide mass table directly).
  if (!FindFile_(dir, "actinide_masses").empty()) {
    DepletionCase c = LoadCase_(dir, BaseName_(dir));
    if (!c.comp_series.empty()) library_.push_back(c);
    return;
  }

  // (b) case_dir holds one subfolder per case.
  DIR* d = opendir(dir.c_str());
  if (!d) {
    throw cyclus::ValueError("ads: cannot open case_dir '" + dir + "'.");
  }
  struct dirent* ent;
  while ((ent = readdir(d)) != NULL) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    std::string sub = dir + "/" + name;
    if (FindFile_(sub, "actinide_masses").empty()) continue;
    DepletionCase c = LoadCase_(sub, name);
    if (!c.comp_series.empty()) library_.push_back(c);
  }
  closedir(d);
}

std::string ads::BaseName_(const std::string& path) const {
  size_t p = path.find_last_of('/');
  return (p == std::string::npos) ? path : path.substr(p + 1);
}

// --- helpers for the columnar plot_data files ------------------------------
namespace {

bool IsDataLine(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
  return i < line.size() && std::isdigit((unsigned char)line[i]);
}

std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

// Read a "Step Day [d] <value> ..." file; vcol=0 is the first value column.
void ReadSeries(const std::string& path, int vcol,
                std::vector<double>* days, std::vector<double>* vals) {
  if (path.empty()) return;
  std::ifstream f(path.c_str());
  if (!f.good()) return;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (!IsDataLine(line)) continue;
    std::vector<std::string> tk = Tokens(line);
    if (tk.size() < static_cast<size_t>(2 + vcol + 1)) continue;
    days->push_back(std::strtod(tk[1].c_str(), NULL));
    vals->push_back(std::strtod(tk[2 + vcol].c_str(), NULL));
  }
}

}  // namespace

bool ads::ReadMassTable_(const std::string& path, const std::string& case_name,
                         std::vector<double>* days,
                         std::vector<cyclus::CompMap>* series) {
  if (path.empty()) return false;
  std::ifstream f(path.c_str());
  if (!f.good()) return false;

  std::vector<std::string> header;
  std::vector<std::pair<size_t, int> > cols;  // (data token index, nuclide id)
  bool mapped = false;
  bool warned_bad_nuc = false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> tk = Tokens(line);
    if (tk.empty()) continue;
    if (tk[0] == "Step") { header = tk; continue; }
    if (!IsDataLine(line)) continue;

    if (!mapped) {
      // Value labels are the trailing (ncols - 2) header tokens; this absorbs
      // the "Day [d]" two-token quirk without index juggling.
      size_t n_values = tk.size() - 2;
      if (header.size() >= n_values) {
        size_t off = header.size() - n_values;
        for (size_t j = 0; j < n_values; ++j) {
          const std::string& label = header[off + j];
          if (EndsWith_(label, "_kg") && !EndsWith_(label, "_delta_kg")) {
            std::string iso = label.substr(0, label.size() - 3);
            int nid = NucId_(iso);
            if (nid > 0) {
              cols.push_back(std::make_pair(2 + j, nid));
            } else if (!warned_bad_nuc) {
              LOG(cyclus::LEV_WARN, "ads") << prototype()
                  << ": skipping unrecognized isotope column(s) (e.g. '"
                  << iso << "') in case '" << case_name << "'.";
              warned_bad_nuc = true;
            }
          }
        }
      }
      mapped = true;
    }

    days->push_back(std::strtod(tk[1].c_str(), NULL));
    cyclus::CompMap cm;
    for (size_t k = 0; k < cols.size(); ++k) {
      double kg = std::strtod(tk[cols[k].first].c_str(), NULL);
      if (kg > 0.0) cm[cols[k].second] += kg;  // metastables fold onto ground
    }
    series->push_back(cm);
  }
  return !series->empty();
}

ads::DepletionCase ads::LoadCase_(const std::string& case_path,
                                  const std::string& case_name) {
  DepletionCase c;
  c.name = case_name;

  // --- actinides: prefer the full table extracted from _dep.m --------------
  std::string act_path = FindFile_(case_path, "actinide_masses_full");
  if (act_path.empty()) {
    act_path = FindFile_(case_path, "plot_data_06_actinide_masses");
  }
  if (act_path.empty()) {
    act_path = FindFile_(case_path, "actinide_masses");
  }

  std::vector<double> a_days;
  std::vector<cyclus::CompMap> a_series;
  if (!ReadMassTable_(act_path, case_name, &a_days, &a_series)) {
    throw cyclus::ValueError("ads: cannot read an actinide mass table in " +
                             case_path);
  }

  // --- fission products (same time axis) -----------------------------------
  std::string fp_path = FindFile_(case_path, "fission_product_masses");
  std::vector<double> f_days;
  std::vector<cyclus::CompMap> f_series;
  bool have_fp = ReadMassTable_(fp_path, case_name, &f_days, &f_series);

  if (have_fp && f_series.size() != a_series.size()) {
    LOG(cyclus::LEV_WARN, "ads") << prototype() << ": case '" << case_name
        << "' fission-product table has " << f_series.size()
        << " rows but the actinide table has " << a_series.size()
        << "; ignoring fission products for this case.";
    have_fp = false;
  }
  if (!have_fp) {
    LOG(cyclus::LEV_WARN, "ads") << prototype() << ": case '" << case_name
        << "' has no matching fission-product table; discharged material will "
           "contain actinides only and mass will not be conserved.";
  }

  // --- combine -------------------------------------------------------------
  c.time = a_days;
  for (size_t t = 0; t < a_series.size(); ++t) {
    cyclus::CompMap cm = a_series[t];
    double a_kg = 0.0, f_kg = 0.0;
    for (cyclus::CompMap::const_iterator it = a_series[t].begin();
         it != a_series[t].end(); ++it) {
      a_kg += it->second;
    }
    if (have_fp) {
      for (cyclus::CompMap::const_iterator it = f_series[t].begin();
           it != f_series[t].end(); ++it) {
        cm[it->first] += it->second;
        f_kg += it->second;
      }
    }
    c.comp_series.push_back(cm);
    c.actinide_kg.push_back(a_kg);
    c.fp_kg.push_back(f_kg);
    c.total_kg.push_back(a_kg + f_kg);
  }

  // --- optional series -----------------------------------------------------
  std::vector<double> d2;
  ReadSeries(FindFile_(case_path, "keff"), 0, &d2, &c.keff);

  std::vector<double> d3, pw;
  ReadSeries(FindFile_(case_path, "power"), 0, &d3, &pw);
  if (!pw.empty()) {
    c.power_mw = pw.front();
  } else {
    c.power_mw = power_level;
    LOG(cyclus::LEV_WARN, "ads") << prototype()
        << ": no power file for case '" << case_name
        << "'; using facility power for matching.";
  }

  // --- input fingerprint from the t=0 actinide vector -----------------------
  double act = 0.0, ma = 0.0, pu = 0.0, u = 0.0;
  const cyclus::CompMap& c0 = a_series.front();
  for (cyclus::CompMap::const_iterator it = c0.begin(); it != c0.end(); ++it) {
    int z = it->first / 10000000;
    double g = it->second;
    if (z < 89) continue;
    act += g;
    if (z == 94) pu += g;
    else if (z == 93 || z == 95 || z == 96) ma += g;
    else if (z == 92) u += g;
  }
  c.in_ma_frac = act > 0 ? ma / act : 0.0;
  c.in_pu_frac = act > 0 ? pu / act : 0.0;
  c.in_u_frac  = act > 0 ? u  / act : 0.0;

  LOG(cyclus::LEV_INFO3, "ads") << prototype() << ": case '" << case_name
      << "': " << c.time.size() << " time points ("
      << c.time.front() << ".." << c.time.back() << " d), "
      << c.actinide_kg.front() << " kg actinide at t=0, power "
      << c.power_mw << " MW.";

  return c;
}

bool ads::EndsWith_(const std::string& s, const std::string& suf) const {
  return s.size() >= suf.size() &&
         s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ===========================================================================
//  Nuclide parsing: "Np237", "Am242m", "Cs137" -> ZZAAAM id (0 if invalid)
// ===========================================================================

int ads::NucId_(const std::string& tok) const {
  std::string t = tok;
  t.erase(std::remove_if(t.begin(), t.end(),
                         [](unsigned char ch) {
                           return ch == '-' || std::isspace(ch);
                         }),
          t.end());
  if (t.empty()) return 0;

  size_t pos = 0;
  while (pos < t.size() && std::isalpha((unsigned char)t[pos])) pos++;
  if (pos == 0 || pos == t.size()) return 0;

  std::string sym = t.substr(0, pos);
  std::string a_str = t.substr(pos);

  int meta = 0;
  if (!a_str.empty() && (a_str[a_str.size() - 1] == 'm' ||
                         a_str[a_str.size() - 1] == 'M')) {
    meta = 1;
    a_str = a_str.substr(0, a_str.size() - 1);
  }

  sym[0] = std::toupper((unsigned char)sym[0]);
  for (size_t i = 1; i < sym.size(); ++i) {
    sym[i] = std::tolower((unsigned char)sym[i]);
  }
  int A = std::atoi(a_str.c_str());
  // Reject impossible mass numbers (e.g. the bogus "Am642" in some files).
  if (A <= 0 || A > 300) return 0;

  static const std::unordered_map<std::string, int> Z = {
      {"H",1},{"He",2},{"Li",3},{"Be",4},{"B",5},{"C",6},{"N",7},{"O",8},
      {"F",9},{"Ne",10},{"Na",11},{"Mg",12},{"Al",13},{"Si",14},{"P",15},
      {"S",16},{"Cl",17},{"Ar",18},{"K",19},{"Ca",20},{"Sc",21},{"Ti",22},
      {"V",23},{"Cr",24},{"Mn",25},{"Fe",26},{"Co",27},{"Ni",28},{"Cu",29},
      {"Zn",30},{"Ga",31},{"Ge",32},{"As",33},{"Se",34},{"Br",35},{"Kr",36},
      {"Rb",37},{"Sr",38},{"Y",39},{"Zr",40},{"Nb",41},{"Mo",42},{"Tc",43},
      {"Ru",44},{"Rh",45},{"Pd",46},{"Ag",47},{"Cd",48},{"In",49},{"Sn",50},
      {"Sb",51},{"Te",52},{"I",53},{"Xe",54},{"Cs",55},{"Ba",56},{"La",57},
      {"Ce",58},{"Pr",59},{"Nd",60},{"Pm",61},{"Sm",62},{"Eu",63},{"Gd",64},
      {"Tb",65},{"Dy",66},{"Ho",67},{"Er",68},{"Tm",69},{"Yb",70},{"Lu",71},
      {"Hf",72},{"Ta",73},{"W",74},{"Re",75},{"Os",76},{"Ir",77},{"Pt",78},
      {"Au",79},{"Hg",80},{"Tl",81},{"Pb",82},{"Bi",83},{"Po",84},{"At",85},
      {"Rn",86},{"Fr",87},{"Ra",88},{"Ac",89},{"Th",90},{"Pa",91},{"U",92},
      {"Np",93},{"Pu",94},{"Am",95},{"Cm",96},{"Bk",97},{"Cf",98},{"Es",99},
      {"Fm",100},{"Md",101},{"No",102},{"Lr",103}};

  std::unordered_map<std::string, int>::const_iterator it = Z.find(sym);
  if (it == Z.end()) return 0;
  return it->second * 10000000 + A * 10000 + meta;
}

// ===========================================================================

void ads::Record_(const std::string& event, const std::string& matched_case,
                  double value) {
  context()
      ->NewDatum("AdsEvents")
      ->AddVal("AgentId", id())
      ->AddVal("Time", context()->time())
      ->AddVal("Event", event)
      ->AddVal("MatchedCase", matched_case)
      ->AddVal("Value", value)
      ->Record();
}

extern "C" cyclus::Agent* Constructads(cyclus::Context* ctx) {
  return new ads(ctx);
}

}  // namespace einstein
