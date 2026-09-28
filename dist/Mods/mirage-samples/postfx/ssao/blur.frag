#version 120
// 7-tap blur that stops at depth edges; HORIZONTAL selects the direction.
uniform sampler2D mg_prev;
uniform sampler2D mg_depth;
uniform mat4 mg_invProj;
uniform vec4 mg_resolution;
varying vec2 mg_uv;

float ViewZ(vec2 uv) {
    vec4 p = mg_invProj * vec4(0.0, 0.0, texture2D(mg_depth, uv).r * 2.0 - 1.0, 1.0);
    return p.z / p.w;
}

void main() {
#ifdef HORIZONTAL
    vec2 dir = vec2(mg_resolution.z, 0.0);
#else
    vec2 dir = vec2(0.0, mg_resolution.w);
#endif
    float z0 = ViewZ(mg_uv);
    float sum = 0.0, wsum = 0.0;
    for (int i = -3; i <= 3; ++i) {
        vec2 uv = mg_uv + dir * float(i);
        float w = exp(-float(i * i) / 8.0) * exp(-abs(ViewZ(uv) - z0) * 8.0 / max(abs(z0), 1e-3));
        sum += texture2D(mg_prev, uv).r * w;
        wsum += w;
    }
    float ao = sum / max(wsum, 1e-5);
    gl_FragColor = vec4(ao, ao, ao, 1.0);
}
