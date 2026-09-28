#version 120
// 9-tap Gaussian in 5 bilinear fetches; HORIZONTAL selects the direction.
uniform sampler2D mg_prev;
uniform vec4 mg_resolution;
uniform float p_radius;
varying vec2 mg_uv;

void main() {
#ifdef HORIZONTAL
    vec2 dir = vec2(mg_resolution.z, 0.0) * p_radius;
#else
    vec2 dir = vec2(0.0, mg_resolution.w) * p_radius;
#endif
    vec3 s = texture2D(mg_prev, mg_uv).rgb * 0.2270270270;
    s += (texture2D(mg_prev, mg_uv + dir * 1.3846153846).rgb + texture2D(mg_prev, mg_uv - dir * 1.3846153846).rgb) * 0.3162162162;
    s += (texture2D(mg_prev, mg_uv + dir * 3.2307692308).rgb + texture2D(mg_prev, mg_uv - dir * 3.2307692308).rgb) * 0.0702702703;
    gl_FragColor = vec4(s, 1.0);
}
