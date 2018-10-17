/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <ostream>
//Hash		
#include <iostream>
//end_Hash
#include <thread>

#include "misc.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "syzygy/tbprobe.h"
#include "polybook.h"

using std::string;

UCI::OptionsMap Options; // Global object

namespace UCI {

/// 'On change' actions, triggered by an option's value change
void on_clear_hash(const Option&) { Search::clear(); }
void on_hash_size(const Option& o) { TT.resize(o); }
void on_large_pages(const Option& o) { TT.resize(o); }  // warning is ok, will be removed
void on_logger(const Option& o) { start_logger(o); }
void on_threads(const Option& o) { Threads.set(o); }
void on_tb_path(const Option& o) { Tablebases::init(o); }
//Hash	
void on_HashFile(const Option& o) { TT.set_hash_file_name(o); }
void SaveHashtoFile(const Option&) { TT.save(); }
void LoadHashfromFile(const Option&) { TT.load(); }
void LoadEpdToHash(const Option&) { TT.load_epd_to_hash(); }
//end_Hash

void on_book_file(const Option& o) { polybook.init(o); }
void on_best_book_move(const Option& o) { polybook.set_best_book_move(o); }
void on_book_depth(const Option& o) { polybook.set_book_depth(o); }

/// Our case insensitive less() function as required by UCI protocol
bool CaseInsensitiveLess::operator() (const string& s1, const string& s2) const {

  return std::lexicographical_compare(s1.begin(), s1.end(), s2.begin(), s2.end(),
         [](char c1, char c2) { return tolower(c1) < tolower(c2); });
}


/// init() initializes the UCI options to their hard-coded default values

void init(OptionsMap& o) {

  // at most 2^32 clusters.
  constexpr int MaxHashMB = Is64Bit ? 131072 : 2048;

  unsigned n = std::thread::hardware_concurrency();
  if (!n) n = 1;
  
  o["Debug Log File"]        << Option("", on_logger);
  o["Contempt"]              << Option(21, -100, 100);
  o["Analysis_CT"]           << Option("Both var Off var White var Black var Both", "Both");
  o["Threads"]               << Option(n, unsigned(1), unsigned(512), on_threads);
  o["Hash"]                  << Option(16, 1, MaxHashMB, on_hash_size);
  o["BookFile"]              << Option("Cerebellum_Light_Poly.bin", on_book_file);
  o["BestBookMove"]          << Option(true, on_best_book_move);
  o["BookDepth"]             << Option(255, 1, 255, on_book_depth);
  o["Clear Hash"]            << Option(on_clear_hash);
  o["Ponder"]                << Option(false);
  o["MultiPV"]               << Option(1, 1, 500);
  o["Move Overhead"]         << Option(30, 0, 5000);
  o["UCI_Chess960"]          << Option(false);
  o["NeverClearHash"]        << Option(false);
  o["HashFile"]              << Option("hash.hsh", on_HashFile);
  o["SaveHashtoFile"]        << Option(SaveHashtoFile);
  o["LoadHashfromFile"]      << Option(LoadHashfromFile);
  o["LoadEpdToHash"]         << Option(LoadEpdToHash);
  o["UCI_AnalyseMode"]       << Option(false);
  o["Large Pages"]           << Option(true, on_large_pages);
  o["ICCF Analyzes"]         << Option(0, 0,  8);
  o["NullMove"]              << Option(true);
  o["SyzygyPath"]            << Option("<empty>", on_tb_path);
  o["SyzygyProbeDepth"]      << Option(1, 1, 100);
  o["SyzygyProbeLimit"]      << Option(7, 0, 7);
}


/// operator<<() is used to print all the options default values in chronological
/// insertion order (the idx field) and in the format defined by the UCI protocol.

std::ostream& operator<<(std::ostream& os, const OptionsMap& om) {

  for (size_t idx = 0; idx < om.size(); ++idx)
      for (const auto& it : om)
          if (it.second.idx == idx)
          {
              const Option& o = it.second;
              os << "\noption name " << it.first << " type " << o.type;

              if (o.type == "string" || o.type == "check" || o.type == "combo")
                  os << " default " << o.defaultValue;

              if (o.type == "spin")
                  os << " default " << int(stof(o.defaultValue))
                     << " min "     << o.min
                     << " max "     << o.max;

              break;
          }

  return os;
}


/// Option class constructors and conversion operators

Option::Option(const char* v, OnChange f) : type("string"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = v; }

Option::Option(bool v, OnChange f) : type("check"), min(0), max(0), on_change(f)
{ defaultValue = currentValue = (v ? "true" : "false"); }

Option::Option(OnChange f) : type("button"), min(0), max(0), on_change(f)
{}

Option::Option(double v, int minv, int maxv, OnChange f) : type("spin"), min(minv), max(maxv), on_change(f)
{ defaultValue = currentValue = std::to_string(v); }

Option::Option(const char* v, const char* cur, OnChange f) : type("combo"), min(0), max(0), on_change(f)
{ defaultValue = v; currentValue = cur; }

Option::operator double() const {
  assert(type == "check" || type == "spin");
  return (type == "spin" ? stof(currentValue) : currentValue == "true");
}

Option::operator std::string() const {
  assert(type == "string");
  return currentValue;
}

bool Option::operator==(const char* s) const {
  assert(type == "combo");
  return    !CaseInsensitiveLess()(currentValue, s)
         && !CaseInsensitiveLess()(s, currentValue);
}


/// operator<<() inits options and assigns idx in the correct printing order

void Option::operator<<(const Option& o) {

  static size_t insert_order = 0;

  *this = o;
  idx = insert_order++;
}


/// operator=() updates currentValue and triggers on_change() action. It's up to
/// the GUI to check for option's limits, but we could receive the new value from
/// the user by console window, so let's check the bounds anyway.

Option& Option::operator=(const string& v) {

  assert(!type.empty());

  if (   (type != "button" && v.empty())
      || (type == "check" && v != "true" && v != "false")
      || (type == "spin" && (stof(v) < min || stof(v) > max)))
      return *this;

  if (type != "button")
      currentValue = v;

  if (on_change)
      on_change(*this);

  return *this;
}

} // namespace UCI



/// Tuning Framework. Fully separated from SF code, appended here to avoid
/// adding a *.cpp file and to modify Makefile.

#include <iostream>
#include <sstream>

bool Tune::update_on_last;
const UCI::Option* LastOption = nullptr;
BoolConditions Conditions;
static std::map<std::string, int> TuneResults;

string Tune::next(string& names, bool pop) {

  string name;

  do {
      string token = names.substr(0, names.find(','));

      if (pop)
          names.erase(0, token.size() + 1);

      std::stringstream ws(token);
      name += (ws >> token, token); // Remove trailing whitespace

  } while (  std::count(name.begin(), name.end(), '(')
           - std::count(name.begin(), name.end(), ')'));

  return name;
}

static void on_tune(const UCI::Option& o) {

  if (!Tune::update_on_last || LastOption == &o)
      Tune::read_options();
}

static void make_option(const string& n, int v, const SetRange& r) {

  // Do not generate option when there is nothing to tune (ie. min = max)
  if (r(v).first == r(v).second)
      return;

  if (TuneResults.count(n))
      v = TuneResults[n];

  Options[n] << UCI::Option(v, r(v).first, r(v).second, on_tune);
  LastOption = &Options[n];

  // Print formatted parameters, ready to be copy-pasted in fishtest
  std::cout << n << ","
            << v << ","
            << r(v).first << "," << r(v).second << ","
            << (r(v).second - r(v).first) / 20.0 << ","
            << "0.0020"
            << std::endl;
}

template<> void Tune::Entry<int>::init_option() { make_option(name, value, range); }

template<> void Tune::Entry<int>::read_option() {
  if (Options.count(name))
      value = Options[name];
}

template<> void Tune::Entry<Value>::init_option() { make_option(name, value, range); }

template<> void Tune::Entry<Value>::read_option() {
  if (Options.count(name))
      value = Value(int(Options[name]));
}

template<> void Tune::Entry<Score>::init_option() {
  make_option("m" + name, mg_value(value), range);
  make_option("e" + name, eg_value(value), range);
}

template<> void Tune::Entry<Score>::read_option() {
  if (Options.count("m" + name))
      value = make_score(Options["m" + name], eg_value(value));

  if (Options.count("e" + name))
      value = make_score(mg_value(value), Options["e" + name]);
}

// Instead of a variable here we have a PostUpdate function: just call it
template<> void Tune::Entry<Tune::PostUpdate>::init_option() {}
template<> void Tune::Entry<Tune::PostUpdate>::read_option() { value(); }


// Set binary conditions according to a probability that depends
// on the corresponding parameter value.

void BoolConditions::set() {

  static PRNG rng(now());
  static bool startup = true; // To workaround fishtest bench

  for (size_t i = 0; i < binary.size(); i++)
      binary[i] = !startup && (values[i] + int(rng.rand<unsigned>() % variance) > threshold);

  startup = false;

  for (size_t i = 0; i < binary.size(); i++)
      sync_cout << binary[i] << sync_endl;
}


// Init options with tuning session results instead of default values. Useful to
// get correct bench signature after a tuning session or to test tuned values.
// Just copy fishtest tuning results in a result.txt file and extract the
// values with:
//
// cat results.txt | sed 's/^param: \([^,]*\), best: \([^,]*\).*/  TuneResults["\1"] = int(round(\2));/'
//
// Then paste the output below, as the function body

#include <cmath>

void Tune::read_results() {

  /* ...insert your values here... */
}
