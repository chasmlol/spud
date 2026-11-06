#include "commands.hpp"

#include "core/error.hpp"
#include "core/log.hpp"
#include "core/path.hpp"
#include "core/platform.hpp"
#include "formats/forms.hpp"
#include "formats/forms_value.hpp"
#include "importer/legacy/data_source.hpp"
#include "importer/legacy/strings.hpp"
#include "importer/records/forms_import.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <set>

namespace skytool {

namespace {

using namespace spud;

std::string language_of(const legacy::Install& install) {
    return install_language(install);
}

std::optional<std::filesystem::path> schema_dir(Args args) {
    if (auto d = option(args, "--schema")) {
        return std::filesystem::path(*d);
    }
    return std::nullopt;
}

std::set<forms::Sig> type_filter(Args args) {
    std::set<forms::Sig> out;
    if (auto t = option(args, "--types")) {
        std::string s = *t;
        for (size_t at = 0; at <= s.size();) {
            auto comma = s.find(',', at);
            auto item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (auto sig = forms::parse_sig(item)) {
                out.insert(*sig);
            }
            if (comma == std::string::npos) {
                break;
            }
            at = comma + 1;
        }
    }
    return out;
}

void print_check(const records::Diagnostics& diag, const records::FormsImportResult& result,
                 const std::set<forms::Sig>& types, size_t examples) {
    std::map<records::Diagnostics::Issue, uint64_t> totals;
    for (const auto& [key, e] : diag.issues()) {
        auto [rec, field, issue] = key;
        totals[issue] += e.count;
        if (!types.empty() && !types.contains(rec)) {
            continue;
        }
        std::printf("%s %s  %s x%llu\n", forms::sig_name(rec).c_str(), field ? forms::sig_name(field).c_str() : "----",
                    std::string(records::issue_name(issue)).c_str(), static_cast<unsigned long long>(e.count));
        for (size_t i = 0; i < std::min(examples, e.examples.size()); ++i) {
            std::printf("    %s\n", e.examples[i].c_str());
        }
    }
    uint64_t formid_total = 0;
    for (const auto& [key, p] : result.link.formid_problems) {
        auto [rec, field, problem] = key;
        formid_total += p.count;
        if (!types.empty() && !types.contains(rec)) {
            continue;
        }
        std::printf("%s %s  formid %s x%llu\n", forms::sig_name(rec).c_str(), field ? forms::sig_name(field).c_str() : "----",
                    problem.c_str(), static_cast<unsigned long long>(p.count));
        for (size_t i = 0; i < std::min(examples, p.examples.size()); ++i) {
            std::printf("    %s\n", p.examples[i].c_str());
        }
    }
    std::printf("\nsummary: %llu fields, %llu kept raw, %llu missing strings; %llu FormIDs checked, %llu problems\n",
                static_cast<unsigned long long>(result.fields), static_cast<unsigned long long>(result.raw_fields),
                static_cast<unsigned long long>(result.missing_strings),
                static_cast<unsigned long long>(result.link.formids_checked),
                static_cast<unsigned long long>(formid_total));
    for (const auto& [issue, n] : totals) {
        std::printf("  %-16s %llu\n", std::string(records::issue_name(issue)).c_str(), static_cast<unsigned long long>(n));
    }
}

std::optional<uint32_t> parse_hex(std::string_view s) {
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
    }
    uint32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc() || p != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

std::filesystem::path db_path(Args args) {
    if (auto d = option(args, "--db")) {
        return *d;
    }
    return cache_dir() / "forms.osf";
}

const forms::FormEntry* find_form(const forms::Database& db, std::string_view key) {
    if (auto id = parse_hex(key)) {
        if (auto f = db.find(*id)) {
            return f;
        }
    }
    auto lower = path::to_lower(key);
    for (const auto& f : db.forms()) {
        if (path::to_lower(db.editor_id(f)) == lower) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

int cmd_records(Args args) {
    auto pos = positionals(args, {"--schema", "--types", "--db", "--examples", "--threads", "--out", "--language"});
    if (pos.empty() || has_flag(args, "--help")) {
        std::puts(
            "records check [--schema DIR] [--types A,B] [--examples N]\n"
            "    convert every plugin of the load order with diagnostics, link, check every FormID; report issues\n"
            "records import [--schema DIR] [--force] [--out FILE]\n"
            "    write <cache>/forms.osf, reusing compiled plugin units whose inputs are unchanged\n"
            "records show <FormID|EditorID> [--db FILE]     one form as JSON\n"
            "records list <TYPE> [--db FILE]                FormID, EditorID and plugin of every form of a type\n"
            "records stats [--db FILE]                      forms per record type\n"
            "records dump <TYPE|*> [--db FILE] [--out F]    every form of a type as JSON lines\n"
            "  --language L overrides sLanguage (default: the game's INI)");
        return pos.empty() ? 1 : 0;
    }
    const auto& sub = pos[0];
    if (sub == "check" || sub == "import") {
        auto install = open_install();
        legacy::DataSource data(install);
        auto schema = records::load_schema(schema_dir(args));
        records::FormsImportOptions opt;
        opt.cache_dir = cache_dir();
        opt.language = option(args, "--language").value_or(language_of(install));
        if (auto t = option(args, "--threads")) {
            opt.threads = static_cast<unsigned>(std::atoi(t->c_str()));
        }
        if (auto o = option(args, "--out")) {
            opt.out = *o;
        }
        opt.force = has_flag(args, "--force");
        records::Diagnostics diag;
        if (sub == "check") {
            opt.diagnostics = &diag;
            opt.validate = true;
            opt.keep_units = false;
            if (!opt.out) {
                opt.out = std::filesystem::path(); // link and check only
            }
        }
        auto result = records::import_forms(install, data, schema, opt);
        if (sub == "check") {
            size_t examples = 3;
            if (auto e = option(args, "--examples")) {
                examples = static_cast<size_t>(std::atoi(e->c_str()));
            }
            print_check(diag, result, type_filter(args), examples);
        }
        return 0;
    }
    forms::Database db(db_path(args));
    if (sub == "show" && pos.size() >= 2) {
        auto f = find_form(db, pos[1]);
        if (!f) {
            spud::log::error("no form {}", pos[1]);
            return 1;
        }
        std::puts(forms::form_json(db, *f).c_str());
        return 0;
    }
    if (sub == "list" && pos.size() >= 2) {
        auto type = forms::parse_sig(pos[1]);
        if (!type) {
            return 1;
        }
        for (auto f : db.of_type(*type)) {
            std::printf("%08X  %-40s  %s\n", f->form_id, std::string(db.editor_id(*f)).c_str(),
                        std::string(db.plugin_name(f->plugin)).c_str());
        }
        return 0;
    }
    if (sub == "stats") {
        std::map<std::string, std::pair<size_t, size_t>> counts;
        for (const auto& f : db.forms()) {
            auto& c = counts[forms::sig_name(f.type)];
            ++c.first;
            c.second += (f.flags & forms::form_deleted) != 0;
        }
        std::printf("%zu plugins, %zu forms, language %s\n", db.plugins().size(), db.forms().size(),
                    std::string(db.language()).c_str());
        for (const auto& [t, c] : counts) {
            std::printf("  %s %8zu  (%zu deleted)\n", t.c_str(), c.first, c.second);
        }
        return 0;
    }
    if (sub == "gamedump") {
        return records_gamedump(db, args);
    }
    if (sub == "dump" && pos.size() >= 2) {
        std::optional<forms::Sig> type;
        if (pos[1] != "*") {
            type = forms::parse_sig(pos[1]);
        }
        FILE* out = stdout;
        if (auto o = option(args, "--out")) {
            out = std::fopen(o->c_str(), "wb");
            if (!out) {
                throw Error("cannot write " + *o);
            }
        }
        for (const auto& f : db.forms()) {
            if (!type || f.type == *type) {
                auto line = forms::form_json(db, f);
                std::fputs(line.c_str(), out);
                std::fputc('\n', out);
            }
        }
        if (out != stdout) {
            std::fclose(out);
        }
        return 0;
    }
    std::puts("unknown records command; see 'skytool records --help'");
    return 1;
}

int cmd_strings(Args args) {
    auto pos = positionals(args, {"--language", "--table", "--id"});
    if (pos.empty() || has_flag(args, "--help")) {
        std::puts("strings <plugin> [--table strings|dl|il] [--id HEX] [--language L]\n"
                  "    the plugin's string tables as the game finds them (path, origin, count); with --id one string,\n"
                  "    else every string of --table as 'ID<TAB>text' (UTF-8)");
        return pos.empty() ? 1 : 0;
    }
    auto install = open_install();
    legacy::DataSource data(install);
    auto language = option(args, "--language").value_or(language_of(install));
    legacy::Localization loc(data, language);
    const auto& t = loc.tables(pos[0]);
    for (int k = 0; k < 3; ++k) {
        std::fprintf(stderr, "%s: %s (%s, %zu strings)\n", t.paths[k].c_str(), t.present[k] ? "found" : "missing",
                     t.origins[k].c_str(), t.tables[k].size());
    }
    auto table = option(args, "--table").value_or("strings");
    int k = table == "dl" ? 1 : table == "il" ? 2 : 0;
    if (auto id = option(args, "--id")) {
        std::string_view text;
        auto r = loc.lookup(pos[0], static_cast<legacy::StringTableKind>(k), *parse_hex(*id), text);
        std::printf("%s [%s]\n", legacy::to_utf8(text, language).c_str(),
                    r == legacy::LookupResult::found ? "found" : r == legacy::LookupResult::failed ? "failed" : "unchanged");
        return 0;
    }
    t.tables[k].for_each([&](uint32_t id, std::string_view text) {
        std::printf("%08X\t%s\n", id, legacy::to_utf8(text, language).c_str());
    });
    return 0;
}

} // namespace skytool
