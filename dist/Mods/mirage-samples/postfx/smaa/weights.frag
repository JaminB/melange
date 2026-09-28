#version 130
// SMAA pass 2: blending weights from the edges and the precomputed area/search textures.
uniform sampler2D mg_pass_edges;
uniform sampler2D t_area;
uniform sampler2D t_search;
in vec2 mg_uv;
out vec4 mg_out;
#include "smaa_config.h"

void main() {
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(mg_uv, pixcoord, offset);
    mg_out = SMAABlendingWeightCalculationPS(mg_uv, pixcoord, offset, mg_pass_edges, t_area, t_search, vec4(0.0));
}
