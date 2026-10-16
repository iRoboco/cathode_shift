#version 310 es

// =============================================================================
//  highlight.vert — прицел/подсветка видимой грани блока.
//
//  Геометрия грани запекается на CPU из той же ячейки Вороного и того же
//  желейного поля смещений, что и воронои-террейн (voronoi::buildBlockFace).
//  Вершинная АНИМАЦИЯ здесь повторяет voronoi_terrain.vert ОДИН-В-ОДИН
//  (событийное дрожание желе + покачивание травы ветром), поэтому подсветка
//  «ездит» по грани синхронно с ней, а не висит прямоугольником по сетке.
//
//  Смещения — чистые функции мировой позиции вершины (узел = round(worldPos))
//  и запечённых атрибутов (a_texCoord1 = маска/нагрузка, a_texCoord2.z = ветер),
//  поэтому при совпадающих позициях результат бит-в-бит тот же, что у террейна.
//  Вершины приходят в МИРОВЫХ координатах → u_chunkOrigin = 0.
//
//  Атрибуты (интерливинг voronoi::vertexAttribs):
//    POSITION  = a_position  (vec3, мировые координаты)
//    NORMAL    = a_normal    (vec3, нормаль грани — не используется)
//    TEXCOORD0 = a_texCoord  (vec2, UV — проброс во фрагментник)
//    TEXCOORD1 = a_texCoord1 (vec2, x: маска дрожания, y: нагрузка)
//    слот 4    = a_texCoord2 (vec3, z: вес покачивания травы)
// =============================================================================

layout(location = POSITION)  in vec4 a_position;
layout(location = TEXCOORD0) in vec2 a_texCoord;
layout(location = TEXCOORD1) in vec2 a_texCoord1;
layout(location = 4)         in vec3 a_texCoord2;
layout(location = NORMAL)    in vec3 a_normal;

layout(location = TEXCOORD0) out vec2 v_texCoord;

// ВНИМАНИЕ: имя u_time должно существовать ТОЛЬКО в vs_ub — движковая рефлексия
// uniform'ов «плющит» имена, и дублирование в fs_ub ломает выборку локации.
layout(std140) uniform vs_ub {
    mat4  u_MVPMatrix;
    vec3  u_chunkOrigin;  // 0: вершины уже в мировых координатах
    float u_time;         // те же часы анимации, что у террейна (тикает рейкастер)
};

// --- Параметры желейного дрожания (ДОЛЖНЫ совпадать с voronoi_terrain.vert) ---
const float JW_PERIOD   = 6.0;
const float JW_DURATION = 1.6;
const float JW_FIRE_P   = 0.18;
const float JW_AMP      = 0.14;
const float JW_DECAY    = 2.2;
const float JW_OMEGA    = 8.0;

// --- Покачивание травы ветром (ДОЛЖНО совпадать с voronoi_terrain.vert) ---
const float GW_ENABLE = 1.0;
const float GW_AMP    = 0.045;

// PCG3D: быстрый детерминированный хэш (копия из voronoi_terrain.vert)
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

vec3 hash3f(ivec3 node, uint salt) {
    uvec3 h = pcg3d(uvec3(node) + uvec3(salt * 0x85EBCA6Bu, salt * 0xC2B2AE35u, salt * 0x27D4EB2Fu));
    return vec3(h) * (1.0 / 4294967295.0);
}

// Событие «вспышки» узла: смещение, наклон нормали и огибающая (0..1).
void jellyImpulse(ivec3 node, float t, out vec3 disp, out vec3 tilt, out float env) {
    disp = vec3(0.0); tilt = vec3(0.0); env = 0.0;

    vec3 r0   = hash3f(node, 0u);
    float T   = JW_PERIOD * (0.75 + 0.5 * r0.y);
    float tt  = t + r0.x * T;
    float cyc = floor(tt / T);
    float a   = tt - cyc * T;

    if (a >= JW_DURATION) return;
    if (hash3f(node, uint(mod(cyc, 1024.0)) + 1u).x > JW_FIRE_P) return;

    env  = exp(-JW_DECAY * a) * sin(JW_OMEGA * a) * (1.0 - a / JW_DURATION);
    vec3 dir = hash3f(node, 2u) * 2.0 - 1.0;
    dir.y   += 0.35;
    disp = normalize(dir) * (JW_AMP * (0.6 + 0.8 * r0.z) * env);
    tilt = (hash3f(node, 3u) * 2.0 - 1.0) * (0.35 * env);
}

void main(void) {
    vec3 localPos = a_position.xyz;
    vec3 worldPos = localPos + u_chunkOrigin;   // u_chunkOrigin = 0 → мировые координаты

    float mask = a_texCoord1.x;                 // softness·surf, запечено билдером грани
    if (mask > 0.0) {
        float damp = (1.0 - 0.35 * clamp(a_texCoord1.y, 0.0, 1.0)) * mask;
        ivec3 node = ivec3(floor(worldPos + 0.5));
        vec3 disp, tilt;
        float env;
        jellyImpulse(node, u_time, disp, tilt, env);
        localPos += disp * damp;
    }

    // Покачивание травы: тот же код, что у террейна (вес запечён в a_texCoord2.z).
    float gw = a_texCoord2.z * GW_ENABLE * (1.0 - mask);
    if (gw > 0.0) {
        float wave = sin(u_time * 1.25 + worldPos.x * 0.21 + worldPos.z * 0.17) * 0.5 + 0.5;
        vec2  gust = vec2(sin(worldPos.x * 0.35 + u_time * 1.7),
                          cos(worldPos.z * 0.31 + u_time * 1.35));
        localPos.xz += gust * (GW_AMP * (0.35 + 0.65 * wave)) * gw;
    }

    v_texCoord  = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
    gl_Position = u_MVPMatrix * vec4(localPos, 1.0);
}
