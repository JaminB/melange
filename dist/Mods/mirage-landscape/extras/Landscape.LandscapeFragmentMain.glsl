#version 120
// GLSL port of the landscape pixel shader (Landscape.cg, LandscapeFragmentMain), for Mirage's experimental
// per-program GLSL replacement. It renders like the original. Copy it into a mod's shaders\ folder to use it.
// Inputs follow Cg's conventions: TEXCOORDn is gl_TexCoord[n], COLOR is gl_Color; uniforms keep their Cg names.
uniform sampler2D texture0;
uniform sampler2DShadow shadowMap;
uniform vec3 shadowSize;
uniform vec3 globalDiffuse;
uniform vec3 globalAmbient;
uniform vec3 globalSpecular;
uniform vec3 globalFresnel;

float Lit(vec4 pos, vec2 offset, vec2 texel) {
    return shadow2DProj(shadowMap, vec4(pos.xy + offset * texel * pos.w, pos.zw)).r;
}

void main() {
    const vec3 fresnelCol = vec3(0.2, 0.275, 0.175);
    const vec3 specularCol = vec3(0.6, 0.6, 0.6);
    const float fresnelPower = 1.5;
    const float specularPower = 20.0;

    vec4 shadowPos = gl_TexCoord[4];
    vec4 proj = shadowPos / shadowPos.w;
    float lit = 1.0;
    if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0) {
        vec2 texel = 1.0 / shadowSize.xy;
        lit = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) lit += Lit(shadowPos, vec2(float(x), float(y)), texel);
        lit /= 9.0;
    }

    vec3 n = normalize(gl_TexCoord[2].xyz);
    vec3 v = normalize(gl_TexCoord[1].xyz);
    float diffuse = 0.0;
    float specular = 0.0;
    if (lit > 0.01) {
        vec3 l = normalize(-gl_TexCoord[3].xyz);
        diffuse = clamp(dot(n, l) * lit, 0.0, 1.0);
        specular = lit * pow(clamp(dot(n, normalize(l + v)), 0.0, 1.0), specularPower);
    }
    vec3 base = globalDiffuse * diffuse + globalAmbient;
    vec3 add = globalSpecular * specularCol * specular;
    add += (0.5 + lit * 0.5) * fresnelCol * globalFresnel * clamp(pow(max(1.0 - dot(v, n), 0.0), fresnelPower), 0.0, 1.0);

    vec4 tex = texture2D(texture0, gl_TexCoord[0].xy);
    gl_FragColor = vec4(clamp(base * tex.rgb + add, 0.0, 1.0), tex.a) * gl_Color;
}
