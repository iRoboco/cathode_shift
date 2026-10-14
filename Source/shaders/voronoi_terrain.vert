#version 310 es

// =============================================================================
//  voronoi_terrain.vert — вершинный шейдер воронои-террейна.
// =============================================================================
//  Как terrain.vert (V инвертирован под кастомные шейдеры), плюс нормаль грани
//  для flat-освещения. Нод чанка только сдвигается (без поворота), поэтому
//  нормаль в пространстве меша == мировая нормаль.
// =============================================================================

layout(location = POSITION)  in vec4 a_position;
layout(location = TEXCOORD0) in vec2 a_texCoord;
layout(location = NORMAL)    in vec3 a_normal;

layout(location = TEXCOORD0) out vec2 v_texCoord;
layout(location = 1)         out vec3 v_normal;

layout(std140) uniform vs_ub {
    mat4 u_MVPMatrix;
};

void main(void)
{
    v_texCoord  = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
    v_normal    = a_normal;
    gl_Position = u_MVPMatrix * vec4(a_position.xyz, 1.0);
}
