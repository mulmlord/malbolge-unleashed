#include "asm/label_map.hpp"

#include <algorithm>
#include <utility>

namespace malm::assembler {

std::string format_label_map(const Layout& layout) {
    std::vector<std::pair<std::string, const LabelInfo*>> labels;
    for (const auto& [name, info] : layout.labels) {
        labels.emplace_back(name, &info);
    }
    std::ranges::sort(labels, [](const auto& x, const auto& y) {
        return x.second->addr.raw() != y.second->addr.raw()
                   ? x.second->addr.raw() < y.second->addr.raw()
                   : x.first < y.first;
    });
    std::string text;
    for (const auto& [name, info] : labels) {
        const PlacedBlock& block = layout.blocks[info->block];
        text += name + ' ' + info->addr.to_string() + ' ' +
                (info->section == Section::Code ? "code" : "data") + ' ' + block.first.to_string() +
                ' ' + block.last.to_string() + '\n';
    }
    return text;
}

namespace {

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

bool is_identifier(std::string_view s) {
    const auto alpha = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    return !s.empty() && s.size() <= 100 && alpha(s[0]) &&
           std::ranges::all_of(s, [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}

} // namespace

core::Result<std::vector<MapEntry>, std::string> parse_label_map(std::string_view text) {
    using R = core::Result<std::vector<MapEntry>, std::string>;
    std::vector<MapEntry> entries;
    std::size_t line_no = 0;
    while (!text.empty()) {
        ++line_no;
        const std::size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        const auto f = fields(line);
        if (f.empty()) {
            continue;
        }
        const std::string where = "line " + std::to_string(line_no) + ": ";
        if (f.size() != 5) {
            return R::err(where + "expected 'name address code|data first last'");
        }
        if (!is_identifier(f[0])) {
            return R::err(where + "'" + std::string(f[0]) + "' is not a label name");
        }
        if (f[2] != "code" && f[2] != "data") {
            return R::err(where + "expected 'code' or 'data', got '" + std::string(f[2]) + "'");
        }
        MapEntry e{std::string(f[0]), {}, f[2] == "code" ? Section::Code : Section::Data, {}, {}};
        for (const auto& [field, out] :
             {std::pair{f[1], &e.addr}, std::pair{f[3], &e.first}, std::pair{f[4], &e.last}}) {
            const auto v = Value::parse(field);
            if (!v) {
                return R::err(where + "'" + std::string(field) + "' is not an address");
            }
            *out = *v;
        }
        if (e.first.head() != e.last.head() || e.addr.head() != e.first.head() ||
            e.first.raw() > e.addr.raw() || e.addr.raw() > e.last.raw()) {
            return R::err(where + "the address of '" + e.name + "' is not inside its block");
        }
        entries.push_back(std::move(e));
    }
    return R::ok(std::move(entries));
}

} // namespace malm::assembler
