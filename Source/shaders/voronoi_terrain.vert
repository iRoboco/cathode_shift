#version 310 es

// =============================================================================
//  voronoi_terrain.vert — воронои-террейн: событийное дрожание «желе»,
//  покачивание травы ветром, данные для «элегантного» фрагментника.
// =============================================================================
//  Как terrain.vert (V инвертирован под кастомные шейдеры), плюс нормаль грани
//  для flat-освещения. Нод чанка только сдвигается (без поворота), поэтому
//  нормаль в пространстве меша == мировая нормаль.
//
//  Желейное дрожание — СОБЫТИЙНОЕ (в отличие от непрерывных волн воды):
//  террейн биома неподвижен, а отдельные кубики время от времени «вздрагивают»
//  затухающей синусоидой и замирают. Расписание вспышек — детерминированный
//  хэш узла решётки (целые мировые координаты): псевдослучайные фаза, период,
//  амплитуда и направление + случайный пропуск циклов. В среднем одновременно
//  активна ~1/5 кубиков поверхности (настраивается JW_FIRE_P/JW_PERIOD).
//
//  Бесшовность: вершина находит свой узел через round(u_chunkOrigin + local),
//  т.е. смещение — функция ТОЛЬКО мировой позиции. Общие углы соседних кубов
//  (и чанков) получают одинаковое смещение → трещин нет. Углы одного кубика
//  дрожат независимо — кубик заметно «перекашивает», что и даёт ощущение желе.
//
//  Покачивание травы (GW_*): вес запечён мешером в a_texCoord2.z и тоже является
//  чистой функцией позиции (вершина качается, если блок под ней — трава), поэтому
//  верхняя кромка травяного блока движется едино со всех граней — без щелей.
//
//  Огибающая вспышки (env) уходит во фрагментник (v_misc.w): кубик в момент
//  вспышки не только двигается, но и «вспыхивает» глянцем (желе-шиммер).
//
//  Атрибуты (интерливинг из VoronoiMeshBuilder::vertexAttribs):
//    POSITION  = a_position  (vec3, локальные коорд. чанка)
//    NORMAL    = a_normal    (vec3, нормаль грани после CPU-деформации)
//    TEXCOORD0 = a_texCoord  (vec2, UV атласа)
//    TEXCOORD1 = a_texCoord1 (vec2, x: маска дрожания softness·surf, y: нагрузка)
//    слот 4    = a_texCoord2 (vec3, x: вершинное AO, y: индекс тайла, z: вес ветра)
//  (макроса TEXCOORD2 в axslcc нет: слот 4 == shaderinfos::VERTEX_ATTRIB_TEX_COORD2).
// =============================================================================

layout(location = POSITION)  in vec4 a_position;
layout(location = TEXCOORD0) in vec2 a_texCoord;
layout(location = TEXCOORD1) in vec2 a_texCoord1;
layout(location = 4)         in vec3 a_texCoord2;
layout(location = NORMAL)    in vec3 a_normal;

layout(location = TEXCOORD0) out vec2 v_texCoord;
layout(location = 1)         out vec3 v_normal;
layout(location = 3)         out vec3 v_worldPos;  // мировая позиция ДО анимаций
layout(location = 4)         out vec4 v_misc;      // x: тайл, y: AO, z: маска желе, w: вспышка
layout(location = 5)         out float v_time;     // часы анимации для фрагментника

// ВНИМАНИЕ: имя u_time должно существовать ТОЛЬКО в vs_ub — движковая рефлексия
// uniform'ов «плющит» имена, и дублирование в fs_ub ломает выборку локации.
layout(std140) uniform vs_ub {
    mat4  u_MVPMatrix;
    vec3  u_chunkOrigin;  // мировой угол чанка
    float u_time;         // часы анимации (секунды, тикает GameScene::update)
};

// --- Параметры желейного дрожания (событийная модель) ---
const float JW_PERIOD   = 6.0;   // средний период цикла событий на узел (с)
const float JW_DURATION = 1.6;   // длительность затухающей вспышки (с)
const float JW_FIRE_P   = 0.18;  // вероятность вспышки в цикле
const float JW_AMP      = 0.14;  // пиковая амплитуда вспышки (блоков)
const float JW_DECAY    = 2.2;   // скорость затухания (1/с)
const float JW_OMEGA    = 8.0;   // частота качков внутри вспышки (рад/с)

// --- Покачивание травы ветром ---
const float GW_ENABLE = 1.0;     // 0.0 → выключить покачивание
const float GW_AMP    = 0.045;   // амплитуда качания верхней кромки (блоков)

// PCG3D: быстрый детерминированный хэш (та же пара функций нужна фрагментнику)
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

// Событие «вспышки» узла: смещение, наклон нормали и огибающая (0..1, для свечения).
void jellyImpulse(ivec3 node, float t, out vec3 disp, out vec3 tilt, out float env) {
    disp = vec3(0.0); tilt = vec3(0.0); env = 0.0;

    vec3 r0   = hash3f(node, 0u);
    float T   = JW_PERIOD * (0.75 + 0.5 * r0.y);       // свой период 4.5–7.5 с
    float tt  = t + r0.x * T;                          // своя фаза
    float cyc = floor(tt / T);                         // номер цикла
    float a   = tt - cyc * T;                          // время внутри цикла

    if (a >= JW_DURATION) return;                      // окно вспышки прошло — узел спит
    if (hash3f(node, uint(mod(cyc, 1024.0)) + 1u).x > JW_FIRE_P) return;  // цикл пропущен

    env  = exp(-JW_DECAY * a) * sin(JW_OMEGA * a) * (1.0 - a / JW_DURATION);
    vec3 dir = hash3f(node, 2u) * 2.0 - 1.0;
    dir.y   += 0.35;                                   // чаще вверх — «подпрыгивание»
    disp = normalize(dir) * (JW_AMP * (0.6 + 0.8 * r0.z) * env);
    tilt = (hash3f(node, 3u) * 2.0 - 1.0) * (0.35 * env);
}

void main(void) {
    vec3 localPos = a_position.xyz;
    vec3 n        = a_normal;
    vec3 worldPos = localPos + u_chunkOrigin;   // до анимаций — для хэшей фрагментника

    float mask = a_texCoord1.x;                 // softness·surf, запечено мешером
    float env  = 0.0;
    if (mask > 0.0) {
        float damp = (1.0 - 0.35 * clamp(a_texCoord1.y, 0.0, 1.0)) * mask;  // нагрузка глушит
        ivec3 node = ivec3(floor(worldPos + 0.5));                          // round(worldPos)
        vec3 disp, tilt;
        jellyImpulse(node, u_time, disp, tilt, env);
        localPos += disp * damp;
        n        = normalize(n + tilt * damp);
    }

    // Покачивание травы: чистая функция мировой позиции и времени; в желе-биоме
    // приглушено (там своя анимация). Вес запечён мешером (a_texCoord2.z).
    float gw = a_texCoord2.z * GW_ENABLE * (1.0 - mask);
    if (gw > 0.0) {
        float wave = sin(u_time * 1.25 + worldPos.x * 0.21 + worldPos.z * 0.17) * 0.5 + 0.5;
        vec2  gust = vec2(sin(worldPos.x * 0.35 + u_time * 1.7),
                          cos(worldPos.z * 0.31 + u_time * 1.35));
        localPos.xz += gust * (GW_AMP * (0.35 + 0.65 * wave)) * gw;
    }

    v_texCoord  = vec2(a_texCoord.x, 1.0 - a_texCoord.y);
    v_normal    = n;
    v_worldPos  = worldPos;
    v_misc      = vec4(a_texCoord2.y, a_texCoord2.x, mask, env * mask);
    v_time      = u_time;
    gl_Position = u_MVPMatrix * vec4(localPos, 1.0);
}
