// SMAA settings shared by the three passes (the "high" preset, with a tunable threshold).
uniform vec4 mg_resolution;
uniform float p_threshold;
#define SMAA_RT_METRICS mg_resolution.zwxy
#define SMAA_GLSL_3
#define SMAA_THRESHOLD p_threshold
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 1
#include "SMAA.hlsl"
