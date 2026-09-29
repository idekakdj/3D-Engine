// graph_editor.cpp — the visual script editor window (see graph_editor.h, ADR-0018).
#include "graph_editor.h"

#include "aether/core/log.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <format>

namespace aether::editor {

using namespace aether::scripting;

namespace {
constexpr f32 kNodeWidth = 210.0f;
constexpr f32 kWideNodeWidth = 300.0f;
constexpr f32 kHeader = 24.0f;
constexpr f32 kRow = 24.0f;
constexpr f32 kPinRadius = 5.5f;

ImVec2 iv(const Vec2& v) { return ImVec2(v.x, v.y); }
Vec2   vv(const ImVec2& v) { return Vec2(v.x, v.y); }

ImU32 pin_color(PinType t) {
    switch (t) {
    case PinType::Exec: return IM_COL32(235, 235, 235, 255);
    case PinType::Bool: return IM_COL32(200, 60, 60, 255);
    case PinType::Number: return IM_COL32(120, 220, 120, 255);
    case PinType::Vector: return IM_COL32(240, 200, 60, 255);
    case PinType::String: return IM_COL32(230, 90, 200, 255);
    case PinType::Entity: return IM_COL32(80, 170, 250, 255);
    case PinType::Any: return IM_COL32(170, 170, 170, 255);
    }
    return IM_COL32_WHITE;
}

ImU32 header_color(std::string_view category) {
    if (category == "Events") return IM_COL32(150, 40, 40, 255);
    if (category == "Flow") return IM_COL32(90, 90, 100, 255);
    if (category == "Actions") return IM_COL32(40, 80, 150, 255);
    if (category == "Variables") return IM_COL32(40, 120, 90, 255);
    if (category == "Entity") return IM_COL32(40, 110, 140, 255);
    if (category == "Input") return IM_COL32(130, 80, 150, 255);
    return IM_COL32(70, 100, 70, 255); // math, logic, vector, text, time
}

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != hay.end();
}

bool input_string(const char* label, std::string& s, f32 width) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buf, sizeof(buf))) {
        s = buf;
        return true;
    }
    return false;
}

const char* kVariableTypes[] = { "Bool", "Number", "Vector", "String" };
PinType     variable_type_at(int i) {
    switch (i) {
    case 0: return PinType::Bool;
    case 2: return PinType::Vector;
    case 3: return PinType::String;
    default: return PinType::Number;
    }
}
int variable_type_index(PinType t) {
    switch (t) {
    case PinType::Bool: return 0;
    case PinType::Vector: return 2;
    case PinType::String: return 3;
    default: return 1;
    }
}
} // namespace

// =================================================================================================
// file / editing
// =================================================================================================
bool GraphEditor::open(const std::filesystem::path& file, std::string display) {
    if (is_open() && dirty_ && file != file_) {
        save(); // never lose edits when switching graphs
    }
    auto g = load_graph(file);
    if (!g) {
        AE_LOG_ERROR("Editor", "cannot open visual script {}: {}", display, g.error().message);
        return false;
    }
    graph_ = std::move(*g);
    file_ = file;
    display_ = std::move(display);
    undo_.clear();
    redo_.clear();
    selection_.clear();
    dirty_ = false;
    compile_stale_ = true;
    scroll_ = Vec2(40.0f, 40.0f);
    zoom_ = 1.0f;
    return true;
}

bool GraphEditor::create(const std::filesystem::path& file, std::string display) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        AE_LOG_ERROR("Editor", "{} already exists", display);
        return false;
    }
    if (auto r = save_graph(make_starter_graph(), file); !r) {
        AE_LOG_ERROR("Editor", "cannot create {}: {}", display, r.error().message);
        return false;
    }
    return open(file, std::move(display));
}

bool GraphEditor::save() {
    if (!is_open()) {
        return false;
    }
    if (auto r = save_graph(graph_, file_); !r) {
        AE_LOG_ERROR("Editor", "saving {} failed: {}", display_, r.error().message);
        return false;
    }
    dirty_ = false;
    AE_LOG_INFO("Editor", "saved {}{}", display_, compiled().ok() ? "" : " (it has errors: the previous version keeps running)");
    return true;
}

void GraphEditor::close() {
    file_.clear();
    graph_ = {};
    undo_.clear();
    redo_.clear();
    selection_.clear();
    dirty_ = false;
}

const GraphCompileResult& GraphEditor::compiled() {
    if (compile_stale_) {
        compiled_ = compile_graph_to_lua(graph_, display_);
        compile_stale_ = false;
    }
    return compiled_;
}

void GraphEditor::snapshot() {
    undo_.push_back(graph_to_json(graph_));
    if (undo_.size() > 200) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

void GraphEditor::changed() {
    dirty_ = true;
    compile_stale_ = true;
}

bool GraphEditor::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(graph_to_json(graph_));
    if (auto g = graph_from_json(undo_.back())) {
        graph_ = std::move(*g);
    }
    undo_.pop_back();
    selection_.clear();
    changed();
    return true;
}

bool GraphEditor::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(graph_to_json(graph_));
    if (auto g = graph_from_json(redo_.back())) {
        graph_ = std::move(*g);
    }
    redo_.pop_back();
    selection_.clear();
    changed();
    return true;
}

u32 GraphEditor::add_node(std::string_view type, Vec2 canvas_pos) {
    snapshot();
    const u32 id = graph_.add_node(type, canvas_pos);
    if (id == 0) {
        undo_.pop_back();
        return 0;
    }
    selection_ = { id };
    changed();
    return id;
}

bool GraphEditor::connect(u32 from, std::string_view from_pin, u32 to, std::string_view to_pin) {
    const std::string before = graph_to_json(graph_);
    if (!graph_.connect(from, from_pin, to, to_pin)) {
        return false;
    }
    undo_.push_back(before);
    redo_.clear();
    changed();
    return true;
}

void GraphEditor::delete_selection() {
    if (selection_.empty()) {
        return;
    }
    snapshot();
    for (u32 id : selection_) {
        graph_.remove_node(id);
    }
    selection_.clear();
    changed();
}

// =================================================================================================
// geometry
// =================================================================================================
Vec2 GraphEditor::node_size(const GraphNode& n) const {
    const NodeDef* d = find_node_def(n.type);
    if (d == nullptr) {
        return Vec2(kNodeWidth, kHeader + kRow);
    }
    bool wide = false;
    for (const PinDef& p : d->inputs) {
        if (graph_.pin_type(n, p.name, false) == PinType::Vector && graph_.input_link(n.id, p.name) == nullptr) {
            wide = true;
        }
    }
    const usize rows = std::max(d->inputs.size(), d->outputs.size()) + (d->uses_variable ? 1u : 0u);
    return Vec2(wide ? kWideNodeWidth : kNodeWidth, kHeader + static_cast<f32>(std::max<usize>(rows, 1)) * kRow + 6.0f);
}

Vec2 GraphEditor::pin_position(const GraphNode& n, std::string_view pin, bool output, const Vec2& origin) const {
    const NodeDef* d = find_node_def(n.type);
    if (d == nullptr) {
        return to_screen(n.position, origin);
    }
    const auto& pins = output ? d->outputs : d->inputs;
    usize       index = 0;
    for (; index < pins.size() && pins[index].name != pin; ++index) {
    }
    const f32  row = static_cast<f32>(index + (d->uses_variable ? 1u : 0u));
    const Vec2 size = node_size(n);
    const Vec2 local(output ? size.x : 0.0f, kHeader + (row + 0.5f) * kRow);
    return to_screen(n.position + local, origin);
}

GraphEditor::PinRef GraphEditor::pin_at(const Vec2& screen, const Vec2& origin) const {
    const f32 r = (kPinRadius + 4.0f) * zoom_;
    for (auto it = graph_.nodes.rbegin(); it != graph_.nodes.rend(); ++it) {
        const NodeDef* d = find_node_def(it->type);
        if (d == nullptr) {
            continue;
        }
        for (const bool out : { false, true }) {
            for (const PinDef& p : out ? d->outputs : d->inputs) {
                const Vec2 pp = pin_position(*it, p.name, out, origin);
                if (glm::length(pp - screen) <= r) {
                    return PinRef{ it->id, p.name, out };
                }
            }
        }
    }
    return {};
}

u32 GraphEditor::node_at(const Vec2& screen, const Vec2& origin) const {
    for (auto it = graph_.nodes.rbegin(); it != graph_.nodes.rend(); ++it) {
        const Vec2 a = to_screen(it->position, origin);
        const Vec2 b = to_screen(it->position + node_size(*it), origin);
        if (screen.x >= a.x && screen.y >= a.y && screen.x <= b.x && screen.y <= b.y) {
            return it->id;
        }
    }
    return 0;
}

// =================================================================================================
// UI
// =================================================================================================
void GraphEditor::draw(bool* p_open, unsigned int dock_id) {
    if (!is_open()) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(1000.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!docked_once_ && dock_id != 0) { // open as a tab next to the Viewport (once per session)
        ImGui::SetNextWindowDockID(dock_id, ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
    }
    docked_once_ = true;
    const std::string title = std::format("Visual Script - {}{}###VisualScript", display_, dirty_ ? " *" : "");
    if (!ImGui::Begin(title.c_str(), p_open, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                save();
            }
            if (ImGui::MenuItem("Revert to saved", nullptr, false, dirty_)) {
                dirty_ = false; // discard the edits: open() would otherwise save them first
                (void)open(std::filesystem::path(file_), display_);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_.empty())) {
                undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) {
                redo();
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection_.empty())) {
                snapshot();
                std::set<u32> copies;
                for (u32 id : selection_) {
                    if (const GraphNode* n = graph_.find(id)) {
                        GraphNode c = *n;
                        c.id = graph_.next_id();
                        c.position += Vec2(30.0f, 30.0f);
                        graph_.nodes.push_back(c);
                        copies.insert(c.id);
                    }
                }
                selection_ = copies;
                changed();
            }
            if (ImGui::MenuItem("Delete", "Del", false, !selection_.empty())) {
                delete_selection();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset view")) {
                scroll_ = Vec2(40.0f, 40.0f);
                zoom_ = 1.0f;
            }
            ImGui::MenuItem("Show generated Lua", nullptr, &show_lua_);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            save();
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            io.KeyShift ? redo() : undo();
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            redo();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            delete_selection();
        }
    }

    ImGui::BeginChild("##sidebar", ImVec2(230.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    const f32 status_h = show_lua_ ? 220.0f : 74.0f;
    ImGui::BeginChild("##canvas", ImVec2(0.0f, -status_h), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    draw_canvas();
    ImGui::EndChild();
    ImGui::BeginChild("##status", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    draw_status();
    ImGui::EndChild();
    ImGui::EndGroup();
    ImGui::End();
}

void GraphEditor::draw_sidebar() {
    ImGui::SeparatorText("Variables");
    ImGui::TextDisabled("per-entity values (script properties)");
    for (usize i = 0; i < graph_.variables.size(); ++i) {
        GraphVariable& v = graph_.variables[i];
        ImGui::PushID(static_cast<int>(i));
        std::string name = v.name;
        if (input_string("##name", name, 100.0f)) {
            if (ImGui::IsItemActive() && name != v.name) {
                snapshot();
                for (GraphNode& n : graph_.nodes) {
                    if (n.variable == v.name) {
                        n.variable = name;
                    }
                }
                v.name = name;
                changed();
            }
        }
        ImGui::SameLine();
        int type = variable_type_index(v.type);
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::Combo("##type", &type, kVariableTypes, IM_ARRAYSIZE(kVariableTypes))) {
            snapshot();
            v.type = variable_type_at(type);
            v.value = default_value(v.type);
            changed();
        }
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("x");
        // Default value.
        ImGui::Indent(8.0f);
        bool edited = false;
        if (v.type == PinType::Bool) {
            bool b = std::holds_alternative<bool>(v.value) && std::get<bool>(v.value);
            if (ImGui::Checkbox("default##d", &b)) {
                v.value = b;
                edited = true;
            }
        } else if (v.type == PinType::Number) {
            f32 f = std::holds_alternative<f64>(v.value) ? static_cast<f32>(std::get<f64>(v.value)) : 0.0f;
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat("default##d", &f, 0.05f)) {
                v.value = static_cast<f64>(f);
                edited = true;
            }
        } else if (v.type == PinType::Vector) {
            Vec3 x = std::holds_alternative<Vec3>(v.value) ? std::get<Vec3>(v.value) : Vec3(0.0f);
            ImGui::SetNextItemWidth(170.0f);
            if (ImGui::DragFloat3("##d", &x.x, 0.05f)) {
                v.value = x;
                edited = true;
            }
        } else {
            std::string s = std::holds_alternative<std::string>(v.value) ? std::get<std::string>(v.value) : std::string();
            if (input_string("default##d", s, 120.0f)) {
                v.value = s;
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            snapshot();
        }
        if (edited) {
            changed();
        }
        ImGui::Unindent(8.0f);
        ImGui::PopID();
        if (remove) {
            snapshot();
            graph_.variables.erase(graph_.variables.begin() + static_cast<std::ptrdiff_t>(i));
            changed();
            break;
        }
    }
    if (ImGui::Button("+ Variable")) {
        snapshot();
        std::string name;
        for (int k = 1;; ++k) {
            name = std::format("var{}", k);
            if (graph_.find_variable(name) == nullptr) {
                break;
            }
        }
        graph_.variables.push_back(GraphVariable{ name, PinType::Number, 0.0 });
        changed();
    }

    ImGui::SeparatorText("Nodes");
    ImGui::TextDisabled("click to add; or right-click the canvas");
    std::string category;
    bool        open_cat = false;
    for (const NodeDef& d : node_library()) {
        if (d.category != category) {
            category = d.category;
            open_cat = ImGui::CollapsingHeader(category.c_str(), category == "Events" ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        }
        if (open_cat) {
            if (ImGui::Selectable(d.title.c_str())) {
                add_node(d.type, -scroll_ + Vec2(200.0f, 120.0f) + Vec2(static_cast<f32>(graph_.nodes.size() % 5) * 20.0f));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", d.help.c_str());
            }
        }
    }
}

void GraphEditor::draw_status() {
    const GraphCompileResult& c = compiled();
    if (c.ok()) {
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Compiled OK");
        ImGui::SameLine();
        ImGui::TextDisabled("- %zu nodes, %zu wires. Save (Ctrl+S) to apply; attach it like a Lua script.",
                            graph_.nodes.size(), graph_.links.size());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%zu error(s):", c.errors.size());
        for (usize i = 0; i < c.errors.size(); ++i) {
            const GraphError& e = c.errors[i];
            const GraphNode*  n = graph_.find(e.node);
            const NodeDef*    d = n != nullptr ? find_node_def(n->type) : nullptr;
            const std::string line = std::format("{}: {}##err{}", d != nullptr ? d->title : std::string("graph"), e.message, i);
            if (ImGui::Selectable(line.c_str()) && n != nullptr) {
                selection_ = { n->id };
                focus_node_ = n->id;
            }
        }
    }
    if (show_lua_) {
        std::string lua = c.lua;
        ImGui::InputTextMultiline("##lua", lua.data(), lua.size() + 1, ImVec2(-1.0f, -1.0f), ImGuiInputTextFlags_ReadOnly);
    }
}

void GraphEditor::draw_node(u32 id, const Vec2& origin) {
    GraphNode* n = graph_.find(id);
    if (n == nullptr) {
        return;
    }
    const NodeDef* d = find_node_def(n->type);
    ImDrawList*    dl = ImGui::GetWindowDrawList();
    const Vec2     size = node_size(*n);
    const Vec2     a = to_screen(n->position, origin);
    const Vec2     b = a + size * zoom_;
    const f32      round = 6.0f * zoom_;
    const bool     selected = selection_.contains(id);
    bool           has_error = false;
    std::string    error_text;
    for (const GraphError& e : compiled().errors) {
        if (e.node == id) {
            has_error = true;
            error_text += e.message + "\n";
        }
    }
    dl->AddRectFilled(iv(a), iv(b), IM_COL32(32, 34, 40, 235), round);
    dl->AddRectFilled(iv(a), ImVec2(b.x, a.y + kHeader * zoom_), header_color(d != nullptr ? d->category : ""), round,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRect(iv(a), iv(b),
                has_error ? IM_COL32(255, 80, 70, 255) : selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(15, 15, 18, 255),
                round, 0, has_error || selected ? 2.5f * zoom_ : 1.0f);
    const std::string title = d != nullptr ? d->title : "Unknown: " + n->type;
    dl->AddText(ImVec2(a.x + 8.0f * zoom_, a.y + (kHeader * zoom_ - ImGui::GetFontSize()) * 0.5f), IM_COL32_WHITE,
                title.c_str());
    const ImVec2 mouse = ImGui::GetMousePos();
    if (mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= a.y + kHeader * zoom_ && !ImGui::IsAnyItemActive() &&
        ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s%s%s", d != nullptr ? d->help.c_str() : "", has_error ? "\n\n" : "", error_text.c_str());
    }
    if (d == nullptr) {
        return;
    }
    ImGui::PushID(static_cast<int>(id));
    f32 row = 0.0f;
    auto row_y = [&](f32 r) { return a.y + (kHeader + (r + 0.5f) * kRow) * zoom_; };
    if (d->uses_variable) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 10.0f * zoom_, row_y(0.0f) - ImGui::GetFrameHeight() * 0.5f));
        ImGui::SetNextItemWidth((size.x - 20.0f) * zoom_);
        if (ImGui::BeginCombo("##var", n->variable.empty() ? "(variable)" : n->variable.c_str())) {
            for (const GraphVariable& v : graph_.variables) {
                if (ImGui::Selectable(v.name.c_str(), v.name == n->variable)) {
                    snapshot();
                    n->variable = v.name;
                    changed();
                }
            }
            if (graph_.variables.empty()) {
                ImGui::TextDisabled("add one in the Variables list");
            }
            ImGui::EndCombo();
        }
        row = 1.0f;
    }
    // Inputs: pin, label, literal widget when unconnected.
    for (usize i = 0; i < d->inputs.size(); ++i) {
        const PinDef& p = d->inputs[i];
        const PinType t = graph_.pin_type(*n, p.name, false);
        const Vec2    pp(a.x, row_y(row + static_cast<f32>(i)));
        const bool    linked = graph_.input_link(id, p.name) != nullptr ||
                            (t == PinType::Exec && std::any_of(graph_.links.begin(), graph_.links.end(), [&](const GraphLink& l) {
                                 return l.to_node == id && l.to_pin == p.name;
                             }));
        if (t == PinType::Exec) {
            const f32 s = kPinRadius * zoom_;
            dl->AddTriangleFilled(ImVec2(pp.x - s * 0.6f, pp.y - s), ImVec2(pp.x - s * 0.6f, pp.y + s), ImVec2(pp.x + s, pp.y),
                                  linked ? pin_color(t) : IM_COL32(120, 120, 120, 255));
        } else if (linked) {
            dl->AddCircleFilled(iv(pp), kPinRadius * zoom_, pin_color(t));
        } else {
            dl->AddCircle(iv(pp), kPinRadius * zoom_, pin_color(t), 0, 1.5f * zoom_);
        }
        if (t == PinType::Exec) {
            continue;
        }
        const ImVec2 label_pos(pp.x + 10.0f * zoom_, pp.y - ImGui::GetFontSize() * 0.5f);
        dl->AddText(label_pos, IM_COL32(210, 210, 215, 255), p.name.c_str());
        if (linked || t == PinType::Entity) {
            if (t == PinType::Entity && !linked) {
                dl->AddText(ImVec2(label_pos.x + ImGui::CalcTextSize(p.name.c_str()).x + 6.0f * zoom_, label_pos.y),
                            IM_COL32(130, 130, 140, 255), "(Self)");
            }
            continue;
        }
        const f32 x = label_pos.x + ImGui::CalcTextSize(p.name.c_str()).x + 6.0f * zoom_;
        ImGui::SetCursorScreenPos(ImVec2(x, pp.y - ImGui::GetFrameHeight() * 0.5f));
        GraphValue& v = n->values[p.name];
        const f32   w = std::max(40.0f * zoom_, b.x - x - 16.0f * zoom_);
        bool        edited = false;
        ImGui::PushID(p.name.c_str());
        if (t == PinType::Bool) {
            bool bv = std::holds_alternative<bool>(v) && std::get<bool>(v);
            if (ImGui::Checkbox("##v", &bv)) {
                v = bv;
                edited = true;
            }
        } else if (t == PinType::Number) {
            f32 f = std::holds_alternative<f64>(v) ? static_cast<f32>(std::get<f64>(v)) : 0.0f;
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat("##v", &f, 0.05f)) {
                v = static_cast<f64>(f);
                edited = true;
            }
        } else if (t == PinType::Vector) {
            Vec3 x3 = std::holds_alternative<Vec3>(v) ? std::get<Vec3>(v) : Vec3(0.0f);
            ImGui::SetNextItemWidth(w);
            if (ImGui::DragFloat3("##v", &x3.x, 0.05f)) {
                v = x3;
                edited = true;
            }
        } else {
            std::string s = std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string();
            if (input_string("##v", s, w)) {
                v = s;
                edited = true;
            }
        }
        if (ImGui::IsItemActivated()) {
            snapshot();
        }
        if (edited) {
            changed();
        }
        ImGui::PopID();
    }
    // Outputs: label right-aligned, pin on the right edge.
    for (usize i = 0; i < d->outputs.size(); ++i) {
        const PinDef& p = d->outputs[i];
        const PinType t = graph_.pin_type(*n, p.name, true);
        const Vec2    pp(b.x, row_y(row + static_cast<f32>(i)));
        const bool    linked = std::any_of(graph_.links.begin(), graph_.links.end(),
                                           [&](const GraphLink& l) { return l.from_node == id && l.from_pin == p.name; });
        if (t == PinType::Exec) {
            const f32 s = kPinRadius * zoom_;
            dl->AddTriangleFilled(ImVec2(pp.x - s * 0.6f, pp.y - s), ImVec2(pp.x - s * 0.6f, pp.y + s), ImVec2(pp.x + s, pp.y),
                                  linked ? pin_color(t) : IM_COL32(120, 120, 120, 255));
        } else if (linked) {
            dl->AddCircleFilled(iv(pp), kPinRadius * zoom_, pin_color(t));
        } else {
            dl->AddCircle(iv(pp), kPinRadius * zoom_, pin_color(t), 0, 1.5f * zoom_);
        }
        if (t != PinType::Exec || p.name != "then" || d->outputs.size() > 1) {
            const ImVec2 ts = ImGui::CalcTextSize(p.name.c_str());
            dl->AddText(ImVec2(pp.x - ts.x - 10.0f * zoom_, pp.y - ts.y * 0.5f), IM_COL32(210, 210, 215, 255), p.name.c_str());
        }
    }
    ImGui::PopID();
}

void GraphEditor::draw_canvas() {
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const Vec2   origin = vv(ImGui::GetCursorScreenPos());
    const Vec2   canvas_size = vv(ImGui::GetContentRegionAvail());
    const Vec2   mouse = vv(ImGui::GetMousePos());
    ImGuiIO&     io = ImGui::GetIO();
    const bool   hovered = ImGui::IsWindowHovered();

    if (focus_node_ != 0) {
        if (const GraphNode* n = graph_.find(focus_node_)) {
            scroll_ = -n->position + canvas_size / (2.0f * zoom_) - node_size(*n) * 0.5f;
        }
        focus_node_ = 0;
    }

    // Grid.
    const f32 grid = 32.0f * zoom_;
    dl->AddRectFilled(iv(origin), iv(origin + canvas_size), IM_COL32(22, 23, 27, 255));
    for (f32 x = std::fmod(scroll_.x * zoom_, grid); x < canvas_size.x; x += grid) {
        dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + canvas_size.y), IM_COL32(40, 42, 48, 255));
    }
    for (f32 y = std::fmod(scroll_.y * zoom_, grid); y < canvas_size.y; y += grid) {
        dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + canvas_size.x, origin.y + y), IM_COL32(40, 42, 48, 255));
    }
    if (graph_.nodes.empty()) {
        dl->AddText(iv(origin + Vec2(16.0f)), IM_COL32(150, 150, 160, 255),
                    "Right-click to add nodes; drag empty space to move around. Start from an event (On Start / On Update), then wire actions.");
    }

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * zoom_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * zoom_, 1.0f * zoom_));

    // Wires (behind the nodes).
    auto bezier = [&](const Vec2& p0, const Vec2& p1, ImU32 col, f32 thick) {
        const f32 dx = std::max(std::abs(p1.x - p0.x) * 0.5f, 40.0f * zoom_);
        dl->AddBezierCubic(iv(p0), ImVec2(p0.x + dx, p0.y), ImVec2(p1.x - dx, p1.y), iv(p1), col, thick);
    };
    for (const GraphLink& l : graph_.links) {
        const GraphNode* from = graph_.find(l.from_node);
        const GraphNode* to = graph_.find(l.to_node);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        const PinType t = graph_.pin_type(*from, l.from_pin, true);
        bezier(pin_position(*from, l.from_pin, true, origin), pin_position(*to, l.to_pin, false, origin), pin_color(t),
               (t == PinType::Exec ? 3.0f : 2.2f) * zoom_);
    }
    // Nodes (copy the ids: drawing may edit the graph).
    std::vector<u32> ids;
    for (const GraphNode& n : graph_.nodes) {
        ids.push_back(n.id);
    }
    for (u32 id : ids) {
        draw_node(id, origin);
    }
    // Wire being dragged.
    if (drag_) {
        if (const GraphNode* n = graph_.find(drag_.node)) {
            const Vec2    p = pin_position(*n, drag_.pin, drag_.output, origin);
            const PinType t = graph_.pin_type(*n, drag_.pin, drag_.output);
            drag_.output ? bezier(p, mouse, pin_color(t), 2.5f * zoom_) : bezier(mouse, p, pin_color(t), 2.5f * zoom_);
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();

    // ---- interaction (after the widgets, so their hover / active state is known) ----
    const bool widget = ImGui::IsAnyItemActive() || ImGui::IsAnyItemHovered();
    if (hovered && !widget && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (PinRef pin = pin_at(mouse, origin)) {
            if (io.KeyAlt) { // clear every wire of the pin
                snapshot();
                std::erase_if(graph_.links, [&](const GraphLink& l) {
                    return pin.output ? (l.from_node == pin.node && l.from_pin == pin.pin)
                                      : (l.to_node == pin.node && l.to_pin == pin.pin);
                });
                changed();
            } else if (!pin.output && graph_.pin_type(*graph_.find(pin.node), pin.pin, false) != PinType::Exec) {
                if (const GraphLink* l = graph_.input_link(pin.node, pin.pin)) { // pick the wire up
                    snapshot();
                    drag_ = PinRef{ l->from_node, l->from_pin, true };
                    graph_.disconnect_input(pin.node, pin.pin);
                    changed();
                } else {
                    drag_ = pin;
                }
            } else {
                drag_ = pin;
            }
        } else if (const u32 id = node_at(mouse, origin)) {
            if (io.KeyCtrl) {
                selection_.contains(id) ? (void)selection_.erase(id) : (void)selection_.insert(id);
            } else if (!selection_.contains(id)) {
                selection_ = { id };
            }
            moving_ = true;
            undo_.push_back(graph_to_json(graph_)); // dropped again below if nothing moved
            redo_.clear();
        } else {
            selection_.clear();
            panning_left_ = true; // dragging empty canvas scrolls the view (a click just deselects)
        }
    }
    if (panning_left_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            scroll_ += vv(io.MouseDelta) / zoom_;
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        } else {
            panning_left_ = false;
        }
    }
    if (moving_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
                for (u32 id : selection_) {
                    if (GraphNode* n = graph_.find(id)) {
                        n->position += vv(io.MouseDelta) / zoom_;
                    }
                }
                changed();
            }
        } else {
            moving_ = false;
            if (!undo_.empty() && undo_.back() == graph_to_json(graph_)) {
                undo_.pop_back(); // a click without a move is not an edit
            }
        }
    }
    if (drag_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const PinRef target = pin_at(mouse, origin);
        if (target && target.output != drag_.output) {
            drag_.output ? connect(drag_.node, drag_.pin, target.node, target.pin)
                         : connect(target.node, target.pin, drag_.node, drag_.pin);
        } else if (!target && hovered) { // dropped on empty space: create a node and wire it
            open_menu_ = true;
            menu_connect_ = drag_;
            menu_canvas_pos_ = to_canvas(mouse, origin);
        }
        drag_ = {};
    }
    // Pan (right / middle drag), context menu (right click), zoom (wheel).
    if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        panning_ = true;
    }
    if (panning_) {
        scroll_ += vv(io.MouseDelta) / zoom_;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            panning_ = false;
        }
    } else if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        open_menu_ = true;
        menu_connect_ = {};
        menu_canvas_pos_ = to_canvas(mouse, origin);
    }
    if (hovered && io.MouseWheel != 0.0f && !widget) {
        const Vec2 before = to_canvas(mouse, origin);
        // Fixed zoom steps: each distinct text size bakes new glyphs into the UI font atlas, so a
        // handful of sizes keeps the atlas stable (continuous zoom rebuilt it on every wheel step).
        static constexpr f32 kZoomSteps[] = { 0.5f, 0.625f, 0.75f, 0.875f, 1.0f, 1.125f, 1.25f, 1.5f };
        constexpr int        kCount = static_cast<int>(std::size(kZoomSteps));
        int                  cur = 0;
        for (int k = 0; k < kCount; ++k) {
            if (std::abs(kZoomSteps[k] - zoom_) < std::abs(kZoomSteps[cur] - zoom_)) {
                cur = k;
            }
        }
        cur = std::clamp(cur + (io.MouseWheel > 0.0f ? 1 : -1), 0, kCount - 1);
        zoom_ = kZoomSteps[cur];
        scroll_ += to_canvas(mouse, origin) - before; // keep the point under the cursor
    }
    draw_create_menu();
}

void GraphEditor::draw_create_menu() {
    if (open_menu_) {
        ImGui::OpenPopup("##create");
        search_[0] = '\0';
        open_menu_ = false;
    }
    if (!ImGui::BeginPopup("##create")) {
        return;
    }
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##search", "search nodes", search_, sizeof(search_));
    const GraphNode* from = menu_connect_ ? graph_.find(menu_connect_.node) : nullptr;
    const PinType    want = from != nullptr ? graph_.pin_type(*from, menu_connect_.pin, menu_connect_.output) : PinType::Exec;
    // A node fits the dropped wire if it has a compatible pin on the other side.
    auto fits = [&](const NodeDef& d, std::string* pin_name) {
        if (from == nullptr) {
            return true;
        }
        for (const PinDef& p : menu_connect_.output ? d.inputs : d.outputs) {
            const bool ok = menu_connect_.output ? pins_compatible(want, p.type) : pins_compatible(p.type, want);
            if (ok) {
                if (pin_name != nullptr) {
                    *pin_name = p.name;
                }
                return true;
            }
        }
        return false;
    };
    std::string category;
    int         shown = 0;
    ImGui::BeginChild("##list", ImVec2(260.0f, 320.0f));
    for (const NodeDef& d : node_library()) {
        if (!contains_ci(d.title, search_) && !contains_ci(d.category, search_)) {
            continue;
        }
        if (!fits(d, nullptr)) {
            continue;
        }
        if (d.category != category) {
            category = d.category;
            ImGui::SeparatorText(category.c_str());
        }
        ++shown;
        if (ImGui::Selectable(d.title.c_str())) {
            const u32 id = add_node(d.type, menu_canvas_pos_);
            std::string pin;
            if (id != 0 && from != nullptr && fits(d, &pin)) {
                menu_connect_.output ? graph_.connect(menu_connect_.node, menu_connect_.pin, id, pin)
                                     : graph_.connect(id, pin, menu_connect_.node, menu_connect_.pin);
                changed(); // part of the same undo step as the new node
            }
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.help.c_str());
        }
    }
    if (shown == 0) {
        ImGui::TextDisabled("no matching node");
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

} // namespace aether::editor
