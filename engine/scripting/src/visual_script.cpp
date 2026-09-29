// visual_script.cpp — node library, graph editing, JSON and the graph -> Lua compiler (ADR-0018).
#include "aether/scripting/visual_script.h"

#include "aether/core/paths_ext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <functional>
#include <set>

namespace aether::scripting {

using json = nlohmann::json;

// =================================================================================================
// node library
// =================================================================================================
const char* pin_type_name(PinType t) noexcept {
    switch (t) {
    case PinType::Exec: return "exec";
    case PinType::Bool: return "bool";
    case PinType::Number: return "number";
    case PinType::Vector: return "vector";
    case PinType::String: return "string";
    case PinType::Entity: return "entity";
    case PinType::Any: return "any";
    }
    return "exec";
}

namespace {
PinType pin_type_from_name(std::string_view s) {
    for (PinType t : { PinType::Exec, PinType::Bool, PinType::Number, PinType::Vector, PinType::String,
                       PinType::Entity, PinType::Any }) {
        if (s == pin_type_name(t)) {
            return t;
        }
    }
    return PinType::Number;
}

PinDef exec(const char* name) { return PinDef{ name, PinType::Exec, 0.0 }; }
PinDef num(const char* name, f64 v = 0.0) { return PinDef{ name, PinType::Number, v }; }
PinDef boolean(const char* name, bool v = false) { return PinDef{ name, PinType::Bool, v }; }
PinDef vec(const char* name, Vec3 v = Vec3(0.0f)) { return PinDef{ name, PinType::Vector, v }; }
PinDef str(const char* name, const char* v = "") { return PinDef{ name, PinType::String, std::string(v) }; }
PinDef ent(const char* name) { return PinDef{ name, PinType::Entity, 0.0 }; }
PinDef any(const char* name) { return PinDef{ name, PinType::Any, std::string() }; }

std::vector<NodeDef> build_library() {
    std::vector<NodeDef> l;
    auto add = [&](const char* type, const char* title, const char* cat, const char* help, std::vector<PinDef> in,
                   std::vector<PinDef> out, bool var = false) {
        l.push_back(NodeDef{ type, title, cat, help, std::move(in), std::move(out), var });
    };
    // ---- events ----
    add("event.start", "On Start", "Events", "Runs once when the entity starts.", {}, { exec("then") });
    add("event.update", "On Update", "Events", "Runs every frame (use Delta Time for smooth motion).", {},
        { exec("then") });
    add("event.key_pressed", "On Key Pressed", "Events", "Runs in the frame a key goes down.", { str("key", "space") },
        { exec("then") });
    add("event.key_released", "On Key Released", "Events", "Runs in the frame a key goes up.", { str("key", "space") },
        { exec("then") });
    add("event.custom", "On Event", "Events", "Runs when any script emits this event name.", { str("name", "my_event") },
        { exec("then") });
    add("event.timer", "Every N Seconds", "Events", "Runs repeatedly on a timer.", { num("seconds", 1.0) },
        { exec("then") });
    // ---- flow ----
    add("flow.branch", "Branch", "Flow", "Continues on True or False.", { exec("in"), boolean("condition", true) },
        { exec("true"), exec("false") });
    add("flow.sequence", "Sequence", "Flow", "Runs the outputs one after another.", { exec("in") },
        { exec("then 1"), exec("then 2"), exec("then 3") });
    add("flow.delay", "Delay", "Flow", "Continues after a number of seconds.", { exec("in"), num("seconds", 1.0) },
        { exec("then") });
    // ---- actions ----
    add("action.print", "Print", "Actions", "Writes a value to the log / console.", { exec("in"), any("value") },
        { exec("then") });
    add("action.set_position", "Set Position", "Actions", "Sets the local position.",
        { exec("in"), ent("target"), vec("position") }, { exec("then") });
    add("action.set_world_position", "Set World Position", "Actions", "Sets the position in world space.",
        { exec("in"), ent("target"), vec("position") }, { exec("then") });
    add("action.translate", "Move By", "Actions", "Adds an offset to the local position.",
        { exec("in"), ent("target"), vec("offset", Vec3(0.0f, 1.0f, 0.0f)) }, { exec("then") });
    add("action.rotate", "Rotate", "Actions", "Rotates around an axis (degrees) in the local frame.",
        { exec("in"), ent("target"), vec("axis", Vec3(0.0f, 1.0f, 0.0f)), num("degrees", 90.0) }, { exec("then") });
    add("action.move_over_time", "Move By Over Time", "Actions",
        "Smoothly moves by an offset over some seconds; 'finished' runs at the end ('then' continues at once).",
        { exec("in"), ent("target"), vec("offset", Vec3(0.0f, 1.0f, 0.0f)), num("seconds", 0.5), boolean("ease", true) },
        { exec("then"), exec("finished") });
    add("action.move_to_over_time", "Move To Over Time", "Actions",
        "Smoothly moves to a local position over some seconds; 'finished' runs at the end.",
        { exec("in"), ent("target"), vec("position"), num("seconds", 0.5), boolean("ease", true) },
        { exec("then"), exec("finished") });
    add("action.rotate_over_time", "Rotate Over Time", "Actions",
        "Smoothly rotates around an axis (degrees) over some seconds; 'finished' runs at the end.",
        { exec("in"), ent("target"), vec("axis", Vec3(0.0f, 1.0f, 0.0f)), num("degrees", 90.0), num("seconds", 1.0),
          boolean("ease", true) },
        { exec("then"), exec("finished") });
    add("action.set_scale", "Set Scale", "Actions", "Sets the local scale.",
        { exec("in"), ent("target"), vec("scale", Vec3(1.0f)) }, { exec("then") });
    add("action.set_visible", "Set Visible", "Actions", "Shows or hides the entity and its children.",
        { exec("in"), ent("target"), boolean("visible", true) }, { exec("then") });
    add("action.look_at", "Look At", "Actions", "Points the entity's forward (-Z) at a position.",
        { exec("in"), ent("target"), vec("position") }, { exec("then") });
    add("action.spawn", "Spawn Entity", "Actions", "Creates an empty entity (add components from scripts or prefabs).",
        { exec("in"), str("name", "Spawned"), vec("position") }, { exec("then"), ent("entity") });
    add("action.destroy", "Destroy Entity", "Actions", "Destroys the entity and its children.", { exec("in"), ent("target") },
        { exec("then") });
    add("action.emit_event", "Emit Event", "Actions", "Triggers On Event nodes (in every script) with this name.",
        { exec("in"), str("name", "my_event") }, { exec("then") });
    add("action.set_variable", "Set Variable", "Variables", "Stores a value in a graph variable.",
        { exec("in"), any("value") }, { exec("then") }, true);
    // ---- data ----
    add("data.get_variable", "Get Variable", "Variables", "Reads a graph variable.", {}, { any("value") }, true);
    add("data.self", "Self", "Entity", "The entity this script is attached to.", {}, { ent("entity") });
    add("data.find_entity", "Find Entity", "Entity", "The first entity with this name (nil if none).",
        { str("name", "Entity") }, { ent("entity") });
    add("data.get_position", "Get Position", "Entity", "Local position.", { ent("target") }, { vec("position") });
    add("data.get_world_position", "Get World Position", "Entity", "World position.", { ent("target") },
        { vec("position") });
    add("data.forward", "Get Forward", "Entity", "World-space forward (-Z) direction.", { ent("target") },
        { vec("direction") });
    add("data.delta_time", "Delta Time", "Time", "Seconds since the last frame.", {}, { num("seconds") });
    add("data.time", "Time", "Time", "Seconds since the scripts started.", {}, { num("seconds") });
    add("input.key_down", "Is Key Down", "Input", "True while the key is held.", { str("key", "space") },
        { boolean("down") });
    add("input.axis", "Key Axis", "Input", "+1 / -1 / 0 from two keys (e.g. d / a).",
        { str("positive", "d"), str("negative", "a") }, { num("value") });
    add("math.add", "Add", "Math", "a + b", { num("a"), num("b") }, { num("result") });
    add("math.subtract", "Subtract", "Math", "a - b", { num("a"), num("b") }, { num("result") });
    add("math.multiply", "Multiply", "Math", "a x b", { num("a", 1.0), num("b", 1.0) }, { num("result") });
    add("math.divide", "Divide", "Math", "a / b", { num("a", 1.0), num("b", 1.0) }, { num("result") });
    add("math.sin", "Sine", "Math", "sin(radians)", { num("radians") }, { num("result") });
    add("math.cos", "Cosine", "Math", "cos(radians)", { num("radians") }, { num("result") });
    add("math.abs", "Absolute", "Math", "|value|", { num("value") }, { num("result") });
    add("math.min", "Min", "Math", "The smaller of a and b.", { num("a"), num("b") }, { num("result") });
    add("math.max", "Max", "Math", "The larger of a and b.", { num("a"), num("b") }, { num("result") });
    add("math.clamp", "Clamp", "Math", "value limited to min..max.", { num("value"), num("min"), num("max", 1.0) },
        { num("result") });
    add("math.lerp", "Lerp", "Math", "a + (b - a) x t", { num("a"), num("b", 1.0), num("t", 0.5) }, { num("result") });
    add("math.random", "Random Range", "Math", "A random number between min and max.", { num("min"), num("max", 1.0) },
        { num("result") });
    add("compare.greater", "Greater", "Logic", "a > b", { num("a"), num("b") }, { boolean("result") });
    add("compare.less", "Less", "Logic", "a < b", { num("a"), num("b") }, { boolean("result") });
    add("compare.equal", "Equal", "Logic", "a == b", { num("a"), num("b") }, { boolean("result") });
    add("logic.and", "And", "Logic", "a and b", { boolean("a", true), boolean("b", true) }, { boolean("result") });
    add("logic.or", "Or", "Logic", "a or b", { boolean("a"), boolean("b") }, { boolean("result") });
    add("logic.not", "Not", "Logic", "not a", { boolean("a") }, { boolean("result") });
    add("vector.make", "Make Vector", "Vector", "A vector from x, y, z.", { num("x"), num("y"), num("z") },
        { vec("vector") });
    add("vector.break", "Break Vector", "Vector", "The x, y, z of a vector.", { vec("vector") },
        { num("x"), num("y"), num("z") });
    add("vector.add", "Add Vectors", "Vector", "a + b", { vec("a"), vec("b") }, { vec("result") });
    add("vector.subtract", "Subtract Vectors", "Vector", "a - b", { vec("a"), vec("b") }, { vec("result") });
    add("vector.scale", "Scale Vector", "Vector", "vector x number", { vec("vector", Vec3(1.0f)), num("scale", 1.0) },
        { vec("result") });
    add("vector.length", "Vector Length", "Vector", "Length of a vector.", { vec("vector") }, { num("length") });
    add("vector.normalize", "Normalize", "Vector", "Unit-length direction.", { vec("vector", Vec3(0.0f, 1.0f, 0.0f)) },
        { vec("result") });
    add("vector.distance", "Distance", "Vector", "Distance between two points.", { vec("a"), vec("b") },
        { num("distance") });
    add("text.join", "Join Text", "Text", "a followed by b (any values).", { any("a"), any("b") }, { str("text") });
    return l;
}

const PinDef* find_pin(const std::vector<PinDef>& pins, std::string_view name) {
    for (const PinDef& p : pins) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

bool valid_identifier(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}
} // namespace

const std::vector<NodeDef>& node_library() {
    static const std::vector<NodeDef> lib = build_library();
    return lib;
}

const NodeDef* find_node_def(std::string_view type) {
    for (const NodeDef& d : node_library()) {
        if (d.type == type) {
            return &d;
        }
    }
    return nullptr;
}

GraphValue default_value(PinType t) {
    switch (t) {
    case PinType::Bool: return false;
    case PinType::Vector: return Vec3(0.0f);
    case PinType::String:
    case PinType::Any: return std::string();
    default: return 0.0;
    }
}

bool pins_compatible(PinType from, PinType to) noexcept {
    if (from == PinType::Exec || to == PinType::Exec) {
        return from == to;
    }
    return from == to || to == PinType::Any || from == PinType::Any;
}

// =================================================================================================
// graph editing
// =================================================================================================
GraphNode* VisualGraph::find(u32 id) {
    for (GraphNode& n : nodes) {
        if (n.id == id) {
            return &n;
        }
    }
    return nullptr;
}

const GraphNode* VisualGraph::find(u32 id) const { return const_cast<VisualGraph*>(this)->find(id); }

u32 VisualGraph::next_id() const {
    u32 m = 0;
    for (const GraphNode& n : nodes) {
        m = std::max(m, n.id);
    }
    return m + 1;
}

u32 VisualGraph::add_node(std::string_view type, Vec2 position) {
    const NodeDef* def = find_node_def(type);
    if (def == nullptr) {
        return 0;
    }
    GraphNode n;
    n.id = next_id();
    n.type = std::string(type);
    n.position = position;
    for (const PinDef& p : def->inputs) {
        if (p.type != PinType::Exec && p.type != PinType::Entity) {
            n.values[p.name] = p.value;
        }
    }
    if (def->uses_variable && !variables.empty()) {
        n.variable = variables.front().name;
    }
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void VisualGraph::remove_node(u32 id) {
    std::erase_if(nodes, [&](const GraphNode& n) { return n.id == id; });
    std::erase_if(links, [&](const GraphLink& l) { return l.from_node == id || l.to_node == id; });
}

const GraphVariable* VisualGraph::find_variable(std::string_view name) const {
    for (const GraphVariable& v : variables) {
        if (v.name == name) {
            return &v;
        }
    }
    return nullptr;
}

PinType VisualGraph::pin_type(const GraphNode& node, std::string_view pin, bool output) const {
    const NodeDef* def = find_node_def(node.type);
    if (def == nullptr) {
        return PinType::Exec;
    }
    const PinDef* p = find_pin(output ? def->outputs : def->inputs, pin);
    if (p == nullptr) {
        return PinType::Exec;
    }
    if (def->uses_variable && p->type == PinType::Any) {
        const GraphVariable* v = find_variable(node.variable);
        return v != nullptr ? v->type : PinType::Any;
    }
    return p->type;
}

const GraphLink* VisualGraph::input_link(u32 node, std::string_view pin) const {
    for (const GraphLink& l : links) {
        if (l.to_node == node && l.to_pin == pin) {
            return &l;
        }
    }
    return nullptr;
}

const GraphLink* VisualGraph::output_link(u32 node, std::string_view pin) const {
    for (const GraphLink& l : links) {
        if (l.from_node == node && l.from_pin == pin) {
            return &l;
        }
    }
    return nullptr;
}

void VisualGraph::disconnect_input(u32 node, std::string_view pin) {
    std::erase_if(links, [&](const GraphLink& l) { return l.to_node == node && l.to_pin == pin; });
}

bool VisualGraph::connect(u32 from_node, std::string_view from_pin, u32 to_node, std::string_view to_pin) {
    const GraphNode* a = find(from_node);
    const GraphNode* b = find(to_node);
    if (a == nullptr || b == nullptr || from_node == to_node) {
        return false;
    }
    const NodeDef* da = find_node_def(a->type);
    const NodeDef* db = find_node_def(b->type);
    if (da == nullptr || db == nullptr || find_pin(da->outputs, from_pin) == nullptr ||
        find_pin(db->inputs, to_pin) == nullptr) {
        return false;
    }
    const PinType ta = pin_type(*a, from_pin, true);
    const PinType tb = pin_type(*b, to_pin, false);
    if (!pins_compatible(ta, tb)) {
        return false;
    }
    if (ta == PinType::Exec) {
        // An exec output drives exactly one node (exec inputs accept many wires).
        std::erase_if(links, [&](const GraphLink& l) { return l.from_node == from_node && l.from_pin == from_pin; });
    } else {
        disconnect_input(to_node, to_pin); // a data input reads exactly one wire
    }
    links.push_back(GraphLink{ from_node, std::string(from_pin), to_node, std::string(to_pin) });
    return true;
}

VisualGraph make_starter_graph() {
    VisualGraph g;
    const u32   update = g.add_node("event.update", Vec2(40.0f, 60.0f));
    const u32   rotate = g.add_node("action.rotate", Vec2(300.0f, 40.0f));
    const u32   dt = g.add_node("data.delta_time", Vec2(40.0f, 200.0f));
    const u32   mul = g.add_node("math.multiply", Vec2(40.0f, 280.0f));
    g.find(mul)->values["a"] = 90.0;
    g.connect(update, "then", rotate, "in");
    g.connect(dt, "seconds", mul, "b");
    g.connect(mul, "result", rotate, "degrees");
    return g;
}

// =================================================================================================
// JSON
// =================================================================================================
namespace {
json value_to_json(const GraphValue& v) {
    return std::visit(
        [](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Vec3>) {
                return json::array({ x.x, x.y, x.z });
            } else {
                return json(x);
            }
        },
        v);
}

GraphValue value_from_json(const json& j, PinType hint) {
    if (j.is_boolean()) {
        return j.get<bool>();
    }
    if (j.is_number()) {
        return j.get<f64>();
    }
    if (j.is_string()) {
        return j.get<std::string>();
    }
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        return Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
    }
    return default_value(hint);
}
} // namespace

std::string graph_to_json(const VisualGraph& g) {
    json vars = json::array();
    for (const GraphVariable& v : g.variables) {
        vars.push_back({ { "name", v.name }, { "type", pin_type_name(v.type) }, { "value", value_to_json(v.value) } });
    }
    json nodes = json::array();
    for (const GraphNode& n : g.nodes) {
        json values = json::object();
        for (const auto& [k, v] : n.values) {
            values[k] = value_to_json(v);
        }
        json jn = { { "id", n.id }, { "type", n.type }, { "pos", json::array({ n.position.x, n.position.y }) },
                    { "values", std::move(values) } };
        if (!n.variable.empty()) {
            jn["variable"] = n.variable;
        }
        nodes.push_back(std::move(jn));
    }
    json links = json::array();
    for (const GraphLink& l : g.links) {
        links.push_back({ { "from", json::array({ l.from_node, l.from_pin }) }, { "to", json::array({ l.to_node, l.to_pin }) } });
    }
    const json doc = { { "format", "aether.graph" }, { "version", 1 }, { "variables", std::move(vars) },
                       { "nodes", std::move(nodes) }, { "links", std::move(links) } };
    return doc.dump(2) + "\n";
}

Result<VisualGraph> graph_from_json(std::string_view text) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return make_error<VisualGraph>(ErrorCode::InvalidArgument, "visual script: not valid JSON");
    }
    if (doc.value("format", std::string()) != "aether.graph") {
        return make_error<VisualGraph>(ErrorCode::InvalidArgument, "visual script: not an aether.graph document");
    }
    if (doc.value("version", 0) != 1) {
        return make_error<VisualGraph>(ErrorCode::InvalidArgument, "visual script: unsupported version");
    }
    VisualGraph g;
    try {
        const json variables = doc.value("variables", json::array());
        const json nodes = doc.value("nodes", json::array());
        const json links = doc.value("links", json::array());
        for (const json& jv : variables) {
            GraphVariable v;
            v.name = jv.at("name").get<std::string>();
            v.type = pin_type_from_name(jv.value("type", std::string("number")));
            v.value = value_from_json(jv.value("value", json()), v.type);
            g.variables.push_back(std::move(v));
        }
        for (const json& jn : nodes) {
            GraphNode n;
            n.id = jn.at("id").get<u32>();
            n.type = jn.at("type").get<std::string>();
            const json pos = jn.value("pos", json::array({ 0.0, 0.0 }));
            if (pos.is_array() && pos.size() == 2) {
                n.position = Vec2(pos[0].get<f32>(), pos[1].get<f32>());
            }
            const NodeDef* def = find_node_def(n.type);
            const json values = jn.value("values", json::object()); // keep alive while iterating items()
            for (const auto& [k, v] : values.items()) {
                const PinDef* p = def != nullptr ? find_pin(def->inputs, k) : nullptr;
                n.values[k] = value_from_json(v, p != nullptr ? p->type : PinType::Number);
            }
            n.variable = jn.value("variable", std::string());
            g.nodes.push_back(std::move(n));
        }
        for (const json& jl : links) {
            GraphLink l;
            l.from_node = jl.at("from").at(0).get<u32>();
            l.from_pin = jl.at("from").at(1).get<std::string>();
            l.to_node = jl.at("to").at(0).get<u32>();
            l.to_pin = jl.at("to").at(1).get<std::string>();
            g.links.push_back(std::move(l));
        }
    } catch (const json::exception& e) {
        return make_error<VisualGraph>(ErrorCode::InvalidArgument, std::format("visual script: {}", e.what()));
    }
    return g;
}

Result<VisualGraph> load_graph(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return make_error<VisualGraph>(ErrorCode::NotFound, std::format("cannot open '{}'", paths::to_utf8(path)));
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return graph_from_json(text);
}

Result<void> save_graph(const VisualGraph& graph, const std::filesystem::path& path) {
    const std::string path_utf8 = paths::to_utf8(path);
    std::error_code   ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        const std::string text = graph_to_json(graph);
        if (!out.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            return make_error(ErrorCode::IoError, std::format("cannot write '{}'", path_utf8));
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return make_error(ErrorCode::IoError, std::format("cannot replace '{}'", path_utf8));
    }
    return {};
}

// =================================================================================================
// compiler
// =================================================================================================
namespace {
std::string lua_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\{:03}", static_cast<int>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string lua_number(f64 v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    std::string s = std::format("{}", v);
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0"; // graph numbers are floats in Lua too (1.0, not the integer 1)
    }
    return s;
}

std::string lua_literal(const GraphValue& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, f64>) {
                return lua_number(x);
            } else if constexpr (std::is_same_v<T, Vec3>) {
                return std::format("Vec3({}, {}, {})", lua_number(x.x), lua_number(x.y), lua_number(x.z));
            } else {
                return lua_string(x);
            }
        },
        v);
}

// Literal of the right kind for a pin (coerces a stored value of the wrong kind).
std::string typed_literal(const GraphValue& v, PinType t) {
    switch (t) {
    case PinType::Bool: return std::holds_alternative<bool>(v) ? lua_literal(v) : "false";
    case PinType::Number: return std::holds_alternative<f64>(v) ? lua_literal(v) : "0.0";
    case PinType::Vector: return std::holds_alternative<Vec3>(v) ? lua_literal(v) : "Vec3(0, 0, 0)";
    case PinType::String: return std::holds_alternative<std::string>(v) ? lua_literal(v) : lua_string("");
    default: return lua_literal(v);
    }
}

class Compiler {
public:
    Compiler(const VisualGraph& g, GraphCompileResult& r) : g_(g), r_(r) {}

    void error(u32 node, std::string msg) {
        for (const GraphError& e : r_.errors) {
            if (e.node == node && e.message == msg) {
                return;
            }
        }
        r_.errors.push_back(GraphError{ node, std::move(msg) });
    }

    const NodeDef* def(const GraphNode& n) {
        const NodeDef* d = find_node_def(n.type);
        if (d == nullptr) {
            error(n.id, std::format("unknown node type '{}'", n.type));
        }
        return d;
    }

    std::string title(const GraphNode& n) {
        const NodeDef* d = find_node_def(n.type);
        return d != nullptr ? d->title : n.type;
    }

    // Expression of a data input: its wire, else its literal (entity inputs: Self).
    std::string input(const GraphNode& n, std::string_view pin, int depth) {
        if (const GraphLink* l = g_.input_link(n.id, pin)) {
            const GraphNode* src = g_.find(l->from_node);
            if (src == nullptr) {
                error(n.id, std::format("'{}' is wired to a missing node", pin));
                return "nil";
            }
            return output(*src, l->from_pin, depth + 1);
        }
        const PinType t = g_.pin_type(n, pin, false);
        if (t == PinType::Entity) {
            return "self.entity";
        }
        const auto it = n.values.find(std::string(pin));
        const GraphValue v = it != n.values.end() ? it->second : default_value(t);
        return typed_literal(v, t == PinType::Any ? PinType::String : t);
    }

    // Expression of a data output.
    std::string output(const GraphNode& n, std::string_view pin, int depth) {
        if (depth > 64) {
            error(n.id, "data wires form a loop");
            return "nil";
        }
        const NodeDef* d = def(n);
        if (d == nullptr) {
            return "nil";
        }
        auto in = [&](std::string_view p) { return input(n, p, depth); };
        const std::string& t = n.type;
        if (t == "action.spawn") return std::format("self.__n{}_entity", n.id);
        if (t == "data.get_variable") return variable_ref(n);
        if (t == "data.self") return "self.entity";
        if (t == "data.find_entity") return std::format("world.find({})", in("name"));
        if (t == "data.get_position") return std::format("({}):position()", in("target"));
        if (t == "data.get_world_position") return std::format("({}):world_position()", in("target"));
        if (t == "data.forward") return std::format("({}):forward()", in("target"));
        if (t == "data.delta_time") return "time.dt";
        if (t == "data.time") return "time.time";
        if (t == "input.key_down") return std::format("(input.available() and input.key_down({}))", in("key"));
        if (t == "input.axis") return std::format("(input.available() and input.axis({}, {}) or 0)", in("positive"), in("negative"));
        if (t == "math.add") return std::format("({} + {})", in("a"), in("b"));
        if (t == "math.subtract") return std::format("({} - {})", in("a"), in("b"));
        if (t == "math.multiply") return std::format("({} * {})", in("a"), in("b"));
        if (t == "math.divide") return std::format("({} / {})", in("a"), in("b"));
        if (t == "math.sin") return std::format("math.sin({})", in("radians"));
        if (t == "math.cos") return std::format("math.cos({})", in("radians"));
        if (t == "math.abs") return std::format("math.abs({})", in("value"));
        if (t == "math.min") return std::format("math.min({}, {})", in("a"), in("b"));
        if (t == "math.max") return std::format("math.max({}, {})", in("a"), in("b"));
        if (t == "math.clamp") return std::format("math.max({}, math.min({}, {}))", in("min"), in("max"), in("value"));
        if (t == "math.lerp") {
            const std::string a = in("a");
            return std::format("({0} + ({1} - {0}) * {2})", a, in("b"), in("t"));
        }
        if (t == "math.random") {
            const std::string lo = in("min");
            return std::format("({0} + math.random() * ({1} - {0}))", lo, in("max"));
        }
        if (t == "compare.greater") return std::format("({} > {})", in("a"), in("b"));
        if (t == "compare.less") return std::format("({} < {})", in("a"), in("b"));
        if (t == "compare.equal") return std::format("({} == {})", in("a"), in("b"));
        if (t == "logic.and") return std::format("({} and {})", in("a"), in("b"));
        if (t == "logic.or") return std::format("({} or {})", in("a"), in("b"));
        if (t == "logic.not") return std::format("(not {})", in("a"));
        if (t == "vector.make") return std::format("Vec3({}, {}, {})", in("x"), in("y"), in("z"));
        if (t == "vector.break") {
            if (pin != "x" && pin != "y" && pin != "z") {
                error(n.id, std::format("unknown output '{}'", pin));
                return "0";
            }
            return std::format("({}).{}", in("vector"), pin);
        }
        if (t == "vector.add") return std::format("({} + {})", in("a"), in("b"));
        if (t == "vector.subtract") return std::format("({} - {})", in("a"), in("b"));
        if (t == "vector.scale") return std::format("({} * {})", in("vector"), in("scale"));
        if (t == "vector.length") return std::format("({}):length()", in("vector"));
        if (t == "vector.normalize") return std::format("({}):normalized()", in("vector"));
        if (t == "vector.distance") return std::format("({}):distance({})", in("a"), in("b"));
        if (t == "text.join") return std::format("(tostring({}) .. tostring({}))", in("a"), in("b"));
        error(n.id, std::format("'{}' has no value output", d->title));
        return "nil";
    }

    std::string variable_ref(const GraphNode& n) {
        if (g_.find_variable(n.variable) == nullptr) {
            error(n.id, n.variable.empty() ? std::string("no variable selected")
                                           : std::format("variable '{}' does not exist", n.variable));
            return "nil";
        }
        return "self." + n.variable;
    }

    // Emits the chain that starts at exec output `pin` of `from`.
    void chain(std::string& out, const GraphNode& from, std::string_view pin, const std::string& ind,
               std::vector<u32>& path) {
        const GraphLink* l = g_.output_link(from.id, pin);
        if (l == nullptr) {
            return;
        }
        const GraphNode* n = g_.find(l->to_node);
        if (n == nullptr) {
            error(from.id, "an execution wire leads to a missing node");
            return;
        }
        if (std::find(path.begin(), path.end(), n->id) != path.end()) {
            error(n->id, std::format("execution loops back into '{}'", title(*n)));
            return;
        }
        if (path.size() > 256) {
            error(n->id, "execution chain too long");
            return;
        }
        const NodeDef* d = def(*n);
        if (d == nullptr) {
            return;
        }
        if (d->inputs.empty() || d->inputs.front().type != PinType::Exec) {
            error(n->id, std::format("'{}' cannot be executed", d->title));
            return;
        }
        path.push_back(n->id);
        statement(out, *n, ind, path);
        path.pop_back();
    }

    // Entity action guarded against invalid / nil targets.
    void entity_call(std::string& out, const GraphNode& n, const std::string& ind, const std::string& call, int depth = 0) {
        out += std::format("{}do local t = {} if t and t:valid() then t:{} end end\n", ind, input(n, "target", depth), call);
    }

    void statement(std::string& out, const GraphNode& n, const std::string& ind, std::vector<u32>& path) {
        const std::string& t = n.type;
        auto in = [&](std::string_view p) { return input(n, p, 0); };
        auto then = [&](std::string_view p, const std::string& i) {
            std::vector<u32> sub = path;
            chain(out, n, p, i, sub);
        };
        if (t == "flow.branch") {
            out += std::format("{}if {} then\n", ind, in("condition"));
            then("true", ind + "    ");
            out += ind + "else\n";
            then("false", ind + "    ");
            out += ind + "end\n";
            return;
        }
        if (t == "flow.sequence") {
            for (const char* p : { "then 1", "then 2", "then 3" }) {
                then(p, ind);
            }
            return;
        }
        if (t == "flow.delay") {
            out += std::format("{}timer.after({}, function()\n", ind, in("seconds"));
            then("then", ind + "    ");
            out += ind + "end)\n";
            return;
        }
        if (t == "action.move_over_time" || t == "action.move_to_over_time" || t == "action.rotate_over_time") {
            // A per-update timer interpolates with smoothstep easing; 'finished' runs inside it at k = 1.
            const std::string i2 = ind + "    ";
            const std::string i3 = i2 + "    ";
            const std::string i4 = i3 + "    ";
            out += std::format("{}do local t = {} if t and t:valid() then\n", ind, in("target"));
            out += std::format("{}local dur, ease, t0 = math.max({}, 0.001), {}, time.time\n", i2, in("seconds"), in("ease"));
            if (t == "action.rotate_over_time") {
                out += std::format("{}local axis, total, done = ({}):normalized(), {}, 0.0\n", i2, in("axis"), in("degrees"));
            } else if (t == "action.move_over_time") {
                // Additive: each tick applies only its share of the offset, so overlapping moves (a
                // hop pressed again mid-air) add up and a matching opposite move always lands back.
                out += std::format("{}local delta, done = {}, 0.0\n", i2, in("offset"));
            } else {
                out += std::format("{}local from = t:position()\n{}local delta = {} - from\n", i2, i2, in("position"));
            }
            out += std::format("{}timer.every(0.0001, function(h)\n", i2);
            out += std::format("{}local k = math.min((time.time - t0) / dur, 1.0)\n", i3);
            out += std::format("{}local s = ease and k * k * (3.0 - 2.0 * k) or k\n", i3);
            if (t == "action.rotate_over_time") {
                out += std::format("{}if t:valid() then t:rotate(Quat.angle_axis(math.rad(total * s - done), axis)) end\n", i3);
                out += std::format("{}done = total * s\n", i3);
            } else if (t == "action.move_over_time") {
                out += std::format("{}if t:valid() then t:set_position(t:position() + delta * (s - done)) end\n", i3);
                out += std::format("{}done = s\n", i3);
            } else {
                out += std::format("{}if t:valid() then t:set_position(from + delta * s) end\n", i3);
            }
            out += std::format("{}if k >= 1.0 then\n{}h:cancel()\n", i3, i4);
            then("finished", i4);
            out += std::format("{}end\n{}end)\n{}end end\n", i3, i2, ind);
        } else if (t == "action.print") {
            out += std::format("{}log.info(tostring({}))\n", ind, in("value"));
        } else if (t == "action.set_position") {
            entity_call(out, n, ind, std::format("set_position({})", in("position")));
        } else if (t == "action.set_world_position") {
            entity_call(out, n, ind, std::format("set_world_position({})", in("position")));
        } else if (t == "action.translate") {
            entity_call(out, n, ind, std::format("translate({})", in("offset")));
        } else if (t == "action.rotate") {
            entity_call(out, n, ind, std::format("rotate(Quat.angle_axis(math.rad({}), ({}):normalized()))", in("degrees"), in("axis")));
        } else if (t == "action.set_scale") {
            entity_call(out, n, ind, std::format("set_scale({})", in("scale")));
        } else if (t == "action.set_visible") {
            entity_call(out, n, ind, std::format("set_visible({})", in("visible")));
        } else if (t == "action.look_at") {
            entity_call(out, n, ind, std::format("look_at({})", in("position")));
        } else if (t == "action.destroy") {
            entity_call(out, n, ind, "destroy()");
        } else if (t == "action.spawn") {
            out += std::format("{}self.__n{}_entity = world.spawn({})\n", ind, n.id, in("name"));
            out += std::format("{}self.__n{}_entity:set_position({})\n", ind, n.id, in("position"));
        } else if (t == "action.emit_event") {
            out += std::format("{}events.emit({}, {{}})\n", ind, in("name"));
        } else if (t == "action.set_variable") {
            const std::string ref = variable_ref(n);
            if (ref != "nil") {
                out += std::format("{}{} = {}\n", ind, ref, in("value"));
            }
        } else {
            error(n.id, std::format("'{}' cannot be executed", title(n)));
            return;
        }
        then("then", ind);
    }

    std::string run(std::string_view source_name) {
        // Structural checks.
        std::set<u32> ids;
        for (const GraphNode& n : g_.nodes) {
            if (!ids.insert(n.id).second || n.id == 0) {
                error(n.id, "duplicate or zero node id");
            }
            const NodeDef* d = def(n);
            if (d != nullptr && d->uses_variable) {
                (void)variable_ref(n);
            }
        }
        for (const GraphLink& l : g_.links) {
            const GraphNode* a = g_.find(l.from_node);
            const GraphNode* b = g_.find(l.to_node);
            if (a == nullptr || b == nullptr) {
                error(0, "a wire connects a missing node");
                continue;
            }
            const NodeDef* da = find_node_def(a->type);
            const NodeDef* db = find_node_def(b->type);
            if (da == nullptr || db == nullptr) {
                continue;
            }
            if (find_pin(da->outputs, l.from_pin) == nullptr || find_pin(db->inputs, l.to_pin) == nullptr) {
                error(b->id, std::format("wire to unknown pin '{}' / '{}'", l.from_pin, l.to_pin));
                continue;
            }
            if (!pins_compatible(g_.pin_type(*a, l.from_pin, true), g_.pin_type(*b, l.to_pin, false))) {
                error(b->id, std::format("'{}' cannot take a {} value", l.to_pin, pin_type_name(g_.pin_type(*a, l.from_pin, true))));
            }
        }
        std::set<std::string> var_names;
        for (const GraphVariable& v : g_.variables) {
            if (!valid_identifier(v.name) || v.name.starts_with("__") || v.name == "entity") {
                error(0, std::format("invalid variable name '{}'", v.name));
            } else if (!var_names.insert(v.name).second) {
                error(0, std::format("duplicate variable '{}'", v.name));
            }
        }

        std::string start, update, keys;
        const std::string ind = "    ";
        std::vector<const GraphNode*> events;
        for (const GraphNode& n : g_.nodes) {
            if (n.type.starts_with("event.")) {
                events.push_back(&n);
            }
        }
        std::sort(events.begin(), events.end(), [](const GraphNode* a, const GraphNode* b) { return a->id < b->id; });
        for (const GraphNode* e : events) {
            std::vector<u32> path{ e->id };
            auto in = [&](std::string_view p) { return input(*e, p, 0); };
            if (e->type == "event.start") {
                chain(start, *e, "then", ind, path);
            } else if (e->type == "event.update") {
                chain(update, *e, "then", ind, path);
            } else if (e->type == "event.key_pressed" || e->type == "event.key_released") {
                keys += std::format("{}if input.{}({}) then\n", ind + ind,
                                    e->type == "event.key_pressed" ? "key_pressed" : "key_released", in("key"));
                chain(keys, *e, "then", ind + ind + ind, path);
                keys += ind + ind + "end\n";
            } else if (e->type == "event.custom") {
                start += std::format("{}events.subscribe({}, function()\n", ind, in("name"));
                chain(start, *e, "then", ind + ind, path);
                start += ind + "end)\n";
            } else if (e->type == "event.timer") {
                start += std::format("{}timer.every({}, function()\n", ind, in("seconds"));
                chain(start, *e, "then", ind + ind, path);
                start += ind + "end)\n";
            } else {
                def(*e);
            }
        }
        if (!keys.empty()) {
            update += ind + "if input.available() then\n" + keys + ind + "end\n";
        }

        std::string lua = std::format("-- Generated by the Aether visual script compiler from {}.\n"
                                      "-- Edit the graph in the editor, not this code.\n",
                                      source_name.empty() ? std::string_view("a graph") : source_name);
        lua += "properties = {\n";
        for (const GraphVariable& v : g_.variables) {
            if (valid_identifier(v.name)) {
                lua += std::format("    {} = {},\n", v.name, typed_literal(v.value, v.type));
            }
        }
        lua += "}\n\n";
        lua += "function on_start(self)\n" + start + "end\n\n";
        lua += "function on_update(self, dt)\n" + update + "end\n";
        return lua;
    }

private:
    const VisualGraph&  g_;
    GraphCompileResult& r_;
};
} // namespace

GraphCompileResult compile_graph_to_lua(const VisualGraph& graph, std::string_view source_name) {
    GraphCompileResult r;
    Compiler           c(graph, r);
    r.lua = c.run(source_name);
    return r;
}

} // namespace aether::scripting
