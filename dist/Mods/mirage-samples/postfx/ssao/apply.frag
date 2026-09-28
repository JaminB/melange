#version 120
uniform sampler2D mg_scene;
uniform sampler2D mg_pass_blurv;
uniform bool p_debug;
varying vec2 mg_uv;

void main() {
    vec4 c = texture2D(mg_scene, mg_uv);
    float ao = texture2D(mg_pass_blurv, mg_uv).r;
    gl_FragColor = p_debug ? vec4(vec3(ao), c.a) : vec4(c.rgb * ao, c.a);
}
