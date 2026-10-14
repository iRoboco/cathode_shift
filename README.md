# Руководство по использованию Axmol для 3D графики и воксельных игр

Добро пожаловать в учебник по использованию Axmol — мощного движка для работы с 3D графикой и созданием воксельных игр. Этот репозиторий представляет собой пример игры с использованием Axmol, включающий детальный разбор этапов работы с 3D-сетками, текстурами и индексами. Вы найдёте полезные советы и примеры кода для начала работы.

### Общее описание

Axmol представляет собой современный графический движок, специализирующийся на 3D-графике. Основные возможности:

- Генерация и работа с воксельными мешами.
- Интеграция текстур и UV-карт.
- Оптимизированная обработка моделей (индексные и вершинные массивы).
- Работа с ограничениями графических API (например, 16-битные индексы).

Этот проект демонстрирует принципы использования Axmol через задачи, связанные с созданием воксельной графики.

---

## Шаг 1. Деинтерливание вершин (ChunkVertex → позиции/UV)

```cpp
struct ChunkVertex { float x, y, z; float u, v; };
std::vector<ChunkVertex> interleavedVertices = /* заполнено */;

std::vector<float> positions;
std::vector<float> uvs;
positions.reserve(interleavedVertices.size() * 3);
uvs.reserve(interleavedVertices.size() * 2);

for (const auto& vertex : interleavedVertices) {
    positions.push_back(vertex.x);
    positions.push_back(vertex.y);
    positions.push_back(vertex.z);
    uvs.push_back(vertex.u);
    uvs.push_back(vertex.v);
}
```

- **Совет**: Используйте `reserve`, чтобы избежать лишних аллокаций.

---

## Шаг 2. Построение индексного буфера

```cpp
std::vector<uint16_t> indicesVec = /* сгенерированные индексы */;
ax::IndexArray indices;
indices.reserve(indicesVec.size());
for (uint16_t index : indicesVec) {
    indices.push_back(index);
}
```

**Важно:** Если количество вершин больше 65535, используйте 32-битные индексы.

---

## Шаг 3. Создание Mesh и привязка текстуры

```cpp
std::vector<float> normals;  // оставить пустым или заполнить (x, y, z) для каждого направления
ax::Mesh* mesh = ax::Mesh::create(positions, normals, uvs, indices);

if (mesh) {
    mesh->setTexture(texture);
    // Включите MeshRenderer для отображения
}
```

Axmol автоматически переносит данные в VRAM и рассчитывает границы (AABB).

---

## Рекомендации

1. Для больших сцен используйте объединённые меши.
2. Выполнение ресурсоёмких операций выполняйте в фоновом потоке.
3. Задействуйте однопоточную интеграцию для `ax::Mesh` и `ax::Texture2D`.
