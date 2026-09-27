// shaders/renderer/pick_id.frag — picking id buffer (ADR-0009). Drawn over the finished depth
// prepass with an Equal depth test, so only the visible (nearest) surface writes its
// RenderMeshInstance::user_id (0 = not pickable). Masked materials need no alpha test: texels
// they discarded never reached the depth buffer, so the Equal test already rejects them.
#version 460
#extension GL_GOOGLE_include_directive : require
#include "common/bindless.glsl"
#include "frame_data.glsl"
#include "mesh_push.glsl"

layout(location = 4) flat in uint v_instance;
layout(location = 0) out uint out_id;

void main() {
    out_id = pc.frame.user_ids.items[v_instance];
}
