#pragma once

// Readers/writers for timed op streams (CSV, JSON lines) and the JSON run export used by the
// visualizer. Formats are specified in docs/EVENT_FORMAT.md.

#include <optional>
#include <string>
#include <string_view>

#include "lob/market_sim.hpp"

namespace lob::sim {

/// "t_ns,op,id,side,type,tif,price,qty"
std::string csv_header();
std::string to_csv_line(const TimedOp& op);
/// nullopt for the header, blank lines, '#' comments and malformed lines.
std::optional<TimedOp> parse_csv_line(std::string_view line);

/// One flat JSON object per line: {"t":..,"op":"S","id":..,"side":"B","type":"L","tif":"GTC","price":..,"qty":..}
std::string to_jsonl_line(const TimedOp& op);
std::optional<TimedOp> parse_jsonl_line(std::string_view line);

/// The visualizer's run format ("lob-run-v1"): config, ops, events and periodic book snapshots, as
/// compact arrays of integers.
std::string run_to_json(const Run& run);
/// The same JSON assigned to `window.LOB_RUN`, so a page can load it with a <script> tag from file://.
std::string run_to_js(const Run& run);

}  // namespace lob::sim
