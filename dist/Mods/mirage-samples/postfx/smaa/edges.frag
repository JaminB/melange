#version 130
// SMAA pass 1: luma edge detection.
uniform sampler2D mg_scene;
in vec2 mg_uv;
out vec4 mg_out;
#include "smaa_config.h"

void main() {
    vec4 offset[3];
    SMAAEdgeDetectionVS(mg_uv, offset);
    mg_out = vec4(SMAALumaEdgeDetectionPS(mg_uv, offset, mg_scene), 0.0, 0.0);
}
