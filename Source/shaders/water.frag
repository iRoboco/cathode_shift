#version 310 es
precision highp float;
precision highp int;

// =============================================================================
//  water.frag — блики, Fresnel-отражение и солнечные искры на воде
// =============================================================================
//  Расчёт освещения поверхности воды:
//   1. Fresnel  — рост отражательной способности под скользящим углом (Schlick).
//   2. Specular — резкий солнечный блик (Blinn-Phong, высокая степень).
//   3. Glitter  — анимированные высокочастотные искры на гребнях волн.
//   4. Sky tint — подмешивание цвета неба по Fresnel (имитация отражения).
//  Альфа берётся из u_color.a — это сохраняет вашу логику прозрачности
//  (над/под водой + пульсация из GameScene::update), плюс лёгкий буст по бликам.
// =============================================================================

layout(location = TEXCOORD0) in vec2 v_texCoord;
layout(location = TEXCOORD1) in vec3 v_normal;
layout(location = TEXCOORD2) in vec3 v_worldPos;
layout(location = TEXCOORD3) in float v_time;   // время приходит из VS (единственный u_time)

layout(binding = 0) uniform sampler2D u_tex0;  // атлас террейна (тайл воды)

layout(std140) uniform fs_ub {
    vec4  u_color;     // авто-биндинг: цвет/opacity нода (Pass _locColor)
    vec3  u_camWorld;  // мировая позиция камеры (обновляется каждый кадр)
    vec3  u_lightDir;  // направление лучей солнца (ОТ солнца К сцене), нормировано
    vec3  u_fogColor;  // цвет тумана
    float u_fogStart;  // дистанция начала тумана (мировые единицы)
    float u_fogEnd;    // дистанция полного тумана (мировые единицы)
    float u_tilePx;    // пикселизация тайла воды: 0 = выкл, >0 = «пикселей» на
                       // сторону тайла (тот же переключатель, что у террейна)
    // u_time НЕ объявляем здесь: он живёт только в vs_ub и приходит как varying v_time
    // (см. water.vert) — иначе дублирование имени ломает вершинную анимацию волн.
};

layout(location = SV_Target0) out vec4 FragColor;

// Палитра воды (можно вынести в uniform при желании)
const vec3 DEEP_COLOR    = vec3(0.04, 0.22, 0.42);  // глубокая вода
const vec3 SHALLOW_COLOR = vec3(0.10, 0.45, 0.62);  // мелководье / гребни
const vec3 SKY_COLOR     = vec3(0.55, 0.78, 0.95);  // отражение неба
const vec3 SUN_COLOR     = vec3(1.00, 0.97, 0.85);  // солнечный блик

// Дешёвый псевдослучайный шум для искр
float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

void main(void)
{
    vec3 N = normalize(v_normal);
    vec3 V = normalize(u_camWorld - v_worldPos);          // к камере
    vec3 L = normalize(-u_lightDir);                      // к солнцу
    vec3 H = normalize(L + V);                            // полу-вектор

    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);

    // --- 1. Fresnel (Schlick), F0 ~0.02 для воды ---
    float fresnel = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);

    // --- Базовый цвет: глубина имитируется по Fresnel + текстура атласа ---
    // Пикселизация тайла — та же схема, что в voronoi_terrain.frag: квантуем UV
    // внутри тайла, градиенты — от непрерывных координат (без мип-мерцания).
    vec2 uv = v_texCoord;
    vec3 tex;
    if (u_tilePx > 0.0)
    {
        vec2 g  = uv * 4.0;                            // 4 = тайлов в ряду атласа (texgen::kTiles)
        vec2 tl = floor(g);
        vec2 f  = fract(g);
        f  = (floor(f * u_tilePx) + 0.5) / u_tilePx;
        tex = textureGrad(u_tex0, (tl + f) * 0.25, dFdx(uv), dFdy(uv)).rgb;
    }
    else
        tex = texture(u_tex0, uv).rgb;
    vec3  baseCol  = mix(DEEP_COLOR, SHALLOW_COLOR, NdotV);
    baseCol        = mix(baseCol, baseCol * (0.7 + 0.6 * tex.b), 0.35);

    // --- 4. Отражение неба по Fresnel ---
    vec3 waterCol = mix(baseCol, SKY_COLOR, fresnel * 0.65);

    // --- 2. Солнечный блик (Blinn-Phong, резкий) ---
    float specular = pow(NdotH, 220.0) * 1.4;

    // --- 3. Тонкие солнечные искры, ДРЕЙФУЮЩИЕ по поверхности ---
    // Поле искр медленно «течёт» (искры бегут, а не мерцают на месте). Каждая искра —
    // маленькая круглая точка у случайного центра своей ячейки, а не вся ячейка целиком
    // (раньше floor() заливал ячейку → искры были крупными квадратами). Светятся лишь
    // редкие ячейки, и совсем тускло — это акцент, а не основной блик.
    vec2  gp      = v_worldPos.xz * 5.0 + vec2(v_time * 0.35, v_time * 0.22);  // дрейф → искры бегут
    vec2  cell    = floor(gp);
    vec2  f       = fract(gp);
    float rnd     = hash(cell);
    vec2  center  = vec2(hash(cell + 0.17), hash(cell + 0.43));   // случайный центр искры в ячейке
    float dot2c   = length(f - center);
    float point   = smoothstep(0.15, 0.0, dot2c);                 // маленькая круглая точка
    float rare    = smoothstep(0.93, 1.0, rnd);                   // светятся только редкие ячейки
    float glint   = point * rare * smoothstep(0.60, 0.95, NdotH) * 0.6;  // мелко, редко, тускло

    vec3 color = waterCol + SUN_COLOR * (specular + glint);

    // Альфа: из нода + усиление на бликах/гранях (вода у кромки плотнее)
    float alpha = clamp(u_color.a + fresnel * 0.18 + specular * 0.5, 0.0, 1.0);

    // --- 5. Distance fog — плавное слияние воды с туманом ---
    float fogDist = length(u_camWorld - v_worldPos);
    float fogFactor = 1.0 - clamp((fogDist - u_fogStart) / (u_fogEnd - u_fogStart), 0.0, 1.0);
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    color = mix(u_fogColor, color, fogFactor);
    alpha *= fogFactor;

    FragColor = vec4(color, alpha);
}
