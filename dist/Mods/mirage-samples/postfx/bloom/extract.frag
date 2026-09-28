#version 120
// Bright-pass with a soft knee, in approximately linear light. The half-size target averages 2x2 scene pixels.
uniform sampler2D mg_scene;
uniform float p_threshold;
varying vec2 mg_uv;

void main() {
    vec3 c = texture2D(mg_scene, mg_uv).rgb;
    vec3 lin = c * c;
    float bright = max(max(lin.r, lin.g), lin.b);
    float knee = max(p_threshold * 0.5, 1e-4);
    float soft = clamp(bright - p_threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contrib = max(soft, bright - p_threshold) / max(bright, 1e-4);
    gl_FragColor = vec4(lin * contrib, 1.0);
}
