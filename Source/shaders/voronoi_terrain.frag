#version 310 es
precision highp float;
precision highp int;

// =============================================================================
//  voronoi_terrain.frag — атлас + простое направленное освещение по нормали грани.
// =============================================================================
//  Без света многогранники выглядели бы плоским ковром (террейн — UNLIT), поэтому
//  затеняем по нормали: ambient + lambert от фиксированного «солнца».
//  Выборка обычная (texture): резкость тексела задаёт фильтр атласа из Config,
//  UV на CPU уже зажаты внутрь тайла.
// =============================================================================

layout(location = TEXCOORD0) in vec2 v_texCoord;
layout(location = 1)         in vec3 v_normal;

layout(binding = 0) uniform sampler2D u_tex0;

layout(std140) uniform fs_ub {
    vec4 u_color;  // opacity нода (туман по дистанции)
};

layout(location = SV_Target0) out vec4 FragColor;

const vec3  SUN     = vec3(0.42, 0.84, 0.34);  // направление НА солнце (нормализуется ниже)
const float AMBIENT = 0.52;

void main(void)
{
    vec4  tex   = texture(u_tex0, v_texCoord);
    float light = AMBIENT + (1.0 - AMBIENT) * max(dot(normalize(v_normal), normalize(SUN)), 0.0);
    FragColor   = vec4(tex.rgb * light, tex.a) * u_color;
}
