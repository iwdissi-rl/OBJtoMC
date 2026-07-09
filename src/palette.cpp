// Block Palette DB loader.
//
// Reads two artifacts:
//   1. data/block_palette_db.csv — per-block average colour catalog produced
//      by scripts/extract_block_colors.py against a vanilla MC 1.21.5 jar.
//   2. third_party/minecraft-data/pc/1.21.5/blocks.json — vendored MIT
//      metadata (opacity, luminance, name list) for the version-assert +
//      per-row opacity/luminance cross-check.
//
// All failure modes throw palette::palette_error (proposal §3.8). The CSV
// is the authoritative source; blocks.json is the cross-check oracle that
// makes drift loud at load time. See research/palette_db_proposal.md.

#include "objtomc/palette.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace objtomc::palette {

namespace {

// ---------------------------------------------------------------------------
// String helpers.
// ---------------------------------------------------------------------------
std::string trim(std::string_view sv) {
    std::size_t i = 0;
    while (i < sv.size() && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
    std::size_t j = sv.size();
    while (j > i && std::isspace(static_cast<unsigned char>(sv[j - 1]))) --j;
    return std::string(sv.substr(i, j - i));
}

bool is_blank(std::string_view sv) {
    for (char c : sv) {
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string read_text_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw palette_error("palette: cannot open file '" + p.string() + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// sRGB -> linear-RGB (proposal §3.4). Input [0, 1], output [0, 1].
float srgb_to_linear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// ---------------------------------------------------------------------------
// blocks.json minimal parser. Extracts (name, filterLight, emitLight) for
// every top-level block object in the JSON array, ignoring nested objects
// (in particular the `states[*].name` field which would otherwise collide
// with the top-level block name). Single-pass; handles JSON string escapes.
// ---------------------------------------------------------------------------
struct BlockMeta {
    int filter_light = -1;
    int emit_light   = -1;
};

void skip_string(std::string_view content, std::size_t& i) {
    // Pre: content[i] == '"'. Advances i past the closing '"'.
    ++i;
    while (i < content.size() && content[i] != '"') {
        if (content[i] == '\\' && i + 1 < content.size()) {
            i += 2;
        } else {
            ++i;
        }
    }
    if (i < content.size()) ++i;  // past closing quote
}

std::string read_string_value(std::string_view content, std::size_t& i) {
    // Pre: content[i] == '"'. Reads the quoted string into a std::string,
    // advancing i past the closing '"'. Returns the string contents.
    // Only handles JSON-style backslash escapes minimally (we never need
    // to interpret them for our three target fields — block names are
    // plain ASCII identifiers).
    ++i;
    std::size_t start = i;
    while (i < content.size() && content[i] != '"') {
        if (content[i] == '\\' && i + 1 < content.size()) {
            i += 2;
        } else {
            ++i;
        }
    }
    std::string out(content.substr(start, i - start));
    if (i < content.size()) ++i;
    return out;
}

int read_int_value(std::string_view content, std::size_t& i) {
    std::size_t start = i;
    if (i < content.size() && content[i] == '-') ++i;
    while (i < content.size() && std::isdigit(static_cast<unsigned char>(content[i]))) {
        ++i;
    }
    return std::stoi(std::string(content.substr(start, i - start)));
}

void skip_whitespace(std::string_view content, std::size_t& i) {
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
        ++i;
    }
}

std::unordered_map<std::string, BlockMeta>
parse_blocks_json_minimal(const std::string& content) {
    std::unordered_map<std::string, BlockMeta> out;
    int brace_depth = 0;
    int bracket_depth = 0;
    std::string current_name;
    BlockMeta current{};

    // Skip past the outer '['. We track depth INSIDE the outer array only,
    // so brace_depth==1 + bracket_depth==0 corresponds to the top-level of
    // each block object — the right scope to capture name/emitLight/filterLight
    // without colliding with the same-named fields nested inside states[*].
    std::size_t i = content.find('[');
    if (i == std::string::npos) {
        return out;
    }
    ++i;

    while (i < content.size()) {
        char c = content[i];

        if (c == '"') {
            const std::size_t key_start = i + 1;
            skip_string(content, i);
            const std::size_t key_end = i - 1;  // i is past the closing quote
            std::string_view key(content.data() + key_start, key_end - key_start);

            skip_whitespace(content, i);
            if (i >= content.size() || content[i] != ':') {
                // Not a key — value usage (e.g. a string element in an array).
                continue;
            }
            ++i;  // past colon
            skip_whitespace(content, i);

            if (brace_depth != 1 || bracket_depth != 0) {
                // Not at top-of-block-object scope; ignore. Don't consume the
                // value here; the main loop will walk past it naturally.
                continue;
            }
            if (key == "name") {
                if (i < content.size() && content[i] == '"') {
                    current_name = read_string_value(content, i);
                }
            } else if (key == "emitLight") {
                if (i < content.size() &&
                    (std::isdigit(static_cast<unsigned char>(content[i])) ||
                     content[i] == '-')) {
                    current.emit_light = read_int_value(content, i);
                }
            } else if (key == "filterLight") {
                if (i < content.size() &&
                    (std::isdigit(static_cast<unsigned char>(content[i])) ||
                     content[i] == '-')) {
                    current.filter_light = read_int_value(content, i);
                }
            }
            continue;
        }

        if (c == '{') {
            ++brace_depth;
            if (brace_depth == 1 && bracket_depth == 0) {
                current_name.clear();
                current = BlockMeta{};
            }
            ++i;
            continue;
        }
        if (c == '}') {
            --brace_depth;
            if (brace_depth == 0 && bracket_depth == 0 &&
                !current_name.empty() &&
                current.emit_light >= 0 && current.filter_light >= 0) {
                out.emplace(current_name, current);
            }
            ++i;
            continue;
        }
        if (c == '[') { ++bracket_depth; ++i; continue; }
        if (c == ']') { --bracket_depth; ++i; continue; }
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// version.json parser. Tiny file (5 lines, 106 bytes). Extracts the
// "minecraftVersion" string value.
// ---------------------------------------------------------------------------
std::string parse_version_json(const std::string& content) {
    constexpr std::string_view key = "\"minecraftVersion\"";
    auto pos = content.find(key);
    if (pos == std::string::npos) {
        throw palette_error(
            "palette: version.json missing 'minecraftVersion' field");
    }
    pos += key.size();
    while (pos < content.size() &&
           (content[pos] == ':' ||
            std::isspace(static_cast<unsigned char>(content[pos])))) {
        ++pos;
    }
    if (pos >= content.size() || content[pos] != '"') {
        throw palette_error(
            "palette: version.json 'minecraftVersion' value not a string");
    }
    ++pos;
    const std::size_t start = pos;
    while (pos < content.size() && content[pos] != '"') ++pos;
    return std::string(content.substr(start, pos - start));
}

// ---------------------------------------------------------------------------
// CSV parsing.
//
// Header lines start with '#'. Exactly one header line must declare
//   # mc_version: <ver>
// Body rows are 9 comma-separated columns (schema_version 2, Session 46 c′):
//   name, opacity, luminance, r, g, b, is_full_cube, texture_path,
//   renders_translucent
// Numeric ranges: opacity, luminance in [0, 15]; r, g, b in [0, 255];
// is_full_cube in {0, 1}; renders_translucent in {true, false, 1, 0}. Blank
// lines are skipped. Leading/trailing whitespace per field is trimmed.
// ---------------------------------------------------------------------------
struct CsvRow {
    std::string  name;
    int          opacity;
    int          luminance;
    int          r;
    int          g;
    int          b;
    bool         is_full_cube;
    std::string  texture_path;
    bool         renders_translucent;
};

struct CsvParseResult {
    std::string           mc_version;
    std::vector<CsvRow>   rows;
};

int parse_int_in_range(const std::string& s, int lo, int hi,
                       const std::string& field, std::size_t line_no) {
    if (s.empty()) {
        throw palette_error(
            "palette: CSV row " + std::to_string(line_no) +
            ": empty " + field + " field");
    }
    for (char c : s) {
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) continue;
        throw palette_error(
            "palette: CSV row " + std::to_string(line_no) +
            ": non-numeric " + field + " '" + s + "'");
    }
    int v;
    try {
        v = std::stoi(s);
    } catch (const std::exception&) {
        throw palette_error(
            "palette: CSV row " + std::to_string(line_no) +
            ": non-numeric " + field + " '" + s + "'");
    }
    if (v < lo || v > hi) {
        throw palette_error(
            "palette: CSV row " + std::to_string(line_no) +
            ": " + field + " " + std::to_string(v) +
            " out of range [" + std::to_string(lo) + ", " +
            std::to_string(hi) + "]");
    }
    return v;
}

CsvParseResult parse_csv(const std::string& content,
                         const std::filesystem::path& csv_path) {
    CsvParseResult result;
    std::size_t line_no = 0;
    std::size_t pos = 0;

    while (pos < content.size()) {
        std::size_t eol = content.find('\n', pos);
        if (eol == std::string::npos) eol = content.size();
        std::string_view line(content.data() + pos, eol - pos);
        pos = eol + 1;
        ++line_no;

        if (is_blank(line)) continue;
        if (!line.empty() && line.front() == '#') {
            std::string body = trim(line.substr(1));
            constexpr std::string_view tag = "mc_version:";
            if (body.size() >= tag.size() &&
                body.compare(0, tag.size(), tag) == 0) {
                if (!result.mc_version.empty()) {
                    throw palette_error(
                        "palette: CSV has multiple '# mc_version:' headers in '" +
                        csv_path.string() + "'");
                }
                result.mc_version = trim(body.substr(tag.size()));
                if (result.mc_version.empty()) {
                    throw palette_error(
                        "palette: CSV '# mc_version:' header is empty in '" +
                        csv_path.string() + "'");
                }
            }
            continue;
        }

        // Body row: split into 8 fields. The texture_path field is the last
        // and may be empty (no trailing whitespace required).
        std::vector<std::string> fields;
        std::size_t fpos = 0;
        while (true) {
            std::size_t comma = line.find(',', fpos);
            if (comma == std::string_view::npos) {
                fields.emplace_back(trim(line.substr(fpos)));
                break;
            }
            fields.emplace_back(trim(line.substr(fpos, comma - fpos)));
            fpos = comma + 1;
        }
        if (fields.size() != 9) {
            throw palette_error(
                "palette: CSV row " + std::to_string(line_no) +
                ": expected 9 columns, got " + std::to_string(fields.size()) +
                " (schema_version 2 adds renders_translucent as 9th column; "
                "see research/translucency_pool_partitioning_proposal.md §3)");
        }

        CsvRow row;
        row.name = fields[0];
        if (row.name.empty()) {
            throw palette_error(
                "palette: CSV row " + std::to_string(line_no) + ": empty name");
        }
        row.opacity      = parse_int_in_range(fields[1], 0, 15,  "opacity",      line_no);
        row.luminance    = parse_int_in_range(fields[2], 0, 15,  "luminance",    line_no);
        row.r            = parse_int_in_range(fields[3], 0, 255, "r",            line_no);
        row.g            = parse_int_in_range(fields[4], 0, 255, "g",            line_no);
        row.b            = parse_int_in_range(fields[5], 0, 255, "b",            line_no);
        const int fc     = parse_int_in_range(fields[6], 0, 1,   "is_full_cube", line_no);
        row.is_full_cube = (fc == 1);
        row.texture_path = fields[7];

        // renders_translucent — accepts {"true", "false", "1", "0"} case-sensitive.
        const std::string& rt = fields[8];
        if (rt == "true" || rt == "1") {
            row.renders_translucent = true;
        } else if (rt == "false" || rt == "0") {
            row.renders_translucent = false;
        } else {
            throw palette_error(
                "palette: CSV row " + std::to_string(line_no) +
                ": invalid renders_translucent value '" + rt +
                "' (expected 'true', 'false', '1', or '0')");
        }

        result.rows.push_back(std::move(row));
    }

    if (result.mc_version.empty()) {
        throw palette_error(
            "palette: CSV '" + csv_path.string() +
            "' missing required '# mc_version:' header");
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// PaletteDB members.
// ---------------------------------------------------------------------------
PaletteDB::PaletteDB(std::vector<BlockPaletteEntry> entries)
    : entries_(std::move(entries)) {
    name_index_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& e = entries_[i];
        auto [it, inserted] = name_index_.emplace(e.name, i);
        if (!inserted) {
            throw palette_error(
                "palette: duplicate block name '" + e.name + "' in catalog");
        }
        (void)it;
    }
}

std::size_t PaletteDB::size() const noexcept { return entries_.size(); }

const BlockPaletteEntry& PaletteDB::get(const std::string& name) const {
    auto it = name_index_.find(name);
    if (it == name_index_.end()) {
        throw palette_error("palette: unknown block name '" + name + "'");
    }
    return entries_[it->second];
}

const BlockPaletteEntry* PaletteDB::find(const std::string& name) const noexcept {
    auto it = name_index_.find(name);
    if (it == name_index_.end()) return nullptr;
    return &entries_[it->second];
}

PaletteDB::const_iterator PaletteDB::begin() const noexcept { return entries_.cbegin(); }
PaletteDB::const_iterator PaletteDB::end()   const noexcept { return entries_.cend();   }

// ---------------------------------------------------------------------------
// Loaders.
// ---------------------------------------------------------------------------
PaletteDB load_palette_csv(const std::filesystem::path& csv_path,
                           const std::filesystem::path& blocks_json_path) {
    // (1) CSV file exists + non-empty.
    if (!std::filesystem::exists(csv_path)) {
        throw palette_error(
            "palette: CSV not found at '" + csv_path.string() +
            "'. To regenerate: run scripts/extract_block_colors.py "
            "against a vanilla MC 1.21.5 client jar (see "
            "research/palette_db_proposal.md §3.1).");
    }
    std::string csv_content = read_text_file(csv_path);
    if (csv_content.empty()) {
        throw palette_error(
            "palette: CSV is empty at '" + csv_path.string() + "'");
    }

    // (2) blocks.json exists.
    if (!std::filesystem::exists(blocks_json_path)) {
        throw palette_error(
            "palette: blocks.json not found at '" + blocks_json_path.string() +
            "' (vendored under third_party/minecraft-data/)");
    }
    const std::string blocks_content = read_text_file(blocks_json_path);

    // (3) version.json sibling of blocks.json.
    const std::filesystem::path version_path =
        blocks_json_path.parent_path() / "version.json";
    if (!std::filesystem::exists(version_path)) {
        throw palette_error(
            "palette: version.json not found next to blocks.json at '" +
            version_path.string() + "'");
    }
    const std::string version_content = read_text_file(version_path);
    const std::string json_version = parse_version_json(version_content);

    // (4) Parse CSV.
    const CsvParseResult csv = parse_csv(csv_content, csv_path);

    // (5) Version-assert (proposal §3.1, §3.11(e)). Both CSV and blocks.json's
    // sibling version.json must declare the project's target MC version.
    constexpr std::string_view kExpectedVersion = "1.21.5";
    if (csv.mc_version != kExpectedVersion) {
        throw palette_error(
            "palette: version mismatch: CSV declares mc_version='" +
            csv.mc_version + "', expected '" + std::string(kExpectedVersion) +
            "'. Re-run scripts/extract_block_colors.py against the matching "
            "MC jar version, or update the project's MC pin in DECISIONS.md.");
    }
    if (json_version != kExpectedVersion) {
        throw palette_error(
            "palette: version mismatch: blocks.json's version.json declares "
            "minecraftVersion='" + json_version + "', expected '" +
            std::string(kExpectedVersion) + "'.");
    }

    // (6) Parse blocks.json's name -> (filterLight, emitLight) map.
    const auto blocks_meta = parse_blocks_json_minimal(blocks_content);
    if (blocks_meta.empty()) {
        throw palette_error(
            "palette: blocks.json parse produced zero blocks at '" +
            blocks_json_path.string() + "' (parser drift?)");
    }

    // (7) Cross-check each CSV row against blocks.json (proposal §3.8).
    // Names not in blocks.json: skip + warn. Light-value mismatches: warn
    // but include. The CSV is authoritative.
    std::vector<BlockPaletteEntry> entries;
    entries.reserve(csv.rows.size());
    for (const CsvRow& row : csv.rows) {
        // Strip "minecraft:" namespace prefix for the lookup (PrismarineJS
        // uses bare names without the namespace).
        std::string bare_name = row.name;
        constexpr std::string_view kNs = "minecraft:";
        if (bare_name.size() >= kNs.size() &&
            bare_name.compare(0, kNs.size(), kNs) == 0) {
            bare_name = bare_name.substr(kNs.size());
        }

        auto meta_it = blocks_meta.find(bare_name);
        if (meta_it == blocks_meta.end()) {
            std::cerr << "palette warning: ignoring CSV row for unknown block '"
                      << row.name << "' (not in blocks.json)\n";
            continue;
        }
        const BlockMeta& meta = meta_it->second;
        if (meta.filter_light != row.opacity) {
            std::cerr << "palette warning: " << row.name
                      << " CSV opacity " << row.opacity
                      << " disagrees with blocks.json filterLight "
                      << meta.filter_light << " (using CSV)\n";
        }
        if (meta.emit_light != row.luminance) {
            std::cerr << "palette warning: " << row.name
                      << " CSV luminance " << row.luminance
                      << " disagrees with blocks.json emitLight "
                      << meta.emit_light << " (using CSV)\n";
        }

        BlockPaletteEntry e;
        e.name = row.name;
        e.avg_color_srgb = RGB{
            static_cast<std::uint8_t>(row.r),
            static_cast<std::uint8_t>(row.g),
            static_cast<std::uint8_t>(row.b),
        };
        e.avg_color_linear = {
            srgb_to_linear(static_cast<float>(row.r) / 255.0f),
            srgb_to_linear(static_cast<float>(row.g) / 255.0f),
            srgb_to_linear(static_cast<float>(row.b) / 255.0f),
        };
        e.opacity             = static_cast<std::uint8_t>(row.opacity);
        e.luminance           = static_cast<std::uint8_t>(row.luminance);
        e.is_full_cube        = row.is_full_cube;
        e.texture_path        = row.texture_path;
        e.renders_translucent = row.renders_translucent;
        entries.push_back(std::move(e));
    }

    if (entries.empty()) {
        throw palette_error(
            "palette: CSV at '" + csv_path.string() +
            "' produced zero entries (all rows filtered or unknown).");
    }

    return PaletteDB(std::move(entries));
}

PaletteDB load_default_palette() {
#ifndef OBJTOMC_PALETTE_DATA_DIR
#error "OBJTOMC_PALETTE_DATA_DIR not defined; set via CMake target_compile_definitions."
#endif
#ifndef OBJTOMC_VENDOR_MINECRAFT_DATA_DIR
#error "OBJTOMC_VENDOR_MINECRAFT_DATA_DIR not defined; set via CMake target_compile_definitions."
#endif
    const std::filesystem::path csv_path =
        std::filesystem::path(OBJTOMC_PALETTE_DATA_DIR) / "block_palette_db.csv";
    const std::filesystem::path blocks_json_path =
        std::filesystem::path(OBJTOMC_VENDOR_MINECRAFT_DATA_DIR) /
        "pc" / "1.21.5" / "blocks.json";
    return load_palette_csv(csv_path, blocks_json_path);
}

}  // namespace objtomc::palette
