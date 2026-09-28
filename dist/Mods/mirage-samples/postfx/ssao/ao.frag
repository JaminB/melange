#version 120
uniform sampler2D mg_depth;
uniform mat4 mg_proj;
uniform mat4 mg_invProj;
uniform vec4 mg_resolution;
uniform float p_radius;
uniform float p_intensity;
uniform float p_bias;
varying vec2 mg_uv;

const int kTaps = 12;

vec3 ViewPos(vec2 uv) {
    float d = texture2D(mg_depth, uv).r;
    vec4 p = mg_invProj * vec4(vec3(uv, d) * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

void main() {
    float depth = texture2D(mg_depth, mg_uv).r;
    if (depth >= 0.99999) {
        gl_FragColor = vec4(1.0);
        return;
    }
    vec3 P = ViewPos(mg_uv);
    vec2 px = mg_resolution.zw;
    vec3 r = ViewPos(mg_uv + vec2(px.x, 0.0)) - P, l = P - ViewPos(mg_uv - vec2(px.x, 0.0));
    vec3 u = ViewPos(mg_uv + vec2(0.0, px.y)) - P, d = P - ViewPos(mg_uv - vec2(0.0, px.y));
    vec3 N = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(u.z) < abs(d.z) ? u : d));

    // the radius projected to texture space at this depth
    vec2 ruv = min(p_radius * 0.5 * vec2(mg_proj[0][0], mg_proj[1][1]) / max(-P.z, 1e-3), vec2(0.15));
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float angle = noise * 6.2831853;
    float r2 = p_radius * p_radius;
    float occ = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float t = (float(i) + noise) / float(kTaps);
        float a = angle + float(i) * 2.3999632;
        vec3 v = ViewPos(mg_uv + vec2(cos(a), sin(a)) * t * ruv) - P;
        float vv = dot(v, v);
        float falloff = 1.0 - clamp(vv / r2, 0.0, 1.0);
        occ += max(0.0, dot(v, N) - p_bias * p_radius) / (vv + 0.01 * r2) * falloff;
    }
    float ao = clamp(1.0 - p_intensity * 2.0 * p_radius * occ / float(kTaps), 0.0, 1.0);
    gl_FragColor = vec4(ao, ao, ao, 1.0);
}
