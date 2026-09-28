#version 120
uniform sampler2D mg_scene;
uniform sampler2D t_lut;
uniform float p_exposure;
uniform float p_strength;
uniform float p_contrast;
uniform float p_saturation;
uniform float p_lut;
varying vec2 mg_uv;

// Hable's filmic curve, normalised so that 0 and 1 map to themselves.
vec3 Filmic(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 Lut(vec3 c) {
    float b = c.b * 15.0;
    float s0 = floor(b);
    float s1 = min(s0 + 1.0, 15.0);
    vec2 uv = vec2((c.r * 15.0 + 0.5) / 256.0, (c.g * 15.0 + 0.5) / 16.0);
    vec3 lo = texture2D(t_lut, uv + vec2(s0 / 16.0, 0.0)).rgb;
    vec3 hi = texture2D(t_lut, uv + vec2(s1 / 16.0, 0.0)).rgb;
    return mix(lo, hi, b - s0);
}

void main() {
    vec4 src = texture2D(mg_scene, mg_uv);
    vec3 lin = pow(src.rgb, vec3(2.2)) * exp2(p_exposure);
    vec3 film = Filmic(lin * 2.0) / Filmic(vec3(2.0));
    lin = mix(lin, film, p_strength);
    vec3 c = pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2));
    c = (c - 0.5) * p_contrast + 0.5;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(luma), c, p_saturation), 0.0, 1.0);
    c = mix(c, Lut(c), p_lut);
    gl_FragColor = vec4(c, src.a);
}
