// Tests for core logic that the C ABI smokes (Verification/*.py) cannot reach directly: the type-declaration scanner
// behind the redeclaration guard, and module state that must be separate per world.
// Build with -DFU_BUILD_TESTS=ON; the executable returns the number of failed checks.
#include "flecs_unity.h"
#include "../src/Modules.hpp"
#include "../src/TypeGuard.hpp"
#include <cstdio>
#include <string>

namespace flecs_unity {
// The application module is linked by the plugin, not by these tests.
void register_application(flecs::world&) {}
}

using namespace flecs_unity;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++failures;
}

ecs_world_t* world_of(fu_context* ctx) { return static_cast<ecs_world_t*>(fu_world(ctx)); }

bool refused(fu_context* ctx, const std::string& code) {
    return !check_type_redeclaration(world_of(ctx), code.c_str(), 0).problem.empty();
}

void scanner_ignores_text_that_is_not_a_declaration() {
    fu_context* ctx = fu_create(0);
    expect(fu_run_script(ctx, "struct Wings(span: f32)\nbird { Wings: {1} }") == 0, "setup: a struct in use");
    const std::string changed = "struct Wings(span: f32, sweep: f32)";
    expect(refused(ctx, changed), "a changed declaration is refused");
    expect(!refused(ctx, "// " + changed), "a line comment is not a declaration");
    expect(!refused(ctx, "/* " + changed + " */ other { Position: {0,0,0} }"), "a block comment is not a declaration");
    expect(!refused(ctx, "note { Label: {\"" + changed + "\"} }"), "a string is not a declaration");
    expect(!refused(ctx, "note { Label: {`\n" + changed + "\n`} }"), "a multi-line string is not a declaration");
    expect(!refused(ctx, "note { Label: {\"a \\\" quote then " + changed + "\"} }"), "an escaped quote does not end the string");
    expect(!refused(ctx, "holder {\n  " + changed + "\n}"), "a nested declaration names another type");
    expect(refused(ctx, "@tree ChildOf\nprefab Kind { }\n" + changed), "a declaration after other statements is found");
    expect(refused(ctx, "struct Tail(len: f32)\n" + changed), "the second declaration of a script is checked");
    expect(refused(ctx, "other { Position: {0,0,0} }; " + changed), "a declaration after a semicolon is found");
    expect(!check_type_redeclaration(world_of(ctx), "bird { Wings: {2} }", 0).declares_types, "a script without declarations is reported as such");
    expect(check_type_redeclaration(world_of(ctx), "holder { struct Inner(v: f32) }", 0).declares_types, "a nested declaration still marks the script");
    expect(ecs_lookup(world_of(ctx), "__fu_type_check") == 0, "the staging scope is removed");
    fu_destroy(ctx);
}

void templates_compare_their_props() {
    fu_context* ctx = fu_create(0);
    const std::string original = "template Tree {\n  prop height: f32 = 2\n  trunk { Position: {0, height, 0} }\n}";
    expect(fu_run_script(ctx, (original + "\nt1 { Tree: {height: 3} }").c_str()) == 0, "setup: a template in use");
    expect(!refused(ctx, original), "the same template is allowed");
    expect(!refused(ctx, "template Tree {\n  prop height: f32 = 2\n  trunk { Position: {1, height, 1} }\n  leaf { Position: {0,0,0} }\n}"),
        "a changed body with nested scopes is allowed");
    expect(refused(ctx, "template Tree {\n  prop height: f32 = 2\n  prop width: f32 = 1\n  trunk { Position: {0, height, 0} }\n}"),
        "an added prop is refused");
    fu_destroy(ctx);
}

void worlds_keep_separate_module_state() {
    fu_context* first = fu_create(0);
    fu_context* second = fu_create(0);
    const char* bindings = "T { Position: {0,0,0} }\nA { CapBinding: {target: T, enabled: true, revision: 1} }\n"
        "B { CapBinding: {target: T, enabled: true, revision: 1}; (CapDependsOn, A) }";
    expect(fu_run_script(first, bindings) == 0 && fu_run_script(second, bindings) == 0, "setup: bindings in two worlds");
    const uint64_t b1 = fu_lookup(first, "B"), b2 = fu_lookup(second, "B");
    fu_cap_ack(first, b1);
    fu_cap_ack(second, b2);
    expect(fu_set_component_json(first, "A", "CapState", "{\"phase\":1,\"appliedRevision\":1,\"generation\":1,\"message\":\"\"}") == 0,
        "a result is reported in the first world");
    fu_tick(first, 0.02f);
    fu_tick(second, 0.02f);
    expect(fu_cap_has(first, b1, "CapDirty") == 1, "the consumer in the first world is marked");
    expect(fu_cap_has(second, b2, "CapDirty") == 0, "the consumer in the second world is not");
    fu_destroy(first);
    expect(fu_tick(second, 0.02f) == 0 && fu_cap_ready(second, b2) == 0, "the second world still runs after the first is destroyed");
    fu_destroy(second);
}

void json_writes_report_unknown_names() {
    fu_context* ctx = fu_create(0);
    expect(fu_run_script(ctx, "e { Position: {1,2,3} }") == 0, "setup");
    expect(set_component_json(world_of(ctx), "e", "Position", "{\"x\":4,\"y\":5,\"z\":6}") == 0, "a known entity and type are written");
    expect(set_component_json(world_of(ctx), "missing", "Position", "{}") == -1, "an unknown entity is reported");
    expect(set_component_json(world_of(ctx), "e", "Missing", "{}") == -1, "an unknown type is reported");
    expect(set_component_json(world_of(ctx), "e", "Position", "{not json") == -1, "invalid JSON is reported");
    fu_destroy(ctx);
}

struct EventProbe { int runs = 0; ecs_entity_t entity = 0; fu_event fact{}; };

void events_reach_observers_as_parameters() {
    fu_context* ctx = fu_create(0);
    ecs_world_t* world = world_of(ctx);
    ecs_entity_t touched = ecs_new(world);
    ecs_entity_t subject = ecs_new(world), other = ecs_new(world);
    EventProbe probe;
    ecs_observer_desc_t desc = {};
    desc.query.terms[0].id = EcsAny;
    desc.events[0] = touched;
    desc.callback = [](ecs_iter_t* it) {
        auto* seen = static_cast<EventProbe*>(it->ctx);
        ++seen->runs;
        seen->entity = it->count ? it->entities[0] : 0;
        if (it->param) seen->fact = *static_cast<const fu_event*>(it->param);
    };
    desc.ctx = &probe;
    expect(ecs_observer_init(world, &desc) != 0, "setup: an observer for a custom event on any component");
    fu_event fact{touched, subject, other, 1.5f, -2.0f, 3.25f, 0.5f};
    expect(fu_emit_events(ctx, &fact, 1) == 1, "one live event is delivered");
    expect(probe.runs == 1, "the observer ran during the call");
    expect(probe.entity == subject, "the observer saw the event's entity");
    expect(probe.fact.event == touched && probe.fact.entity == subject && probe.fact.other == other, "the parameter carries event, entity and other");
    expect(probe.fact.x == 1.5f && probe.fact.y == -2.0f && probe.fact.z == 3.25f && probe.fact.value == 0.5f, "the parameter carries x, y, z and value");
    fu_destroy(ctx);
}

void ownership_is_exclusive_and_ends_with_the_owner() {
    fu_context* ctx = fu_create(0);
    expect(ctx != nullptr, "setup: a context");
    if (!ctx)
        return;
    ecs_world_t* world = world_of(ctx);
    ecs_entity_t view = ecs_new(world), a = ecs_new(world), b = ecs_new(world);
    uint64_t relation = fu_lookup(ctx, "TransformOwner");
    expect(relation != 0, "TransformOwner is defined");
    expect(fu_claim(ctx, view, relation, a) == 1, "a free relation is claimed");
    expect(fu_claim(ctx, view, relation, a) == 1, "the owner claims again");
    expect(fu_claim(ctx, view, relation, b) == 0, "a second owner is refused");
    expect(fu_release(ctx, view, relation, b) == 0, "a non-owner does not release");
    expect(fu_release(ctx, view, relation, a) == 1, "the owner releases");
    expect(fu_release(ctx, view, relation, a) == 0, "nothing is left to release");
    expect(fu_claim(ctx, view, relation, b) == 1, "setup: B claims");
    ecs_delete(world, b);
    expect(ecs_get_target(world, view, relation, 0) == 0, "deleting the owner ends the ownership");
    expect(fu_claim(ctx, view, relation, b) == -1, "a dead owner is a bad argument");
    expect(fu_claim(ctx, 0, relation, a) == -1 && fu_release(ctx, view, relation, 0) == -1, "zero ids are bad arguments");
    fu_destroy(ctx);
}

} // namespace

int main() {
    scanner_ignores_text_that_is_not_a_declaration();
    templates_compare_their_props();
    worlds_keep_separate_module_state();
    json_writes_report_unknown_names();
    events_reach_observers_as_parameters();
    ownership_is_exclusive_and_ends_with_the_owner();
    std::printf("%s (%d failed)\n", failures ? "FAILED" : "PASSED", failures);
    return failures;
}
