// input_map.cpp — InputMap (actions/axes, JSON), key names, and the InputSubsystem.
#include "aether/gameplay/input_map.h"

#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/gameplay/input_subsystem.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace aether::gameplay {

namespace {

using json = nlohmann::json;

struct KeyNameEntry {
    Key        key;
    StringView name;
};

// Every named Key (letters/digits/F-keys are generated below).
constexpr std::array kKeyNames{
    KeyNameEntry{ Key::Space, "Space" },           KeyNameEntry{ Key::Apostrophe, "Apostrophe" },
    KeyNameEntry{ Key::Comma, "Comma" },           KeyNameEntry{ Key::Minus, "Minus" },
    KeyNameEntry{ Key::Period, "Period" },         KeyNameEntry{ Key::Slash, "Slash" },
    KeyNameEntry{ Key::Semicolon, "Semicolon" },   KeyNameEntry{ Key::Equal, "Equal" },
    KeyNameEntry{ Key::LeftBracket, "LeftBracket" }, KeyNameEntry{ Key::Backslash, "Backslash" },
    KeyNameEntry{ Key::RightBracket, "RightBracket" }, KeyNameEntry{ Key::GraveAccent, "GraveAccent" },
    KeyNameEntry{ Key::Escape, "Escape" },         KeyNameEntry{ Key::Enter, "Enter" },
    KeyNameEntry{ Key::Tab, "Tab" },               KeyNameEntry{ Key::Backspace, "Backspace" },
    KeyNameEntry{ Key::Insert, "Insert" },         KeyNameEntry{ Key::Delete, "Delete" },
    KeyNameEntry{ Key::Right, "Right" },           KeyNameEntry{ Key::Left, "Left" },
    KeyNameEntry{ Key::Down, "Down" },             KeyNameEntry{ Key::Up, "Up" },
    KeyNameEntry{ Key::PageUp, "PageUp" },         KeyNameEntry{ Key::PageDown, "PageDown" },
    KeyNameEntry{ Key::Home, "Home" },             KeyNameEntry{ Key::End, "End" },
    KeyNameEntry{ Key::CapsLock, "CapsLock" },     KeyNameEntry{ Key::ScrollLock, "ScrollLock" },
    KeyNameEntry{ Key::NumLock, "NumLock" },       KeyNameEntry{ Key::PrintScreen, "PrintScreen" },
    KeyNameEntry{ Key::Pause, "Pause" },           KeyNameEntry{ Key::LeftShift, "LeftShift" },
    KeyNameEntry{ Key::LeftControl, "LeftControl" }, KeyNameEntry{ Key::LeftAlt, "LeftAlt" },
    KeyNameEntry{ Key::LeftSuper, "LeftSuper" },   KeyNameEntry{ Key::RightShift, "RightShift" },
    KeyNameEntry{ Key::RightControl, "RightControl" }, KeyNameEntry{ Key::RightAlt, "RightAlt" },
    KeyNameEntry{ Key::RightSuper, "RightSuper" },
};

constexpr std::array<StringView, 26> kLetters{ "A", "B", "C", "D", "E", "F", "G", "H", "I",
                                               "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                               "S", "T", "U", "V", "W", "X", "Y", "Z" };
constexpr std::array<StringView, 10> kDigits{ "Num0", "Num1", "Num2", "Num3", "Num4",
                                              "Num5", "Num6", "Num7", "Num8", "Num9" };
constexpr std::array<StringView, 12> kFKeys{ "F1", "F2", "F3", "F4",  "F5",  "F6",
                                             "F7", "F8", "F9", "F10", "F11", "F12" };

struct ButtonName {
    MouseButton button;
    StringView  name;
};
constexpr std::array kButtonNames{
    ButtonName{ MouseButton::Left, "Left" },       ButtonName{ MouseButton::Right, "Right" },
    ButtonName{ MouseButton::Middle, "Middle" },   ButtonName{ MouseButton::Button4, "Button4" },
    ButtonName{ MouseButton::Button5, "Button5" },
};

bool iequals(StringView a, StringView b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool key_down_in(const InputState& s, Key k) {
    const usize i = static_cast<usize>(k);
    return i < static_cast<usize>(Key::Count) &&
           (s.keys[i] == ButtonState::Pressed || s.keys[i] == ButtonState::Held);
}

bool button_down_in(const InputState& s, MouseButton b) {
    const usize i = static_cast<usize>(b);
    return i < static_cast<usize>(MouseButton::Count) &&
           (s.mouse[i] == ButtonState::Pressed || s.mouse[i] == ButtonState::Held);
}

bool binding_down(const InputState& s, const InputBinding& b) {
    switch (b.kind) {
    case InputSourceKind::Key: return key_down_in(s, b.key);
    case InputSourceKind::MouseButton: return button_down_in(s, b.button);
    default: return false;
    }
}

f32 binding_value(const InputState& s, const InputBinding& b) {
    switch (b.kind) {
    case InputSourceKind::Key: return key_down_in(s, b.key) ? b.scale : 0.0f;
    case InputSourceKind::MouseButton: return button_down_in(s, b.button) ? b.scale : 0.0f;
    case InputSourceKind::MouseDeltaX: return s.cursor_delta.x * b.scale;
    case InputSourceKind::MouseDeltaY: return s.cursor_delta.y * b.scale;
    case InputSourceKind::ScrollX: return s.scroll.x * b.scale;
    case InputSourceKind::ScrollY: return s.scroll.y * b.scale;
    }
    return 0.0f;
}

void add_unique(std::vector<InputBinding>& list, const InputBinding& b) {
    if (std::find(list.begin(), list.end(), b) == list.end()) {
        list.push_back(b);
    }
}

} // namespace

// =================================================================================================
// names
// =================================================================================================
StringView key_name(Key key) {
    const auto k = static_cast<u16>(key);
    if (k >= static_cast<u16>(Key::A) && k <= static_cast<u16>(Key::Z)) {
        return kLetters[k - static_cast<u16>(Key::A)];
    }
    if (k >= static_cast<u16>(Key::Num0) && k <= static_cast<u16>(Key::Num9)) {
        return kDigits[k - static_cast<u16>(Key::Num0)];
    }
    if (k >= static_cast<u16>(Key::F1) && k <= static_cast<u16>(Key::F12)) {
        return kFKeys[k - static_cast<u16>(Key::F1)];
    }
    for (const KeyNameEntry& e : kKeyNames) {
        if (e.key == key) {
            return e.name;
        }
    }
    return "Unknown";
}

std::optional<Key> key_from_name(StringView name) {
    for (usize i = 0; i < kLetters.size(); ++i) {
        if (iequals(name, kLetters[i])) {
            return static_cast<Key>(static_cast<u16>(Key::A) + i);
        }
    }
    for (usize i = 0; i < kDigits.size(); ++i) {
        if (iequals(name, kDigits[i]) || (name.size() == 1 && name[0] == static_cast<char>('0' + i))) {
            return static_cast<Key>(static_cast<u16>(Key::Num0) + i);
        }
    }
    for (usize i = 0; i < kFKeys.size(); ++i) {
        if (iequals(name, kFKeys[i])) {
            return static_cast<Key>(static_cast<u16>(Key::F1) + i);
        }
    }
    for (const KeyNameEntry& e : kKeyNames) {
        if (iequals(name, e.name)) {
            return e.key;
        }
    }
    return std::nullopt;
}

std::string binding_input_name(const InputBinding& b) {
    switch (b.kind) {
    case InputSourceKind::Key: return std::string(key_name(b.key));
    case InputSourceKind::MouseButton:
        for (const ButtonName& n : kButtonNames) {
            if (n.button == b.button) {
                return "Mouse:" + std::string(n.name);
            }
        }
        return "Mouse:Left";
    case InputSourceKind::MouseDeltaX: return "MouseDeltaX";
    case InputSourceKind::MouseDeltaY: return "MouseDeltaY";
    case InputSourceKind::ScrollX: return "ScrollX";
    case InputSourceKind::ScrollY: return "ScrollY";
    }
    return "Unknown";
}

std::optional<InputBinding> binding_from_input_name(StringView name, f32 scale) {
    constexpr StringView kMousePrefix = "mouse:";
    if (name.size() > kMousePrefix.size() && iequals(name.substr(0, kMousePrefix.size()), kMousePrefix)) {
        const StringView button = name.substr(kMousePrefix.size());
        for (const ButtonName& n : kButtonNames) {
            if (iequals(button, n.name)) {
                return InputBinding::from_mouse_button(n.button, scale);
            }
        }
        return std::nullopt;
    }
    if (iequals(name, "MouseDeltaX")) {
        return InputBinding::from_source(InputSourceKind::MouseDeltaX, scale);
    }
    if (iequals(name, "MouseDeltaY")) {
        return InputBinding::from_source(InputSourceKind::MouseDeltaY, scale);
    }
    if (iequals(name, "ScrollX")) {
        return InputBinding::from_source(InputSourceKind::ScrollX, scale);
    }
    if (iequals(name, "ScrollY")) {
        return InputBinding::from_source(InputSourceKind::ScrollY, scale);
    }
    if (const auto k = key_from_name(name); k && *k != Key::Unknown) {
        return InputBinding::from_key(*k, scale);
    }
    return std::nullopt;
}

// =================================================================================================
// InputMap
// =================================================================================================
InputMap::Entry* InputMap::find(std::vector<Entry>& list, StringView name) {
    for (Entry& e : list) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const InputMap::Entry* InputMap::find(const std::vector<Entry>& list, StringView name) const {
    for (const Entry& e : list) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

InputMap::Entry& InputMap::find_or_add(std::vector<Entry>& list, StringView name) {
    if (Entry* e = find(list, name)) {
        return *e;
    }
    list.push_back(Entry{ std::string(name), {}, false, false, 0.0f });
    return list.back();
}

void InputMap::bind_action(StringView action, Key key) { bind_action(action, InputBinding::from_key(key)); }
void InputMap::bind_action(StringView action, MouseButton button) {
    bind_action(action, InputBinding::from_mouse_button(button));
}
void InputMap::bind_action(StringView action, const InputBinding& binding) {
    InputBinding b = binding;
    b.scale        = 1.0f; // actions are digital
    add_unique(find_or_add(actions_, action).bindings, b);
}

void InputMap::bind_axis(StringView axis, Key key, f32 scale) { bind_axis(axis, InputBinding::from_key(key, scale)); }
void InputMap::bind_axis(StringView axis, MouseButton button, f32 scale) {
    bind_axis(axis, InputBinding::from_mouse_button(button, scale));
}
void InputMap::bind_axis(StringView axis, InputSourceKind source, f32 scale) {
    bind_axis(axis, InputBinding::from_source(source, scale));
}
void InputMap::bind_axis(StringView axis, const InputBinding& binding) {
    add_unique(find_or_add(axes_, axis).bindings, binding);
}

bool InputMap::unbind(StringView name) {
    const auto erase = [&](std::vector<Entry>& list) {
        return std::erase_if(list, [&](const Entry& e) { return e.name == name; }) > 0;
    };
    const bool a = erase(actions_);
    const bool b = erase(axes_);
    return a || b;
}

void InputMap::clear() {
    actions_.clear();
    axes_.clear();
}

bool InputMap::has_action(StringView action) const { return find(actions_, action) != nullptr; }
bool InputMap::has_axis(StringView axis) const { return find(axes_, axis) != nullptr; }

const std::vector<InputBinding>* InputMap::action_bindings(StringView action) const {
    const Entry* e = find(actions_, action);
    return e != nullptr ? &e->bindings : nullptr;
}
const std::vector<InputBinding>* InputMap::axis_bindings(StringView axis) const {
    const Entry* e = find(axes_, axis);
    return e != nullptr ? &e->bindings : nullptr;
}

std::vector<std::string> InputMap::action_names() const {
    std::vector<std::string> out;
    for (const Entry& e : actions_) {
        out.push_back(e.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> InputMap::axis_names() const {
    std::vector<std::string> out;
    for (const Entry& e : axes_) {
        out.push_back(e.name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void InputMap::update(const InputState& state) {
    for (Entry& a : actions_) {
        a.prev_down = a.down;
        a.down      = std::any_of(a.bindings.begin(), a.bindings.end(),
                                  [&](const InputBinding& b) { return binding_down(state, b); });
    }
    for (Entry& x : axes_) {
        f32 sum = 0.0f;
        for (const InputBinding& b : x.bindings) {
            sum += binding_value(state, b);
        }
        x.value = sum;
    }
}

void InputMap::reset_state() {
    for (Entry& a : actions_) {
        a.down = false; // prev_down keeps the old value: released() fires on the next update
    }
    for (Entry& x : axes_) {
        x.value = 0.0f;
    }
}

bool InputMap::action_down(StringView action) const {
    const Entry* e = find(actions_, action);
    return e != nullptr && e->down;
}
bool InputMap::action_pressed(StringView action) const {
    const Entry* e = find(actions_, action);
    return e != nullptr && e->down && !e->prev_down;
}
bool InputMap::action_released(StringView action) const {
    const Entry* e = find(actions_, action);
    return e != nullptr && !e->down && e->prev_down;
}
f32 InputMap::axis(StringView axis) const {
    const Entry* e = find(axes_, axis);
    return e != nullptr ? e->value : 0.0f;
}

Result<usize> InputMap::load_json(StringView text) {
    const json doc = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return Error{ ErrorCode::InvalidArgument, "input map: expected a JSON object" };
    }
    const auto actions = doc.find("actions");
    const auto axes    = doc.find("axes");
    if ((actions != doc.end() && !actions->is_object()) || (axes != doc.end() && !axes->is_object())) {
        return Error{ ErrorCode::InvalidArgument, "input map: 'actions' and 'axes' must be objects" };
    }

    InputMap next;
    usize    skipped = 0;
    if (actions != doc.end()) {
        for (const auto& [name, list] : actions->items()) {
            if (!list.is_array()) {
                return Error{ ErrorCode::InvalidArgument, "input map: action '" + name + "' must be an array" };
            }
            next.find_or_add(next.actions_, name);
            for (const json& input : list) {
                const auto b = input.is_string() ? binding_from_input_name(input.get<std::string>()) : std::nullopt;
                if (!b) {
                    AE_LOG_WARN("Input", "input map: action '{}': unknown input {}", name, input.dump());
                    ++skipped;
                    continue;
                }
                next.bind_action(name, *b);
            }
        }
    }
    if (axes != doc.end()) {
        for (const auto& [name, list] : axes->items()) {
            if (!list.is_array()) {
                return Error{ ErrorCode::InvalidArgument, "input map: axis '" + name + "' must be an array" };
            }
            next.find_or_add(next.axes_, name);
            for (const json& entry : list) {
                std::string input;
                f32         scale = 1.0f;
                if (entry.is_string()) {
                    input = entry.get<std::string>();
                } else if (entry.is_object() && entry.contains("input") && entry["input"].is_string()) {
                    input = entry["input"].get<std::string>();
                    if (const auto s = entry.find("scale"); s != entry.end() && s->is_number()) {
                        scale = s->get<f32>();
                    }
                }
                const auto b = binding_from_input_name(input, scale);
                if (!b) {
                    AE_LOG_WARN("Input", "input map: axis '{}': unknown input {}", name, entry.dump());
                    ++skipped;
                    continue;
                }
                next.bind_axis(name, *b);
            }
        }
    }
    *this = std::move(next);
    return skipped;
}

Result<usize> InputMap::load_file(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return Error{ ErrorCode::NotFound, "input map not found: " + file.generic_string() };
    }
    return load_json(paths::read_text_file(file));
}

std::string InputMap::to_json() const {
    json doc   = json::object();
    json acts  = json::object();
    json axes  = json::object();
    for (const Entry& a : actions_) {
        json list = json::array();
        for (const InputBinding& b : a.bindings) {
            list.push_back(binding_input_name(b));
        }
        acts[a.name] = std::move(list);
    }
    for (const Entry& x : axes_) {
        json list = json::array();
        for (const InputBinding& b : x.bindings) {
            list.push_back(json{ { "input", binding_input_name(b) }, { "scale", b.scale } });
        }
        axes[x.name] = std::move(list);
    }
    doc["actions"] = std::move(acts);
    doc["axes"]    = std::move(axes);
    return doc.dump(2);
}

void InputMap::add_default_camera_bindings() {
    bind_axis("Camera.MoveForward", Key::W, 1.0f);
    bind_axis("Camera.MoveForward", Key::S, -1.0f);
    bind_axis("Camera.MoveForward", Key::Up, 1.0f);
    bind_axis("Camera.MoveForward", Key::Down, -1.0f);
    bind_axis("Camera.MoveRight", Key::D, 1.0f);
    bind_axis("Camera.MoveRight", Key::A, -1.0f);
    bind_axis("Camera.MoveUp", Key::E, 1.0f);
    bind_axis("Camera.MoveUp", Key::Q, -1.0f);
    bind_axis("Camera.LookX", InputSourceKind::MouseDeltaX, 1.0f);
    bind_axis("Camera.LookY", InputSourceKind::MouseDeltaY, 1.0f);
    bind_axis("Camera.Zoom", InputSourceKind::ScrollY, 1.0f);
    bind_action("Camera.Look", MouseButton::Right);
    bind_action("Camera.Pan", MouseButton::Middle);
    bind_action("Camera.Boost", Key::LeftShift);
}

// =================================================================================================
// InputSubsystem
// =================================================================================================
InputSubsystem::InputSubsystem() { map_.add_default_camera_bindings(); }

void InputSubsystem::on_begin_frame(FrameContext& ctx) {
    const bool has_window = ctx.engine != nullptr && ctx.engine->window != nullptr;
    const InputState raw  = has_window ? Input::state() : InputState{};
    bool wants_keyboard   = false;
    bool wants_mouse      = false;
    if (ImGui::GetCurrentContext() != nullptr) {
        const ImGuiIO& io = ImGui::GetIO();
        wants_keyboard    = io.WantCaptureKeyboard || io.WantTextInput;
        wants_mouse       = io.WantCaptureMouse;
    }
    feed(raw, wants_keyboard, wants_mouse);
}

void InputSubsystem::feed(const InputState& raw, bool imgui_wants_keyboard, bool imgui_wants_mouse) {
    state_ = raw;
    if (respect_imgui_ && imgui_wants_keyboard) {
        std::fill(std::begin(state_.keys), std::end(state_.keys), ButtonState::Up);
    }
    if (respect_imgui_ && imgui_wants_mouse) {
        std::fill(std::begin(state_.mouse), std::end(state_.mouse), ButtonState::Up);
        state_.cursor_delta = Vec2(0.0f);
        state_.scroll       = Vec2(0.0f);
    }
    map_.update(state_);
}

} // namespace aether::gameplay
