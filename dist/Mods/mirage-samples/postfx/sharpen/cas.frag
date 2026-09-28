#version 120
// Contrast Adaptive Sharpening, the no-scaling path of CasFilter() in ffx_cas.h, ported to GLSL 1.20.
//
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of
// the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

uniform sampler2D mg_scene;
uniform vec4 mg_resolution;
uniform float p_sharpness;
varying vec2 mg_uv;

vec3 Load(vec2 o) { return texture2D(mg_scene, mg_uv + o * mg_resolution.zw).rgb; }

void main() {
    // a b c
    // d e f
    // g h i
    vec3 a = Load(vec2(-1.0, -1.0));
    vec3 b = Load(vec2( 0.0, -1.0));
    vec3 c = Load(vec2( 1.0, -1.0));
    vec3 d = Load(vec2(-1.0,  0.0));
    vec4 e4 = texture2D(mg_scene, mg_uv);
    vec3 e = e4.rgb;
    vec3 f = Load(vec2( 1.0,  0.0));
    vec3 g = Load(vec2(-1.0,  1.0));
    vec3 h = Load(vec2( 0.0,  1.0));
    vec3 i = Load(vec2( 1.0,  1.0));

    // Soft min and max over the cross plus the diagonals (CAS_BETTER_DIAGONALS); 2x bigger, factored out.
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(min(min(mn, a), min(c, g)), i);
    mn = mn + mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(max(max(mx, a), max(c, g)), i);
    mx = mx + mx2;

    // Smooth minimum distance to the signal limit divided by the smooth maximum, then shaped.
    vec3 amp = clamp(min(mn, 2.0 - mx) / max(mx, vec3(1.0 / 65536.0)), 0.0, 1.0);
    amp = sqrt(amp);

    // Filter shape:  0 w 0 / w 1 w / 0 w 0
    float peak = -1.0 / mix(8.0, 5.0, clamp(p_sharpness, 0.0, 1.0));
    vec3 w = amp * peak;
    vec3 rcpWeight = 1.0 / (1.0 + 4.0 * w);
    vec3 pix = clamp((b * w + d * w + f * w + h * w + e) * rcpWeight, 0.0, 1.0);
    gl_FragColor = vec4(pix, e4.a);
}
