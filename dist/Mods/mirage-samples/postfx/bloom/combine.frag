#version 120
uniform sampler2D mg_scene;
uniform sampler2D mg_pass_blurv;
uniform float p_intensity;
varying vec2 mg_uv;

void main() {
    vec4 c = texture2D(mg_scene, mg_uv);
    vec3 glow = texture2D(mg_pass_blurv, mg_uv).rgb * p_intensity;
    vec3 lin = c.rgb * c.rgb + glow;
    gl_FragColor = vec4(sqrt(clamp(lin, 0.0, 1.0)), c.a);
}
