#include "us_inventory.h"
 
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
 
#include "sqlite_db.h"
 
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
     << ", preferred_facility=" << preferred_facility
     << ", storage_type=" << storage_type
     << ", strict_filters=" << strict_filters
     << ")";
  return ss.str();
}
 
void us_inventory::EnterNotify() {
  cyclus::Facility::EnterNotify();
 
  if (outcommod.empty()) {
    throw cyclus::ValueError("us_inventory: outcommod is required.");
  }
  if (data_dir.empty()) {
    throw cyclus::ValueError("us_inventory: data_dir is required.");
  }
 
  // Validate the selection_policy early so the user gets a clear error.
  static const char* valid_policies[] = {
      "first", "older", "newer",
      "highest_burnup", "lowest_burnup",
      "highest_enrichment", "lowest_enrichment",
      "highest_initial_uranium", "lowest_initial_uranium",
      "highest_fissile", "lowest_fissile",
      "highest_plutonium", "lowest_plutonium",
      "highest_minor_actinide", "lowest_minor_actinide"};
  const size_t n_policies = sizeof(valid_policies) / sizeof(valid_policies[0]);
  bool policy_ok = false;
  for (size_t i = 0; i < n_policies; ++i) {
    if (selection_policy == valid_policies[i]) { policy_ok = true; break; }
  }
  if (!policy_ok) {
    throw cyclus::ValueError(
        "us_inventory: unknown selection_policy '" + selection_policy + "'.");
  }
 
  // Validate the storage-type filter value.
  if (!storage_type.empty() && storage_type != "wet" && storage_type != "dry") {
    throw cyclus::ValueError(
        "us_inventory: storage_type must be 'wet' or 'dry' (got '" +
        storage_type + "').");
  }
 
  bins_.clear();
  idx_.clear();
  total_inventory_kg_ = 0.0;
  fallback_warned_ = false;
 
  LoadAllData_(data_dir);
  AttachCoordinates_();   // stamps lat/long onto every assembly
 
  // Warn early if an active filter matches nothing (usually a typo).
  if (!preferred_facility.empty() || !storage_type.empty()) {
    bool any = false;
    for (size_t i = 0; i < bins_.size(); ++i) {
      if (PassesFilters_(bins_[i])) { any = true; break; }
    }
    if (!any) {
      LOG(cyclus::LEV_WARN, "us_inventory") << prototype()
          << ": no assemblies match the active filter (preferred_facility='"
          << preferred_facility << "', storage_type='" << storage_type
          << "').";
    }
  }
 
  // Restore persisted masses on restart, else snapshot the initial masses.
  if (!remaining_kg_.empty()) {
    if (remaining_kg_.size() != bins_.size()) {
      throw cyclus::ValueError(
          "us_inventory: persisted remaining_kg_ length does not match the "
          "number of assemblies loaded. Did the data folder change after a "
          "checkpoint?");
    }
    total_inventory_kg_ = 0.0;
    for (size_t i = 0; i < bins_.size(); ++i) {
      bins_[i].available_kg = remaining_kg_[i];
      total_inventory_kg_  += remaining_kg_[i];
    }
  } else {
    remaining_kg_.resize(bins_.size());
    for (size_t i = 0; i < bins_.size(); ++i) {
      remaining_kg_[i] = bins_[i].available_kg;
    }
  }
}
 
// ===========================================================================
//  Data loading: one aggregate inventory read from a folder of JSONL files
// ===========================================================================
 
// A single Standards 5.0.1 line is a JSON string whose payload is a
// Python-dict literal (single quotes; grams values as quoted scientific
// notation). The tiny tokenizer below accepts BOTH ' and " as string
// delimiters, so plant names containing an apostrophe are safe.
//
// namespace, so the helpers live in usinv_detail instead.
namespace usinv_detail {
 
struct ParsedAssembly {
  std::string facility, location, assembly_id, storage_type;
  double enrichment = 0.0, burnup = 0.0, discharge_year = 0.0,
         initial_uranium_kg = 0.0;
  std::vector<std::pair<std::string, double> > grams;
};
 
class DictParser {
 public:
  explicit DictParser(const std::string& s) : s_(s), i_(0) {}
 
  void SkipWs() {
    while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) i_++;
  }
  char Peek() { SkipWs(); return i_ < s_.size() ? s_[i_] : '\0'; }
  void Expect(char c) {
    SkipWs();
    if (i_ >= s_.size() || s_[i_] != c) {
      throw std::runtime_error(std::string("us_inventory JSONL: expected '") +
                               c + "'");
    }
    i_++;
  }
  bool ValueIsString() { char c = Peek(); return c == '\'' || c == '"'; }
  std::string Str() {
    SkipWs();
    char q = s_[i_++];  // opening quote
    std::string out;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) { out.push_back(s_[i_++]); continue; }
      if (c == q) break;
      out.push_back(c);
    }
    return out;
  }
  double Num() {
    SkipWs();
    size_t start = i_;
    while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' &&
           !std::isspace((unsigned char)s_[i_])) {
      i_++;
    }
    return std::strtod(s_.substr(start, i_ - start).c_str(), NULL);
  }
 
 private:
  const std::string& s_;
  size_t i_;
};
 
// Strip the outer JSON-string wrapper of a raw line -> inner dict text.
std::string UnwrapLine(const std::string& line) {
  size_t a = line.find_first_of('"');
  size_t b = line.find_last_of('"');
  if (a == std::string::npos || b <= a) return line;  // already bare
  std::string inner;
  for (size_t k = a + 1; k < b; ++k) {
    char c = line[k];
    if (c == '\\' && k + 1 < b) { inner.push_back(line[++k]); continue; }
    inner.push_back(c);
  }
  return inner;
}
 
ParsedAssembly ParseAssemblyLine(const std::string& raw) {
  std::string inner = UnwrapLine(raw);
  DictParser p(inner);
  ParsedAssembly a;
  p.Expect('{');
  while (true) {
    if (p.Peek() == '}') { p.Expect('}'); break; }
    std::string key = p.Str();
    p.Expect(':');
    if (key == "grams") {
      p.Expect('{');
      while (true) {
        if (p.Peek() == '}') { p.Expect('}'); break; }
        std::string nuc = p.Str();
        p.Expect(':');
        double g = p.ValueIsString() ? std::strtod(p.Str().c_str(), NULL)
                                     : p.Num();
        if (g > 0.0) a.grams.push_back(std::make_pair(nuc, g));
        if (p.Peek() == ',') p.Expect(','); else { p.Expect('}'); break; }
      }
    } else {
      bool is_str = p.ValueIsString();
      std::string sval = is_str ? p.Str() : std::string();
      double dval = is_str ? 0.0 : p.Num();
      if      (key == "facility_name")           a.facility = sval;
      else if (key == "storage_location_name")   a.location = sval;
      else if (key == "assembly_identifier")     a.assembly_id = sval;
      else if (key == "storage_type_name")       a.storage_type = sval;
      else if (key == "initial_uranium_kg")      a.initial_uranium_kg = dval;
      else if (key == "initial_enrichment")      a.enrichment = dval;
      else if (key == "max_burnup_mwd_per_mthm") a.burnup = dval;
      else if (key == "discharge_year")          a.discharge_year = dval;
      // analysis_date and any other keys are ignored.
    }
    if (p.Peek() == ',') p.Expect(','); else { p.Expect('}'); break; }
  }
  return a;
}
 
}  // namespace usinv_detail
 
void us_inventory::LoadAllData_(const std::string& dir) {
  DIR* d = opendir(dir.c_str());
  if (!d) throw cyclus::ValueError("us_inventory: cannot open folder " + dir);
 
  struct dirent* ent;
  int nfiles = 0;
  while ((ent = readdir(d)) != NULL) {
    std::string fn = ent->d_name;
    bool ok = (fn.size() > 4 && fn.substr(fn.size() - 4) == ".txt") ||
              (fn.find(".jsonl") != std::string::npos);
    if (!ok) continue;
    LoadDataJSONL_(dir + "/" + fn);
    nfiles++;
  }
  closedir(d);
 
  if (nfiles == 0) {
    throw cyclus::ValueError("us_inventory: no .txt/.jsonl files in " + dir);
  }
  if (bins_.empty()) {
    throw cyclus::ValueError("us_inventory: no assemblies loaded from " + dir);
  }
 
  LOG(cyclus::LEV_INFO2, "us_inventory")
      << prototype() << ": loaded " << bins_.size() << " assemblies from "
      << nfiles << " file(s).";
}
 
void us_inventory::LoadDataJSONL_(const std::string& path) {
  std::ifstream f(path.c_str());
  if (!f) throw cyclus::ValueError("us_inventory: cannot open " + path);
 
  std::string line;
  bool first = true;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (first) {
      first = false;
      // Skip the Postgres export header line ("row_to_json").
      if (line.find('{') == std::string::npos) continue;
    }
 
    usinv_detail::ParsedAssembly pa = usinv_detail::ParseAssemblyLine(line);
    if (pa.grams.empty()) continue;
 
    // Build the composition map and the running gram total.
    cyclus::CompMap cm;
    double total_g = 0.0;
    for (size_t k = 0; k < pa.grams.size(); ++k) {
      int nid = NucIdFromString_(pa.grams[k].first);
      cm[nid] += pa.grams[k].second;
      total_g += pa.grams[k].second;
    }
    if (total_g <= 0.0) continue;
 
    // Precompute composition fractions used by the selection policies.
    double pu_g = 0.0, ma_g = 0.0, fis_g = 0.0;
    for (cyclus::CompMap::iterator it = cm.begin(); it != cm.end(); ++it) {
      int nid = it->first;
      double g = it->second;
      int z = nid / 10000000;           // ZZAAAM -> atomic number
      if (z == 94) {
        pu_g += g;                       // all plutonium
      } else if (z == 93 || z == 95 || z == 96) {
        ma_g += g;                       // minor actinides: Np, Am, Cm
      }
      // fissile: U-233, U-235, Pu-239, Pu-241 (ids in ZZAAAM)
      if (nid == 922330000 || nid == 922350000 ||
          nid == 942390000 || nid == 942410000) {
        fis_g += g;
      }
    }
 
    Bin b;
    // The raw assembly_identifier is not unique across the dataset, so the
    // internal key glues on facility, location, and a running index.
    {
      std::ostringstream key;
      key << pa.facility << "|" << pa.location << "|" << pa.assembly_id
          << "|" << bins_.size();
      b.assembly_id = key.str();
    }
    b.facility           = pa.facility;
    b.location           = pa.location;
    b.storage_type       = pa.storage_type;
    b.orig_id            = pa.assembly_id;
    b.available_kg       = total_g / 1000.0;
    b.discharge_date     = pa.discharge_year;
    b.burnup             = pa.burnup;
    b.enrichment         = pa.enrichment;
    b.initial_uranium_kg = pa.initial_uranium_kg;
    b.fissile_frac       = fis_g / total_g;
    b.pu_frac            = pu_g  / total_g;
    b.ma_frac            = ma_g  / total_g;
    b.comp               = cyclus::Composition::CreateFromMass(cm);
    // latitude/longitude filled later by AttachCoordinates_()
 
    idx_[b.assembly_id] = bins_.size();
    bins_.push_back(b);
    total_inventory_kg_ += b.available_kg;
  }
}
 
// Attach lat/long to every assembly, looked up once per distinct plant.
void us_inventory::AttachCoordinates_() {
  if (coordinates_file.empty()) return;
 
  cyclus::SqliteDb db(coordinates_file, /*readonly=*/true);
  db.open();
 
  std::map<std::string, std::pair<double, double> > cache;
  for (size_t i = 0; i < bins_.size(); ++i) {
    const std::string& fac = bins_[i].facility;
 
    std::map<std::string, std::pair<double, double> >::iterator it =
        cache.find(fac);
    if (it == cache.end()) {
      double lat = 0.0, lon = 0.0;
      cyclus::SqlStatement::Ptr st = db.Prepare(
          "SELECT Lat, Long FROM reactors_coordinates "
          "WHERE Name = ? OR Name LIKE ? ORDER BY (Name = ?) DESC LIMIT 1;");
      std::string like = fac + "%";
      st->BindText(1, fac.c_str());
      st->BindText(2, like.c_str());
      st->BindText(3, fac.c_str());
      if (st->Step()) {
        lat = st->GetDouble(0);
        lon = st->GetDouble(1);
      } else {
        LOG(cyclus::LEV_WARN, "us_inventory")
            << prototype() << ": no coordinates found for plant '" << fac
            << "'; leaving its assemblies at (0,0).";
      }
      it = cache.insert(std::make_pair(fac, std::make_pair(lat, lon))).first;
    }
 
    bins_[i].latitude  = it->second.first;
    bins_[i].longitude = it->second.second;
  }
 
  db.close();
}
 
// Record the source (incl. coordinates) of one supplied trade.
void us_inventory::RecordSupply_(const Bin& b, double qty) {
  context()
      ->NewDatum("us_inventorySupply")
      ->AddVal("AgentId", id())
      ->AddVal("Time", context()->time())
      ->AddVal("Facility", b.facility)
      ->AddVal("Location", b.location)
      ->AddVal("StorageType", b.storage_type)
      ->AddVal("AssemblyId", b.orig_id)
      ->AddVal("InitialUraniumKg", b.initial_uranium_kg)
      ->AddVal("Latitude", b.latitude)
      ->AddVal("Longitude", b.longitude)
      ->AddVal("Quantity", qty)
      ->AddVal("SelectionPolicy", selection_policy)
      ->Record();
}
 
// ===========================================================================
//  Bin selection: filters (facility / storage type) + ranking policy
// ===========================================================================
 
bool us_inventory::PassesFilters_(const Bin& b) const {
  if (!preferred_facility.empty() && b.facility != preferred_facility) {
    return false;
  }
  if (!storage_type.empty() && b.storage_type != storage_type) {
    return false;
  }
  return true;
}
 
size_t us_inventory::ChooseBin_(double req_qty, bool full_only,
                                bool apply_filters) const {
  size_t best = bins_.size();
 
  for (size_t i = 0; i < bins_.size(); ++i) {
    const Bin& b = bins_[i];
 
    if (b.comp == NULL || b.available_kg <= cyclus::eps()) continue;
    if (full_only && b.available_kg < req_qty) continue;
    if (apply_filters && !PassesFilters_(b)) continue;
 
    // First eligible bin — always accept it as the initial candidate.
    if (best == bins_.size()) { best = i; continue; }
 
    const Bin& cur = bins_[best];
 
    if (selection_policy == "first") {
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
    } else if (selection_policy == "highest_initial_uranium") {
      if (b.initial_uranium_kg > cur.initial_uranium_kg) best = i;
    } else if (selection_policy == "lowest_initial_uranium") {
      if (b.initial_uranium_kg < cur.initial_uranium_kg) best = i;
    } else if (selection_policy == "highest_fissile") {
      if (b.fissile_frac > cur.fissile_frac) best = i;
    } else if (selection_policy == "lowest_fissile") {
      if (b.fissile_frac < cur.fissile_frac) best = i;
    } else if (selection_policy == "highest_plutonium") {
      if (b.pu_frac > cur.pu_frac) best = i;
    } else if (selection_policy == "lowest_plutonium") {
      if (b.pu_frac < cur.pu_frac) best = i;
    } else if (selection_policy == "highest_minor_actinide") {
      if (b.ma_frac > cur.ma_frac) best = i;
    } else if (selection_policy == "lowest_minor_actinide") {
      if (b.ma_frac < cur.ma_frac) best = i;
    }
  }
 
  return best;
}
 
size_t us_inventory::ChooseInPool_(double req_qty, bool pool_filtered,
                                   bool* full) const {
  size_t i = ChooseBin_(req_qty, /*full_only=*/true, pool_filtered);
  if (i != bins_.size()) { *full = true; return i; }
  if (allow_partial) {
    i = ChooseBin_(req_qty, /*full_only=*/false, pool_filtered);
    if (i != bins_.size()) { *full = false; return i; }
  }
  *full = false;
  return bins_.size();
}
 
// ===========================================================================
//  Bidding and trading
// ===========================================================================
 
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
 
  // Under STRICT filters the facility can only ever supply matching assemblies,
  // so it must bid only that pool (otherwise it would over-promise). Under soft
  // filters (or no filters) it can supply the whole inventory via fallback.
  bool have_filters = !preferred_facility.empty() || !storage_type.empty();
  bool restrict = strict_filters && have_filters;
 
  cyclus::CompMap blended;
  double blend_total = 0.0;
  for (size_t i = 0; i < bins_.size(); ++i) {
    const Bin& b = bins_[i];
    if (b.available_kg <= cyclus::eps() || b.comp == NULL) continue;
    if (restrict && !PassesFilters_(b)) continue;
    const cyclus::CompMap& cm = b.comp->mass();
    for (cyclus::CompMap::const_iterator it = cm.begin(); it != cm.end(); ++it) {
      blended[it->first] += it->second * b.available_kg;
    }
    blend_total += b.available_kg;
  }
 
  if (blend_total <= cyclus::eps()) {
    return ports;
  }
 
  double max_qty = std::min(blend_total, throughput_kg);
  if (max_qty <= cyclus::eps()) {
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
 
void us_inventory::GetMatlTrades(
    const std::vector<cyclus::Trade<cyclus::Material> >& trades,
    std::vector<std::pair<cyclus::Trade<cyclus::Material>,
                          cyclus::Material::Ptr> >& responses) {
  double remaining_throughput = throughput_kg;
  bool have_filters = !preferred_facility.empty() || !storage_type.empty();

  for (size_t t = 0; t < trades.size(); ++t) {
    const cyclus::Trade<cyclus::Material>& tr = trades[t];
    double req_qty = tr.amt;

    if (req_qty <= cyclus::eps()) continue;
    if (remaining_throughput <= cyclus::eps()) break;
    if (total_inventory_kg_ <= cyclus::eps()) break;

    // Fill one trade from as many assemblies as it takes, up to the remaining
    // per-timestep throughput. Each assembly drawn is recorded separately in
    // us_inventorySupply, so provenance stays at assembly resolution even
    // though the delivered material is a mass-weighted blend.
    std::vector<double> draw_kg(bins_.size(), 0.0);
    double filled = 0.0;
    size_t n_drawn = 0;

    while (filled + cyclus::eps() < req_qty &&
           remaining_throughput > cyclus::eps() &&
           total_inventory_kg_ > cyclus::eps()) {
      double want = std::min(req_qty - filled, remaining_throughput);

      bool full = false;
      size_t chosen_i =
          ChooseInPool_(want, /*pool_filtered=*/have_filters, &full);

      if (chosen_i == bins_.size() && have_filters) {
        if (strict_filters) {
          if (n_drawn == 0) {
            LOG(cyclus::LEV_WARN, "us_inventory") << prototype()
                << ": no assemblies match filter (facility='"
                << preferred_facility << "', storage_type='" << storage_type
                << "') for a request of " << req_qty
                << " kg; supplying nothing.";
          }
          break;
        }
        // Soft: relax ALL filters at once for the rest of this trade.
        chosen_i = ChooseInPool_(want, /*pool_filtered=*/false, &full);
        if (chosen_i != bins_.size() && !fallback_warned_) {
          LOG(cyclus::LEV_WARN, "us_inventory") << prototype()
              << ": filter (facility='" << preferred_facility
              << "', storage_type='" << storage_type
              << "') exhausted; falling back to the rest of the inventory.";
          fallback_warned_ = true;
        }
      }

      if (chosen_i == bins_.size()) break;  // pool exhausted

      double actual = std::min({want, bins_[chosen_i].available_kg,
                                remaining_throughput});
      if (actual <= cyclus::eps()) break;

      Bin& b = bins_[chosen_i];
      b.available_kg       -= actual;
      total_inventory_kg_  -= actual;
      remaining_throughput -= actual;
      remaining_kg_[chosen_i] = b.available_kg;
      draw_kg[chosen_i]    += actual;
      filled               += actual;
      n_drawn++;

      RecordSupply_(b, actual);  // one row per assembly

      LOG(cyclus::LEV_INFO5, "us_inventory")
          << prototype() << " drew " << actual << " kg from " << b.facility
          << " / " << b.location << " assembly " << b.orig_id
          << " (policy=" << selection_policy << ")";
    }

    if (filled <= cyclus::eps()) continue;

    cyclus::Composition::Ptr comp = BlendedComp_(draw_kg);
    cyclus::Material::Ptr mat = cyclus::Material::CreateUntracked(filled, comp);
    responses.push_back(std::make_pair(tr, mat));

    LOG(cyclus::LEV_INFO4, "us_inventory")
        << prototype() << " sent " << filled << " kg of " << outcommod
        << " blended from " << n_drawn << " assemblies.";
  }
}
 
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
 
// ===========================================================================
//  Nuclide parsing (element symbol + mass number -> ZZAAAM id)
// ===========================================================================
 
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
 
  // Strip trailing metastable indicator ('m'), e.g. "108m" -> "108"
  size_t m_pos = a_str.find_first_not_of("0123456789");
  if (m_pos != std::string::npos) {
    a_str = a_str.substr(0, m_pos);
  }
 
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
      {"Cm", 96}, {"Bk", 97}, {"Cf", 98}, {"Es", 99}, {"Fm", 100},
      {"Md", 101},{"No", 102},{"Lr", 103}};
 
  std::unordered_map<std::string, int>::const_iterator it = Z.find(sym);
 
  if (it == Z.end()) {
    throw std::runtime_error(
        "Unknown element symbol in nuclide: '" + s + "' parsed as '" +
        sym + "'");
  }
 
  int z = it->second;
  int zzaaam = z * 10000000 + A * 10000;
 
  return zzaaam;
}
 
extern "C" cyclus::Agent* Constructus_inventory(cyclus::Context* ctx) {
  return new us_inventory(ctx);
}
 
}  // namespace einstein
