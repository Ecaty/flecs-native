#include "Modules.hpp"
#include <algorithm>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/xml_parsing.h>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

// Behaviour trees as data: NativeTree holds BehaviorTree.CPP v4 XML, NativeTreeResult reports status, revision and
// tick count. A tree is compiled when its definition is set (not compared every frame) and ticked in OnUpdate while
// it is enabled and unfinished. A tree that is only waiting for simulation time is not ticked until that time.
// Leaves: FlecsSet, FlecsQuery, SimWait (simulation time).
namespace flecs_unity {
namespace {

struct NativeTree : Label { std::int32_t revision = 1; bool enabled = true, loop = false; };
struct NativeTreeResult : Label { std::int32_t status = 0, revision = 0; std::int64_t ticks = 0; };

struct SimWait : BT::StatefulActionNode {
    double& now;
    double end = 0;
    SimWait(const std::string& name, const BT::NodeConfig& config, double& time) : StatefulActionNode(name, config), now(time) {}
    static BT::PortsList providedPorts() { return {BT::InputPort<double>("seconds", 0.0, "Simulation seconds")}; }
    BT::NodeStatus onStart() override {
        const double seconds = getInput<double>("seconds").value();
        if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600) throw BT::RuntimeError("Invalid simulation wait");
        end = now + seconds;
        return seconds == 0 ? BT::NodeStatus::SUCCESS : BT::NodeStatus::RUNNING;
    }
    BT::NodeStatus onRunning() override { return now >= end ? BT::NodeStatus::SUCCESS : BT::NodeStatus::RUNNING; }
    void onHalted() override {}
};

struct Entry {
    std::unique_ptr<BT::Tree> tree;
    std::string source;
    std::int32_t revision = 0;
    bool enabled = false, loop = false, finished = false;
    bool changed = true; // the definition was set since the tree was compiled
    double wake = 0;     // simulation time before which a waiting tree needs no tick
    NativeTreeResult result;
};

} // namespace

struct TreeModule::State {
    ecs_world_t* world = nullptr;
    double now = 0;
    BT::BehaviorTreeFactory factory;
    std::unordered_map<flecs::entity_t, Entry> entries;

    explicit State(ecs_world_t* w) : world(w) {
        factory.registerNodeType<SimWait>("SimWait", std::ref(now));
        factory.registerSimpleAction("FlecsSet", [this](BT::TreeNode& node) {
            const auto entity = node.getInput<std::string>("entity").value();
            const auto component = node.getInput<std::string>("component").value();
            // A JSON object or array is taken verbatim; BT.CPP would otherwise treat braces as a blackboard reference.
            const auto& raw = static_cast<const BT::TreeNode&>(node).config().input_ports.at("value");
            const auto value = (!raw.empty() && (raw[0] == '{' || raw[0] == '[')) ? raw : node.getInput<std::string>("value").value();
            return set_component_json(world, entity.c_str(), component.c_str(), value.c_str()) == 0
                ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
        }, {BT::InputPort<std::string>("entity"), BT::InputPort<std::string>("component"), BT::InputPort<std::string>("value")});
        factory.registerSimpleCondition("FlecsQuery", [this](BT::TreeNode& node) {
            const auto expression = node.getInput<std::string>("expression").value();
            ecs_query_desc_t desc{};
            desc.expr = expression.c_str();
            ecs_query_t* query = ecs_query_init(world, &desc);
            if (!query) return BT::NodeStatus::FAILURE;
            ecs_iter_t it = ecs_query_iter(world, query);
            const bool found = ecs_query_next(&it);
            if (found) ecs_iter_fini(&it);
            ecs_query_fini(query);
            return found ? BT::NodeStatus::SUCCESS : BT::NodeStatus::FAILURE;
        }, {BT::InputPort<std::string>("expression")});
    }

    // Applies a definition that was set. The tree is rebuilt only when its XML or revision changed, so setting the
    // same definition again does not restart it. Throws with the reason when the XML is not usable.
    void compile(Entry& entry, const NativeTree& definition) {
        entry.enabled = definition.enabled;
        entry.loop = definition.loop;
        std::string xml = definition.text ? definition.text : "";
        if (entry.source == xml && entry.revision == definition.revision && (entry.tree || entry.result.status == -1)) return;
        entry.source = std::move(xml);
        const std::string& xml_text = entry.source;
        if (entry.tree) entry.tree->haltTree();
        entry.tree.reset();
        entry.finished = false;
        entry.wake = 0;
        entry.revision = definition.revision;
        ecs_os_free(entry.result.text);
        entry.result.text = nullptr;
        if (xml_text.empty() || xml_text.size() > 262144) throw std::runtime_error("Tree XML is empty or too large");
        BT::XMLParser parser(factory);
        parser.loadFromText(xml_text, false);
        entry.tree = std::make_unique<BT::Tree>(parser.instantiateTree(BT::Blackboard::create()));
    }

    // A tree can rest until its earliest SimWait ends when nothing else could change its outcome before then: every
    // running node is a SimWait or one of these plain structural nodes. Reactive nodes re-check conditions on every
    // tick and timer nodes follow real time, so any other running node keeps the tree ticking every frame.
    static bool passive(const std::string& kind) {
        static const char* const kinds[] = {"Sequence", "SequenceWithMemory", "Fallback", "SubTree", "Repeat",
            "RetryUntilSuccessful", "ForceSuccess", "ForceFailure", "Inverter", "KeepRunningUntilFailure"};
        return std::any_of(std::begin(kinds), std::end(kinds), [&](const char* name) { return kind == name; });
    }
    double wake_time(const BT::Tree& tree) const {
        double earliest = std::numeric_limits<double>::infinity();
        bool can_rest = true;
        tree.applyVisitor([&](const BT::TreeNode* node) {
            if (node->status() != BT::NodeStatus::RUNNING) return;
            if (const auto* wait = dynamic_cast<const SimWait*>(node)) earliest = std::min(earliest, wait->end);
            else if (!passive(node->registrationName())) can_rest = false;
        });
        return can_rest && std::isfinite(earliest) ? earliest : now;
    }

    void tick(double delta) {
        now += delta;
        flecs::world view(world);
        // Leaves may create or delete trees: work on a snapshot of the ids.
        std::vector<flecs::entity_t> ids;
        ids.reserve(entries.size());
        for (const auto& pair : entries) ids.push_back(pair.first);
        for (const flecs::entity_t id : ids) {
            const auto found = entries.find(id);
            if (found == entries.end() || !ecs_is_alive(world, id)) continue;
            Entry& entry = found->second;
            const bool was_changed = entry.changed;
            const auto before_status = entry.result.status;
            const auto before_ticks = entry.result.ticks;
            try {
                if (entry.changed) {
                    entry.changed = false;
                    const NativeTree* definition = view.entity(id).try_get<NativeTree>();
                    if (!definition) continue;
                    compile(entry, *definition);
                }
                if (!entry.enabled) {
                    if (entry.tree) entry.tree->haltTree();
                    entry.result.status = 0;
                    entry.finished = false;
                } else if (entry.tree && (!entry.finished || entry.loop)) {
                    if (entry.wake > now) continue;
                    entry.result.status = static_cast<std::int32_t>(entry.tree->tickExactlyOnce());
                    entry.wake = entry.result.status == static_cast<std::int32_t>(BT::NodeStatus::RUNNING) ? wake_time(*entry.tree) : 0;
                    entry.finished = entry.result.status == static_cast<std::int32_t>(BT::NodeStatus::SUCCESS) ||
                        entry.result.status == static_cast<std::int32_t>(BT::NodeStatus::FAILURE);
                    ++entry.result.ticks;
                }
                entry.result.revision = entry.revision;
            } catch (const std::exception& error) {
                if (entry.tree) entry.tree->haltTree();
                ecs_os_free(entry.result.text);
                entry.result.text = ecs_os_strdup(error.what());
                entry.result.status = -1;
                entry.result.revision = entry.revision;
            }
            // A leaf may have deleted this tree's entity or removed its definition.
            const auto still = entries.find(id);
            if (still == entries.end() || !ecs_is_alive(world, id)) continue;
            if (was_changed || before_status != still->second.result.status || before_ticks != still->second.result.ticks)
                view.entity(id).set<NativeTreeResult>(still->second.result);
        }
    }
};

TreeModule::TreeModule(flecs::world& world) : state(std::make_unique<State>(world.c_ptr())) {
    const ecs_entity_t scope = ecs_set_scope(world, 0);
    world.component<NativeTree>("NativeTree")
        .member(flecs::String, "xml")
        .member(&NativeTree::revision, "revision")
        .member(&NativeTree::enabled, "enabled")
        .member(&NativeTree::loop, "loop");
    world.component<NativeTreeResult>("NativeTreeResult")
        .member(flecs::String, "error")
        .member(&NativeTreeResult::status, "status")
        .member(&NativeTreeResult::revision, "revision")
        .member(&NativeTreeResult::ticks, "ticks");
    ecs_set_scope(world, scope);
    world.module<TreeModule>("::core::tree");
    State* trees = state.get();

    world.observer<const NativeTree>("TreeDefined")
        .event(flecs::OnSet)
        .each([trees](flecs::entity entity, const NativeTree&) { trees->entries[entity.id()].changed = true; });
    world.observer<const NativeTree>("TreeRemoved")
        .event(flecs::OnRemove)
        .each([trees](flecs::entity entity, const NativeTree&) {
            const auto found = trees->entries.find(entity.id());
            if (found == trees->entries.end()) return;
            if (found->second.tree) found->second.tree->haltTree();
            trees->entries.erase(found);
        });
    // Immediate: leaves set components and run queries on the real world, as they did when trees ran after progress.
    world.system("TickTrees")
        .kind(flecs::OnUpdate)
        .immediate()
        .run([trees](flecs::iter& it) { trees->tick(it.delta_time()); });
}

TreeModule::~TreeModule() {
    if (!state) return;
    for (auto& pair : state->entries)
        if (pair.second.tree) pair.second.tree->haltTree();
}
TreeModule::TreeModule(TreeModule&&) noexcept = default;
TreeModule& TreeModule::operator=(TreeModule&&) noexcept = default;

void import_modules(flecs::world& world) {
    world.import<CapabilityModule>();
    world.import<GeometryModule>();
    world.import<TreeModule>();
}

} // namespace flecs_unity
