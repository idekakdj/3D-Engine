// shaders/renderer/clusters.glsl — froxel indexing (mirrors render_math.h cluster math).
// 16x9 screen tiles x 24 exponential depth slices: slice = floor(log(z) * scale - bias).
#ifndef AE_CLUSTERS_GLSL
#define AE_CLUSTERS_GLSL

uint ae_cluster_slice(FrameData frame, float view_z) {
    float s = floor(log(max(view_z, 1e-6)) * frame.cluster_z_scale - frame.cluster_z_bias);
    return uint(clamp(s, 0.0, float(AE_CLUSTER_Z - 1)));
}

uint ae_cluster_index(uint x, uint y, uint z) {
    return x + uint(AE_CLUSTER_X) * (y + uint(AE_CLUSTER_Y) * z);
}

uint ae_cluster_for_fragment(FrameData frame, vec2 frag, float view_z) {
    uint x = min(uint(max(frag.x, 0.0)) / frame.cluster_tile_w, uint(AE_CLUSTER_X) - 1u);
    uint y = min(uint(max(frag.y, 0.0)) / frame.cluster_tile_h, uint(AE_CLUSTER_Y) - 1u);
    return ae_cluster_index(x, y, ae_cluster_slice(frame, view_z));
}

#endif
