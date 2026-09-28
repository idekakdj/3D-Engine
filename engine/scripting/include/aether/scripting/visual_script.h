// aether/scripting/visual_script.h — Blueprint-style visual scripts (ADR-0018).
//
// A visual script is a node graph saved as a `.aegraph` JSON file under content/scripts. Attach it
// to an entity exactly like a Lua script (ScriptComponent::script = "door.aegraph"): the ScriptVM
// compiles the graph to Lua when it loads the file (compile_graph_to_lua), so graphs get the same
// per-entity instances, sandbox, error isolation, hot reload and inspector properties as Lua.
//
//   * Event nodes (On Start, On Update, On Key Pressed, ...) start execution chains; execution
//     flows along exec wires (white) through action and flow nodes.
//   * Data wires carry typed values (bool, number, vector, string, entity) from pure nodes (math,
//     queries) or from other nodes' outputs. An unconnected data input uses its literal value
//     (entity inputs default to the script's own entity, "Self").
//   * Graph variables become the script's `properties`: per-entity state, editable in the
//     inspector like Lua properties.
//
// Thread-safety: plain data + pure functions; thread-safe.
#pragma once

#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aether::scripting {

enum class PinType : u8 { Exec = 0, Bool, Number, Vector, String, Entity, Any };
[[nodiscard]] const char* pin_type_name(PinType t) noexcept;

// A literal value of an unconnected input (or a variable default). Entity literals do not exist.
using GraphValue = std::variant<bool, f64, Vec3, std::string>;
[[nodiscard]] GraphValue default_value(PinType t);

struct PinDef {
    std::string name;  // identifier within the node ("then", "position", ...)
    PinType     type = PinType::Exec;
    GraphValue  value{ 0.0 }; // default literal (data inputs)
};

struct NodeDef {
    std::string         type;     // stable id, e.g. "action.translate"
    std::string         title;    // "Move By"
    std::string         category; // "Events", "Flow", "Actions", "Math", ...
    std::string         help;     // one line for tooltips
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    bool                uses_variable = false; // Get / Set Variable: typed by the chosen variable
};

// Every node type, in menu order.
[[nodiscard]] const std::vector<NodeDef>& node_library();
[[nodiscard]] const NodeDef*              find_node_def(std::string_view type);

struct GraphNode {
    u32                               id = 0;
    std::string                       type;
    Vec2                              position{ 0.0f }; // editor canvas position
    std::map<std::string, GraphValue> values;           // literals of unconnected data inputs
    std::string                       variable;         // Get / Set Variable
};

struct GraphLink {
    u32         from_node = 0;
    std::string from_pin;  // output
    u32         to_node = 0;
    std::string to_pin;    // input
};

struct GraphVariable {
    std::string name; // Lua identifier
    PinType     type = PinType::Number;
    GraphValue  value{ 0.0 };
};

struct VisualGraph {
    std::vector<GraphNode>     nodes;
    std::vector<GraphLink>     links;
    std::vector<GraphVariable> variables;

    [[nodiscard]] GraphNode*       find(u32 id);
    [[nodiscard]] const GraphNode* find(u32 id) const;
    [[nodiscard]] u32              next_id() const; // max id + 1
    // Adds a node of `type` with its default literals; returns its id (0 for an unknown type).
    u32 add_node(std::string_view type, Vec2 position = Vec2(0.0f));
    void remove_node(u32 id); // and its links
    // Resolved type of a pin (variables type Get / Set Variable pins); Exec for unknown pins.
    [[nodiscard]] PinType pin_type(const GraphNode& node, std::string_view pin, bool output) const;
    // Connects output -> input when the types are compatible (an input accepts one data wire, an
    // exec output drives one node: the previous wire is replaced). Returns false (unchanged) for
    // incompatible or unknown pins, and for links to the node itself.
    bool connect(u32 from_node, std::string_view from_pin, u32 to_node, std::string_view to_pin);
    void disconnect_input(u32 node, std::string_view pin);
    [[nodiscard]] const GraphLink* input_link(u32 node, std::string_view pin) const;
    [[nodiscard]] const GraphLink* output_link(u32 node, std::string_view pin) const; // exec outputs
    [[nodiscard]] const GraphVariable* find_variable(std::string_view name) const;
};

// Whether an output of type `from` may feed an input of type `to`.
[[nodiscard]] bool pins_compatible(PinType from, PinType to) noexcept;

// ---- files ------------------------------------------------------------------------------------
// JSON ("format": "aether.graph", "version": 1).
[[nodiscard]] std::string         graph_to_json(const VisualGraph& graph);
[[nodiscard]] Result<VisualGraph> graph_from_json(std::string_view text);
[[nodiscard]] Result<VisualGraph> load_graph(const std::filesystem::path& path);
// Writes through a temporary file + rename (a crash never leaves half a graph).
Result<void>                      save_graph(const VisualGraph& graph, const std::filesystem::path& path);

// ---- compiler -----------------------------------------------------------------------------------
struct GraphError {
    u32         node = 0; // 0 = the graph itself
    std::string message;
};
struct GraphCompileResult {
    std::string             lua;
    std::vector<GraphError> errors;
    [[nodiscard]] bool      ok() const noexcept { return errors.empty(); }
};
// Lua source equivalent to the graph (on_start / on_update / properties). `source_name` is only
// used in the generated header comment.
[[nodiscard]] GraphCompileResult compile_graph_to_lua(const VisualGraph& graph, std::string_view source_name = {});

// A minimal starter graph: On Update -> Rotate (Self, up, 90 deg/s x dt).
[[nodiscard]] VisualGraph make_starter_graph();

} // namespace aether::scripting
