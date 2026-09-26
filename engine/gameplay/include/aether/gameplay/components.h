// aether/gameplay/components.h — ECS components owned by the gameplay layer.
//
// MaterialOverridesComponent complements the frozen MeshRendererComponent (one material) for
// meshes with several material slots (glTF primitives): slot i draws with materials[i] when that
// id is valid, else with MeshRendererComponent::material, else with the default material.
// Serialized by register_gameplay_codecs() (component_codecs.h) as "MaterialOverrides".
//
// Thread-affinity: plain data; follow the World's rules (main thread for mutation).
#pragma once

#include "aether/core/handle.h"
#include "aether/core/reflect.h"
#include "aether/core/types.h"

#include <vector>

namespace aether::gameplay {

struct MaterialOverridesComponent {
    std::vector<AssetId> materials; // indexed by Submesh::material_slot

    [[nodiscard]] AssetId slot(u32 material_slot) const {
        return material_slot < materials.size() ? materials[material_slot] : AssetId{};
    }

    AE_REFLECT(MaterialOverridesComponent, AE_FIELD(materials))
};

} // namespace aether::gameplay
