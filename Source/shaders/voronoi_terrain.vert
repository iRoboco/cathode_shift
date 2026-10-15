#version 310 es

// =============================================================================
//  voronoi_terrain.vert — вершинный шейдер воронои-террейна + дрожание «желе».
// =============================================================================
//  Как terrain.vert (V инвертирован под кастомные шейдеры), плюс нормаль грани
//  для flat-освещения. Нод чанка только сдвигается (без поворота), поэтому
//  нормаль в пространстве меша == мировая нормаль.
//
//  Желейное дрожание — по аналогии с волнами воды (water.vert): смещение вершин
//  суммой синусоид от МИРОВЫХ координат (u_chunkOrigin + local) и u_time, поэтому
//  дрожание непрерывно на стыках чанков. Сила дрожания запечена мешером в
//  a_texCoord1.x (та же маска softness·surf, что у статической CPU-деформации —
//  трясётся ровно то, что «оплывает», а на границах с обычными биомами маска
//  плавно сходит на ноль). В a_texCoord1.y — нагрузка: под толщей блоков желе
//  «тяжелеет» и дрожит слабее. Нормаль корректируется аналитически из градиента
//  смещения — освещение «играет» вместе с геометрией.
//
//  Атрибуты (интерливинг из VoronoiMeshBuilder::vertexAttribs):
//    POSITION  = a_position  (vec3, локальные коорд. чанка)
//    NORMAL    = a_normal    (vec3, нормаль грани после CPU-деформации)
//    TEXCOORD0 = a_texCoord  (vec2, UV атласа)
//    TEXCOORD1 = a_texCoord1 (vec2, x: маска дрожания [0..1], y: нагрузка [0..1])
//  Имя a_texCoord1 обязательно: axmol биндит атрибуты меша по имени.
// =============================================================================

layout(location = POSITION)  in vec4 a_position;
layout(location = TEXCOORD0) in vec2 a_texCoord;
layout(location = TEXCOORD1) in vec2 a_texCoord1;
layout(location = NORMAL)    in vec3 a_normal;

layout(location = TEXCOORD0) out vec2 v_texCoord;
layout(location = 1)         out vec3 v_normal;

layout(std140) uniform vs_ub {
    mat4  u_MVPMatrix;    // авто-биндинг движком (Pass::updateMVPUniform)
    vec3  u_chunkOrigin;  // мировые координаты угла чанка (задаётся при создании нода)
    float u_time;         // время в секундах (обновляется каждый кадр из GameScene)
};

// -----------------------------------------------------------------------------
//  Параметры дрожания. 3 компоненты, как у воды, но с 3D-направлением смещения
//  и более высокими частотами — желе дрожит мелко и быстро. Суммарная амплитуда
//  ~5.5 см: заметно глазу, но не рвёт воксельную геометрию и не отрывает
//  поверхность от коллизий (рейкаст/физика остаются по сетке).
// -----------------------------------------------------------------------------
const int   JW_COUNT = 3;
const vec3  JW_DIR[3] = vec3[3](   // направление смещения
    vec3( 0.00, 1.00,  0.00),      // вертикальное «дыхание»
    vec3( 0.72, 0.35,  0.60),      // косое покачивание
    vec3(-0.55, 0.62, -0.56));     // мелкая дрожь
const vec3  JW_GRAD[3] = vec3[3](  // градиент фазы по миру (пространственная частота)
    vec3( 0.83, 0.55,  0.83),
    vec3( 1.60, 0.90, -1.20),
    vec3( 2.90, 1.70,  2.40));
const float JW_SPD[3]  = float[3](2.4, 3.6, 5.2);
const float JW_AMP[3]  = float[3](0.130, 0.116, 0.108);

// Смещение и поправка нормали. Для x' = x + mask·d(x) якобиан даёт
// n' = normalize(n − mask·(∇d)ᵀ·n), где (∇d)ᵀ·n = Σ aᵢ·cos(phᵢ)·(dirᵢ·n)·gradᵢ.
void jellyWobble(vec3 wp, float t, float mask, vec3 n, out vec3 disp, out vec3 nCorr)
{
    disp  = vec3(0.0);
    nCorr = vec3(0.0);
    for (int i = 0; i < JW_COUNT; ++i)
    {
        float ph = dot(JW_GRAD[i], wp) + t * JW_SPD[i];
        disp  += (JW_AMP[i] * sin(ph)) * JW_DIR[i];
        nCorr += (JW_AMP[i] * cos(ph) * dot(JW_DIR[i], n)) * JW_GRAD[i];
    }
    disp  *= mask;
    nCorr *= mask;
}

void main(void)
{
    vec3 localPos = a_position.xyz;
    vec3 n        = a_normal;

    float mask = a_texCoord1.x;
    if (mask > 0.0)
    {
        // Под нагрузкой желе «тяжелеет»: дрожание затухает до ~65 %
        float damp = 1.0 - 0.35 * clamp(a_texCoord1.y, 0.0, 1.0);
        vec3 disp, nCorr;
        jellyWobble(localPos + u_chunkOrigin, u_time, mask * damp, n, disp, nCorr);
        localPos += disp;
        n        = normalize(n - nCorr);
    }

    v_texCoord  = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
    v_normal    = n;
    gl_Position = u_MVPMatrix * vec4(localPos, 1.0);
}
