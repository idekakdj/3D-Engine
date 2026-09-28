// aether/gameplay/components.h — ECS components owned by the gameplay layer.
//
// MaterialOverridesComponent complements the frozen MeshRendererComponent (one material) for
// meshes with several material slots (glTF primitives): slot i draws with materials[i] when that
// id is valid, else with MeshRendererComponent::material, else with the default material.
// Serialized by register_gameplay_codecs() (component_codecs.h) as "MaterialOverrides".
//
// GIVolumeComponent (ADR-0016) turns on dynamic global illumination inside a box: the entity's
// world position is the box centre and its world scale the box size (rotation is ignored - the
// volume is axis-aligned). Probes are placed every `probe_spacing` metres (at least 2 per axis).
// The first visible, enabled volume is used. Serialized as "GIVolume".
//
// ReflectionProbeComponent (ADR-0017) is a reflection capture: a cubemap of the scene taken at the
// entity's position, used for reflections of surfaces inside the box (centre = position, size =
// world scale, axis-aligned). Up to 8 visible, enabled probes are used; the smallest box wins where
// they overlap. Serialized as "ReflectionProbe".
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

struct GIVolumeComponent {
    f32  probe_spacing = 1.0f; // metres between probes
    f32  intensity = 1.0f;     // scales the bounced light
    bool enabled = true;

    AE_REFLECT(GIVolumeComponent, AE_FIELD(probe_spacing), AE_FIELD(intensity), AE_FIELD(enabled))
};

struct ReflectionProbeComponent {
    f32  intensity = 1.0f;
    f32  blend_distance = 1.0f; // metres over which the probe fades in from its box faces
    bool enabled = true;

    AE_REFLECT(ReflectionProbeComponent, AE_FIELD(intensity), AE_FIELD(blend_distance), AE_FIELD(enabled))
};

} // namespace aether::gameplay
