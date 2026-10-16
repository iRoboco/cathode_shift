#version 310 es
precision highp float;
precision highp int;

// =============================================================================
//  voronoi_terrain.frag — «элегантный» материал террейна поверх процедурного
//  атласа (TextureFactory.h):
//    1. per-block вариация тона/яркости (хэш мировой ячейки) — убирает клетчатость;
//    2. ветер по траве — бегущая волна яркости (тайлы 0/1), синхронна по духу
//       с геометрическим покачиванием из вершинника;
//    3. минеральные блики руд (тайлы 7..10) — редкие мерцающие искры в свете;
//    4. lambert + ambient, умноженные на запечённое вершинное AO (стыки блоков);
//    5. желе-шиммер: френель-глосс + эмиссивная вспышка в такт дрожанию
//       (огибающая env посчитана в вершиннике тем же jellyImpulse — идеальный
//       синхрон «вздрогнул = блеснул»).
//  Выборка обычная (texture): резкость тексела задаёт фильтр атласа из Config,
//  UV на CPU уже зажаты внутрь тайла. Туман — через u_color (opacity нода).
// =============================================================================

layout(location = TEXCOORD0) in vec2 v_texCoord;
layout(location = 1)         in vec3 v_normal;
layout(location = 3)         in vec3 v_worldPos;
layout(location = 4)         in vec4 v_misc;   // x: тайл, y: AO, z: маска желе, w: вспышка
layout(location = 5)         in float v_time;

layout(binding = 0) uniform sampler2D u_tex0;

// ВНИМАНИЕ: не дублировать имена из vs_ub (u_time) — движковая рефлексия
// «плющит» имена uniform'ов, дубль сломает локации. Здесь только свои имена.
layout(std140) uniform fs_ub {
    vec4 u_color;     // opacity нода (туман по дистанции)
    vec3 u_camWorld;  // позиция камеры — для френеля желе (тикает GameScene::update)
    float u_tilePx;   // пикселизация тайлов: 0 = выкл, >0 = «пикселей» на сторону
                      // тайла (тикает GameScene::update, переключение на лету)
};

layout(location = SV_Target0) out vec4 FragColor;

const vec3  SUN     = vec3(0.42, 0.84, 0.34);  // направление НА солнце (нормализуется ниже)
const float AMBIENT = 0.52;

// Индексы тайлов атласа (совпадают с texgen::Tile / getBlockTileIndex)
const float TILE_SAND        = 5.0;
const float TILE_COAL_ORE    = 7.0;
const float TILE_CRYSTAL_ORE = 10.0;

uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}

float hash1(ivec3 p, uint salt) {
    uvec3 h = pcg3d(uvec3(p) + uvec3(salt * 0x85EBCA6Bu, salt * 0xC2B2AE35u, salt * 0x27D4EB2Fu));
    return float(h.x) * (1.0 / 4294967295.0);
}

void main(void)
{
    // --- 0. Пикселизация тайлов: «занижение» разрешения атласа на лету --------
    // Квантуем UV внутри каждого тайла на сетку u_tilePx × u_tilePx (выборка из
    // центров ячеек). Градиенты берём от НЕквантованных UV (textureGrad) — иначе
    // на границах «пикселей» производные взрывались бы и мип-уровень мерцал.
    vec2 uv = v_texCoord;
    vec4 tex;
    if (u_tilePx > 0.0)
    {
        vec2 g  = uv * 4.0;                            // 4 = тайлов в ряду атласа (texgen::kTiles)
        vec2 tl = floor(g);
        vec2 f  = fract(g);
        f       = (floor(f * u_tilePx) + 0.5) / u_tilePx;
        tex     = textureGrad(u_tex0, (tl + f) * 0.25, dFdx(uv), dFdy(uv));
    }
    else
        tex = texture(u_tex0, uv);

    vec3  n    = normalize(v_normal);
    vec3  sun  = normalize(SUN);
    float tile = v_misc.x;
    float ndl  = max(dot(n, sun), 0.0);

    // --- 1. Вариация блоков: тон и яркость от хэша мировой ячейки ------------
    // Соседние одинаковые блоки перестают сливаться в однородный ковёр.
    ivec3 cell = ivec3(floor(v_worldPos + 0.001));
    float hb   = hash1(cell, 11u);
    float hb2  = hash1(cell, 23u);
    tex.rgb *= 0.92 + 0.16 * hb;
    tex.rgb  = mix(tex.rgb, tex.rgb * vec3(1.05, 0.97, 0.92), hb2 * 0.45);

    // --- 2. Ветер по траве: бегущая волна яркости (тайлы 0 = верх, 1 = бок) --
    if (tile < 1.5)
    {
        float wind = sin(dot(v_worldPos.xz, vec2(0.33, 0.24)) - v_time * 1.9) * 0.5 + 0.5;
        float gust = 0.6 + 0.4 * sin(dot(v_worldPos.xz, vec2(0.07, 0.11)) - v_time * 0.53);
        tex.rgb *= 1.0 + 0.06 * wind * gust;
    }

    // --- 2b. Песок: тёплый шиммер на солнце -----------------------------------
    if (abs(tile - TILE_SAND) < 0.5)
        tex.rgb *= 1.0 + 0.05 * ndl;

    // --- 3. Минеральные блики руд (уголь/железо/золото/кристалл) -------------
    // Редкие точечные искры, мерцающие во времени; видны только на освещённых
    // гранях. Уголь — едва заметно, кристалл — ярче всех.
    if (tile >= TILE_COAL_ORE - 0.5 && tile <= TILE_CRYSTAL_ORE + 0.5)
    {
        ivec3 g       = ivec3(floor(v_worldPos * 7.0));
        float ph      = hash1(g, 31u) * 6.2831853;
        float tw      = pow(0.5 + 0.5 * sin(v_time * 2.6 + ph), 16.0);
        float on      = step(0.55, hash1(g, 47u));
        vec3  tint    = tile < 7.5 ? vec3(0.75, 0.72, 0.62)   // уголь — тускло
                      : tile < 8.5 ? vec3(1.00, 0.70, 0.42)   // железо
                      : tile < 9.5 ? vec3(1.00, 0.84, 0.30)   // золото
                                   : vec3(0.55, 0.95, 1.00);  // кристалл
        float strength = tile < 7.5 ? 0.10 : 0.38;
        tex.rgb += tint * (tw * on * strength) * ndl;
    }

    // --- 4. Освещение: lambert + ambient, мягкие тени стыков (вершинное AO) --
    float light = AMBIENT + (1.0 - AMBIENT) * ndl;
    light *= mix(1.0, v_misc.y, 0.85);
    vec3 rgb = tex.rgb * light;

    // --- 5. Желе-шиммер: френель-глосс + вспышка в такт дрожанию -------------
    float jelly = v_misc.z;
    if (jelly > 0.0)
    {
        vec3  viewDir = normalize(u_camWorld - v_worldPos);
        float fres    = pow(1.0 - abs(dot(n, viewDir)), 3.0);
        // постоянный лёгкий глянец «студня» по гранящим углам
        rgb += vec3(0.55, 0.95, 0.75) * (0.12 * jelly * fres);
        // эмиссивная вспышка, синхронная с огибающей jellyImpulse
        rgb += vec3(0.65, 1.00, 0.80) * (0.55 * v_misc.w * (0.35 + 0.65 * fres));
    }

    FragColor = vec4(rgb, tex.a) * u_color;
}
