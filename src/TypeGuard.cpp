#include "TypeGuard.hpp"
#include <atomic>
#include <cctype>
#include <cstring>
#include <vector>

namespace flecs_unity {
namespace {

struct Declaration {
    std::string name; // as written, relative to the script's module
    std::string text; // the whole statement
};
struct Scan {
    std::string module;
    std::vector<std::string> usings;
    std::vector<Declaration> declarations;
    bool nested = false; // a declaration inside another scope: not checked here, the fault hook still reports it
};

bool identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

// Index just past the comment or string that starts at `i`, or `i` when there is none.
std::size_t skip_inert(const std::string& s, std::size_t i) {
    if (s.compare(i, 2, "//") == 0) {
        while (i < s.size() && s[i] != '\n') ++i;
        return i;
    }
    if (s.compare(i, 2, "/*") == 0) {
        const auto end = s.find("*/", i + 2);
        return end == std::string::npos ? s.size() : end + 2;
    }
    if (s[i] == '"' || s[i] == '`') {
        const char quote = s[i++];
        while (i < s.size() && s[i] != quote) i += (s[i] == '\\' && quote == '"') ? 2 : 1;
        return i < s.size() ? i + 1 : s.size();
    }
    return i;
}

// Index just past the bracket group that opens at `i` ('(' or '{').
std::size_t skip_group(const std::string& s, std::size_t i) {
    int depth = 0;
    while (i < s.size()) {
        const auto inert = skip_inert(s, i);
        if (inert != i) { i = inert; continue; }
        if (s[i] == '(' || s[i] == '{') ++depth;
        else if (s[i] == ')' || s[i] == '}') { if (--depth == 0) return i + 1; }
        ++i;
    }
    return s.size();
}

std::size_t skip_spaces(const std::string& s, std::size_t i, bool newlines) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || (newlines && s[i] == '\n'))) ++i;
    return i;
}

std::string line_from(const std::string& s, std::size_t i) {
    auto end = s.find('\n', i);
    if (end == std::string::npos) end = s.size();
    while (end > i && (s[end - 1] == '\r' || s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(i, end - i);
}

// Finds the type declarations at the top level of a script, plus the module and using statements they depend on.
Scan scan(const std::string& s) {
    Scan result;
    int depth = 0;
    bool statement_start = true;
    for (std::size_t i = 0; i < s.size();) {
        const auto inert = skip_inert(s, i);
        if (inert != i) { i = inert; statement_start = false; continue; }
        const char c = s[i];
        if (c == '\n' || c == ';') { statement_start = true; ++i; continue; }
        if (c == '{' || c == '(') { ++depth; statement_start = c == '{'; ++i; continue; }
        if (c == '}' || c == ')') { --depth; statement_start = c == '}'; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '@') { while (i < s.size() && s[i] != '\n') ++i; continue; } // annotation line
        if (!statement_start || !identifier_char(c)) { statement_start = false; ++i; continue; }
        auto end = i;
        while (end < s.size() && identifier_char(s[end])) ++end;
        const std::string word = s.substr(i, end - i);
        statement_start = false;
        if (word == "module" && depth == 0) {
            result.module = line_from(s, skip_spaces(s, end, false));
        } else if (word == "using" && depth == 0) {
            result.usings.push_back(line_from(s, i));
        } else if (word == "struct" || word == "enum" || word == "template") {
            auto at = skip_spaces(s, end, false);
            auto name_end = at;
            while (name_end < s.size() && identifier_char(s[name_end])) ++name_end;
            if (name_end == at) { i = end; continue; }
            auto close = skip_spaces(s, name_end, false);
            if (close < s.size() && s[close] == '(') close = skip_spaces(s, skip_group(s, close), false);
            if (close < s.size() && s[close] == '{') close = skip_group(s, close);
            if (depth == 0) result.declarations.push_back({s.substr(at, name_end - at), s.substr(i, close - i)});
            else result.nested = true;
            i = close;
            continue;
        }
        i = end;
    }
    return result;
}

const char* Staging = "__fu_type_check";

std::string path_of(ecs_world_t* world, ecs_entity_t entity) {
    char* path = ecs_get_path_w_sep(world, 0, entity, ".", nullptr);
    std::string result = path ? path : "";
    ecs_os_free(path);
    const std::string prefix = std::string(Staging) + ".";
    return result.compare(0, prefix.size(), prefix) == 0 ? result.substr(prefix.size()) : result;
}

// Empty when both types store their values the same way; otherwise what differs.
std::string layout_difference(ecs_world_t* world, ecs_entity_t live, ecs_entity_t staged) {
    const auto* a = ecs_get(world, live, EcsComponent);
    const auto* b = ecs_get(world, staged, EcsComponent);
    if (!a && !b) return "";
    if (!a || !b) return "it changes between a tag and a component";
    if (a->size != b->size || a->alignment != b->alignment)
        return "its size changes from " + std::to_string(a->size) + " to " + std::to_string(b->size) + " bytes";
    const auto* sa = ecs_get(world, live, EcsStruct);
    const auto* sb = ecs_get(world, staged, EcsStruct);
    if (!sa && !sb) return "";
    if (!sa || !sb) return "it changes kind";
    const int count = ecs_vec_count(&sa->members);
    if (count != ecs_vec_count(&sb->members)) return "its member count changes";
    const auto* ma = static_cast<const ecs_member_t*>(ecs_vec_first(&sa->members));
    const auto* mb = static_cast<const ecs_member_t*>(ecs_vec_first(&sb->members));
    for (int i = 0; i < count; ++i) {
        if (std::strcmp(ma[i].name, mb[i].name) != 0 || ma[i].offset != mb[i].offset || ma[i].count != mb[i].count ||
            (ma[i].type != mb[i].type && path_of(world, ma[i].type) != path_of(world, mb[i].type)))
            return std::string("member '") + ma[i].name + "' changes";
    }
    return "";
}

std::atomic<std::uint64_t> faults{0};
void (*previous_log)(int32_t, const char*, int32_t, const char*) = nullptr;

void log_hook(int32_t level, const char* file, int32_t line, const char* message) {
    if (level < 0 && message && std::strstr(message, "component is already in use")) ++faults;
    if (previous_log) previous_log(level, file, line, message);
}

} // namespace

TypeCheck check_type_redeclaration(ecs_world_t* world, const char* code, ecs_entity_t owner) {
    TypeCheck check;
    const Scan found = scan(code);
    check.declares_types = !found.declarations.empty() || found.nested;
    if (found.declarations.empty()) return check;

    // Only types that entities already use, and that a reload will not delete first, can be corrupted.
    // A type registered by compiled code is always protected: native systems read it with its compiled size.
    struct Candidate { const Declaration* declaration; ecs_entity_t live; };
    std::vector<Candidate> candidates;
    for (const auto& declaration : found.declarations) {
        const std::string path = found.module.empty() ? declaration.name : found.module + "." + declaration.name;
        const ecs_entity_t live = ecs_lookup(world, path.c_str());
        if (!live) continue;
        const char* symbol = ecs_get_symbol(world, live);
        const bool compiled = symbol && symbol[0];
        if (!compiled && !ecs_id_in_use(world, live)) continue;
        if (owner && ecs_has_pair(world, live, ecs_id(EcsScript), owner)) continue;
        candidates.push_back({&declaration, live});
    }
    if (candidates.empty()) return check;

    // Let Flecs build the new declarations under a throwaway scope and compare the layouts it computes.
    if (const ecs_entity_t stale = ecs_lookup(world, Staging)) ecs_delete(world, stale);
    std::string text;
    for (const auto& line : found.usings) text += line + "\n";
    if (!found.module.empty()) text += "using " + found.module + "\n";
    text += std::string(Staging) + " {\n";
    for (const auto& declaration : found.declarations) text += declaration.text + "\n";
    text += "}\n";
    ecs_script_eval_result_t result{};
    ecs_script_run(world, "type check", text.c_str(), &result);
    const std::string staging_error = result.error ? result.error : "";
    if (result.error) ecs_os_free(result.error);

    for (const auto& candidate : candidates) {
        const std::string name = candidate.declaration->name;
        const ecs_entity_t staged = ecs_lookup(world, (std::string(Staging) + "." + name).c_str());
        const std::string difference = staged ? layout_difference(world, candidate.live, staged)
            : "its new declaration could not be checked" + (staging_error.empty() ? "" : " (" + staging_error + ")");
        if (difference.empty()) continue;
        check.problem = "Type '" + name + "' is in use or built in and cannot be redeclared: " + difference +
            ". Declare a new type name instead.";
        break;
    }
    if (const ecs_entity_t staging = ecs_lookup(world, Staging)) ecs_delete(world, staging);
    return check;
}

void install_layout_fault_hook() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_get_api();
    previous_log = api.log_;
    api.log_ = log_hook;
    ecs_os_set_api(&api);
}

std::uint64_t layout_faults() { return faults.load(); }

} // namespace flecs_unity
