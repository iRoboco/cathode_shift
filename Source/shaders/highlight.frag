#version 310 es
precision highp float;

// =============================================================================
//  highlight.frag — полупрозрачная заливка прицела/прогресса разрушения.
//  Цвет задаётся uniform'ом u_tintColor (тикает VoxelRaycasterNode::update),
//  текстура не сэмплируется — геометрия точно повторяет видимую грань.
//  Имя u_tintColor не пересекается с vs_ub (u_time) — см. предупреждение
//  о «плющении» имён uniform'ов в voronoi_terrain.vert.
// =============================================================================

layout(location = TEXCOORD0) in vec2 v_texCoord;

layout(std140) uniform fs_ub {
    vec4 u_tintColor;   // цвет подсветки (RGBA), включая прозрачность
};

layout(location = SV_Target0) out vec4 FragColor;

void main(void)
{
    FragColor = u_tintColor;
}
