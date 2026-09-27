// aether/editor/material_instance.h — per-entity material instances edited in the editor.
//
// There is no material asset file format yet, so the material editor stores the edited
// parameters IN THE SCENE: MaterialInstanceComponent holds full assets::MaterialData blocks per
// material slot (slot -1 = MeshRendererComponent::material, slot >= 0 =
// MaterialOverridesComponent::materials[slot], i.e. Submesh::material_slot). Because the data is
// scene state, every edit is covered by snapshot undo/redo and saved with the scene (codec
// "EditorMaterialInstance", registered by register_editor_codecs()).
//
// Each slot renders through a runtime material registered with the RenderResourceCache under a
// deterministic id derived from (entity uuid, slot): material_instance_id(). bind_material_instances()
// points every slot's target at its id (entities duplicated / instantiated get fresh uuids, so
// their references are re-derived); the editor re-registers a slot's material whenever its
// material_data_hash() changes (live preview, undo, load).
//
// Main thread only.
#pragma once

#include "aether/assets/asset_types.h"
#include "aether/core/error.h"
#include "aether/core/handle.h"
#include "aether/core/types.h"
#include "aether/scene/entity.h"

#include <string>
#include <vector>

namespace aether {
class World;
}

namespace aether::editor {

inline constexpr StringView kMaterialInstanceCodecName = "EditorMaterialInstance";
inline constexpr i32        kPrimaryMaterialSlot       = -1; // MeshRendererComponent::material

struct MaterialInstanceComponent {
    struct Slot {
        i32                  slot = kPrimaryMaterialSlot;
        assets::MaterialData data;
    };
    std::vector<Slot> slots;

    [[nodiscard]] Slot*       find(i32 slot) noexcept;
    [[nodiscard]] const Slot* find(i32 slot) const noexcept;
};

// Deterministic runtime asset id of an instance slot (never collides with builtin / database ids
// in practice: the hi word carries a fixed tag).
[[nodiscard]] AssetId material_instance_id(u64 entity_uuid, i32 slot);
[[nodiscard]] bool    is_material_instance_id(const AssetId& id);
[[nodiscard]] u64     material_data_hash(const assets::MaterialData& data);

// Parses the 32-hex-digit form written by AssetId::to_string() (hi then lo). Empty -> invalid id.
[[nodiscard]] bool parse_asset_id(StringView text, AssetId& out);

[[nodiscard]] std::string                  material_to_json(const assets::MaterialData& data);
[[nodiscard]] Result<assets::MaterialData> material_from_json(StringView json);

// The id a slot currently references (MeshRenderer material or override slot; invalid if none).
[[nodiscard]] AssetId material_slot_target(const World& world, Entity e, i32 slot);
// Sets what a slot references (grows MaterialOverridesComponent as needed; adds it if missing).
void set_material_slot_target(World& world, Entity e, i32 slot, const AssetId& material);

// Points every instance slot's target at its derived id. Returns the number of references changed.
usize bind_material_instances(World& world);

// Creates (or replaces) an instance slot on `e` initialised with `data` and binds it.
void make_material_instance(World& world, Entity e, i32 slot, const assets::MaterialData& data);
// Removes an instance slot; its target falls back to `replacement` (e.g. the default material).
// The component is removed when its last slot goes.
void remove_material_instance(World& world, Entity e, i32 slot, const AssetId& replacement);

bool register_material_instance_codec(World& world); // called by register_editor_codecs()

} // namespace aether::editor
