#version 130
// SMAA pass 3: neighbourhood blending.
uniform sampler2D mg_scene;
uniform sampler2D mg_pass_weights;
in vec2 mg_uv;
out vec4 mg_out;
#include "smaa_config.h"

void main() {
    vec4 offset;
    SMAANeighborhoodBlendingVS(mg_uv, offset);
    mg_out = SMAANeighborhoodBlendingPS(mg_uv, offset, mg_scene, mg_pass_weights);
}
