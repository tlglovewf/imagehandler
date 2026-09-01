#include "imaging/MaskStackMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr std::size_t kMaximumExplodedQuads = 2'000'000;

quint16 levelAt(const MaskStackPreviewData& preview, int x, int y) {
    if (x < 0 || y < 0 || x >= preview.gridSize.width() ||
        y >= preview.gridSize.height()) return 0;
    return preview.columnLayers[y * preview.gridSize.width() + x];
}

void addQuad(MaskStackGeometry* geometry, const MaskStackVertex& a,
             const MaskStackVertex& b, const MaskStackVertex& c,
             const MaskStackVertex& d) {
    if (!geometry || geometry->vertices.size() >
        std::numeric_limits<std::uint32_t>::max() - 4U) return;
    const auto base = static_cast<std::uint32_t>(geometry->vertices.size());
    geometry->vertices.insert(geometry->vertices.end(), {a, b, c, d});
    geometry->indices.insert(geometry->indices.end(),
                             {base, base + 1U, base + 2U,
                              base, base + 2U, base + 3U});
}

MaskStackVertex vertex(float x, float y, float z, float nx, float ny, float nz,
                       float layer, float kind) {
    return {x, y, z, nx, ny, nz, layer, kind};
}
}

std::shared_ptr<MaskStackGeometry> MaskStackMeshBuilder::buildSolid(
    const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
    const Progress& progress) {
    auto result = std::make_shared<MaskStackGeometry>();
    const int width = preview.gridSize.width();
    const int height = preview.gridSize.height();
    if (width <= 0 || height <= 0 || preview.columnLayers.size() != width * height ||
        preview.physicalSize.width() <= 0.0 || preview.physicalSize.height() <= 0.0 ||
        preview.layerHeight <= 0.0) return result;

    const float pitchX = static_cast<float>(preview.physicalSize.width() / width);
    const float pitchY = static_cast<float>(preview.physicalSize.height() / height);
    const float layerHeight = static_cast<float>(preview.layerHeight);
    if (progress) progress({QStringLiteral("生成连续堆叠实体"), 0, height});
    for (int y = 0; y < height; ++y) {
        if (token && token->isCancelled()) return {};
        const float y0 = (height - y - 1) * pitchY;
        const float y1 = (height - y) * pitchY;
        for (int x = 0; x < width; ++x) {
            const quint16 level = levelAt(preview, x, y);
            if (level == 0) continue;
            const float x0 = x * pitchX;
            const float x1 = (x + 1) * pitchX;
            const float top = level * layerHeight;
            const float layer = static_cast<float>(level);
            addQuad(result.get(), vertex(x0,y0,top,0,0,1,layer,0),
                    vertex(x1,y0,top,0,0,1,layer,0),
                    vertex(x1,y1,top,0,0,1,layer,0),
                    vertex(x0,y1,top,0,0,1,layer,0));

            const auto addWall = [&](quint16 neighbor, const MaskStackVertex& a,
                                     const MaskStackVertex& b,
                                     const MaskStackVertex& c,
                                     const MaskStackVertex& d) {
                if (neighbor >= level) return;
                addQuad(result.get(), a, b, c, d);
            };
            const auto wallBottom = [&](quint16 neighbor) {
                return neighbor * layerHeight;
            };
            const quint16 left = levelAt(preview, x - 1, y);
            const quint16 right = levelAt(preview, x + 1, y);
            const quint16 above = levelAt(preview, x, y - 1);
            const quint16 below = levelAt(preview, x, y + 1);
            addWall(left,
                vertex(x0,y1,wallBottom(left),-1,0,0,layer,1),
                vertex(x0,y0,wallBottom(left),-1,0,0,layer,1),
                vertex(x0,y0,top,-1,0,0,layer,1),
                vertex(x0,y1,top,-1,0,0,layer,1));
            addWall(right,
                vertex(x1,y0,wallBottom(right),1,0,0,layer,1),
                vertex(x1,y1,wallBottom(right),1,0,0,layer,1),
                vertex(x1,y1,top,1,0,0,layer,1),
                vertex(x1,y0,top,1,0,0,layer,1));
            addWall(above,
                vertex(x1,y1,wallBottom(above),0,1,0,layer,1),
                vertex(x0,y1,wallBottom(above),0,1,0,layer,1),
                vertex(x0,y1,top,0,1,0,layer,1),
                vertex(x1,y1,top,0,1,0,layer,1));
            addWall(below,
                vertex(x0,y0,wallBottom(below),0,-1,0,layer,1),
                vertex(x1,y0,wallBottom(below),0,-1,0,layer,1),
                vertex(x1,y0,top,0,-1,0,layer,1),
                vertex(x0,y0,top,0,-1,0,layer,1));
        }
        if (progress) progress({QStringLiteral("生成连续堆叠实体"), y + 1, height});
    }
    return result;
}

std::shared_ptr<MaskStackGeometry> MaskStackMeshBuilder::buildExploded(
    const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
    const Progress& progress) {
    auto result = std::make_shared<MaskStackGeometry>();
    const int width = preview.gridSize.width();
    const int height = preview.gridSize.height();
    if (width <= 0 || height <= 0 || preview.columnLayers.size() != width * height ||
        preview.layerHeight <= 0.0 || preview.layerCount <= 0) return result;

    std::size_t estimatedRuns = 0;
    for (int y = 0; y < height; ++y) {
        quint16 previous = 0;
        for (int x = 0; x < width; ++x) {
            const quint16 current = levelAt(preview, x, y);
            if (current > previous) estimatedRuns += current - previous;
            previous = current;
        }
    }
    result->layerStride = std::max<int>(1, static_cast<int>(
        std::ceil(double(estimatedRuns) / kMaximumExplodedQuads)));

    const float pitchX = static_cast<float>(preview.physicalSize.width() / width);
    const float pitchY = static_cast<float>(preview.physicalSize.height() / height);
    const float layerHeight = static_cast<float>(preview.layerHeight);
    const int visibleLayers = (preview.layerCount + result->layerStride - 1) /
                              result->layerStride;
    if (progress) progress({QStringLiteral("生成分层爆炸预览"), 0, visibleLayers});
    int completed = 0;
    for (int layer = 1; layer <= preview.layerCount; layer += result->layerStride) {
        if (token && token->isCancelled()) return {};
        const float z = layer * layerHeight;
        for (int y = 0; y < height; ++y) {
            int x = 0;
            while (x < width) {
                while (x < width && levelAt(preview, x, y) < layer) ++x;
                const int begin = x;
                while (x < width && levelAt(preview, x, y) >= layer) ++x;
                if (begin == x) continue;
                const float x0 = begin * pitchX;
                const float x1 = x * pitchX;
                const float y0 = (height - y - 1) * pitchY;
                const float y1 = (height - y) * pitchY;
                const float layerValue = static_cast<float>(layer);
                addQuad(result.get(), vertex(x0,y0,z,0,0,1,layerValue,2),
                        vertex(x1,y0,z,0,0,1,layerValue,2),
                        vertex(x1,y1,z,0,0,1,layerValue,2),
                        vertex(x0,y1,z,0,0,1,layerValue,2));
            }
        }
        if (progress) progress({QStringLiteral("生成分层爆炸预览"), ++completed,
                                visibleLayers});
    }
    return result;
}
