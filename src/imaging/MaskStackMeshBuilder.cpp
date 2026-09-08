#include "imaging/MaskStackMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
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
    // Merge coplanar cells into rectangles. Exported masks commonly contain large
    // flat areas; emitting one quad per cell made a 512 x 512 preview consume
    // hundreds of MB and multiplied that work for every Job placement.
    std::vector<std::uint8_t> consumed(static_cast<std::size_t>(width) * height);
    const qint64 progressTotal = qint64(width) + qint64(height) * 2;
    if (progress) progress({QStringLiteral("生成连续堆叠实体"), 0, progressTotal});
    for (int y = 0; y < height; ++y) {
        if (token && token->isCancelled()) return {};
        for (int x = 0; x < width; ++x) {
            const quint16 level = levelAt(preview, x, y);
            const std::size_t cell = static_cast<std::size_t>(y) * width + x;
            if (level == 0 || consumed[cell]) continue;
            int endX = x + 1;
            while (endX < width && !consumed[static_cast<std::size_t>(y) * width + endX] &&
                   levelAt(preview, endX, y) == level) ++endX;
            int endY = y + 1;
            for (; endY < height; ++endY) {
                bool matches = true;
                for (int nextX = x; nextX < endX; ++nextX) {
                    const std::size_t next = static_cast<std::size_t>(endY) * width + nextX;
                    if (consumed[next] || levelAt(preview, nextX, endY) != level) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) break;
            }
            for (int nextY = y; nextY < endY; ++nextY)
                for (int nextX = x; nextX < endX; ++nextX)
                    consumed[static_cast<std::size_t>(nextY) * width + nextX] = 1;
            const float x0 = x * pitchX;
            const float x1 = endX * pitchX;
            const float y0 = (height - endY) * pitchY;
            const float y1 = (height - y) * pitchY;
            const float top = level * layerHeight;
            const float layer = static_cast<float>(level);
            addQuad(result.get(), vertex(x0,y0,top,0,0,1,layer,0),
                    vertex(x1,y0,top,0,0,1,layer,0),
                    vertex(x1,y1,top,0,0,1,layer,0),
                    vertex(x0,y1,top,0,0,1,layer,0));

        }
        if (progress) progress({QStringLiteral("生成连续堆叠实体"), y + 1, progressTotal});
    }

    // Merge X-facing walls along Y when both their top and bottom levels match.
    for (int x = 0; x < width; ++x) {
        for (int side = 0; side < 2; ++side) {
            int y = 0;
            while (y < height) {
                const quint16 level = levelAt(preview, x, y);
                const quint16 neighbor = levelAt(preview, x + (side ? 1 : -1), y);
                if (level == 0 || neighbor >= level) { ++y; continue; }
                int endY = y + 1;
                while (endY < height && levelAt(preview, x, endY) == level &&
                       levelAt(preview, x + (side ? 1 : -1), endY) == neighbor) ++endY;
                const float wallX = (x + side) * pitchX;
                const float yLow = (height - endY) * pitchY;
                const float yHigh = (height - y) * pitchY;
                const float bottom = neighbor * layerHeight;
                const float top = level * layerHeight;
                const float layer = static_cast<float>(level);
                if (!side)
                    addQuad(result.get(), vertex(wallX,yHigh,bottom,-1,0,0,layer,1),
                            vertex(wallX,yLow,bottom,-1,0,0,layer,1),
                            vertex(wallX,yLow,top,-1,0,0,layer,1),
                            vertex(wallX,yHigh,top,-1,0,0,layer,1));
                else
                    addQuad(result.get(), vertex(wallX,yLow,bottom,1,0,0,layer,1),
                            vertex(wallX,yHigh,bottom,1,0,0,layer,1),
                            vertex(wallX,yHigh,top,1,0,0,layer,1),
                            vertex(wallX,yLow,top,1,0,0,layer,1));
                y = endY;
            }
        }
        if (progress) progress({QStringLiteral("生成连续堆叠实体"), height + x + 1,
                                progressTotal});
    }

    // Merge Y-facing walls along X using the same level-pair criterion.
    for (int y = 0; y < height; ++y) {
        for (int side = 0; side < 2; ++side) {
            int x = 0;
            while (x < width) {
                const quint16 level = levelAt(preview, x, y);
                const quint16 neighbor = levelAt(preview, x, y + (side ? 1 : -1));
                if (level == 0 || neighbor >= level) { ++x; continue; }
                int endX = x + 1;
                while (endX < width && levelAt(preview, endX, y) == level &&
                       levelAt(preview, endX, y + (side ? 1 : -1)) == neighbor) ++endX;
                const float x0 = x * pitchX;
                const float x1 = endX * pitchX;
                const float wallY = (height - y - (side ? 1 : 0)) * pitchY;
                const float bottom = neighbor * layerHeight;
                const float top = level * layerHeight;
                const float layer = static_cast<float>(level);
                if (!side)
                    addQuad(result.get(), vertex(x1,wallY,bottom,0,1,0,layer,1),
                            vertex(x0,wallY,bottom,0,1,0,layer,1),
                            vertex(x0,wallY,top,0,1,0,layer,1),
                            vertex(x1,wallY,top,0,1,0,layer,1));
                else
                    addQuad(result.get(), vertex(x0,wallY,bottom,0,-1,0,layer,1),
                            vertex(x1,wallY,bottom,0,-1,0,layer,1),
                            vertex(x1,wallY,top,0,-1,0,layer,1),
                            vertex(x0,wallY,top,0,-1,0,layer,1));
                x = endX;
            }
        }
        if (progress) progress({QStringLiteral("生成连续堆叠实体"),
                                qint64(height) + width + y + 1, progressTotal});
    }
    return result;
}

std::shared_ptr<MaskStackGeometry> MaskStackMeshBuilder::buildExploded(
    const MaskStackPreviewData& preview, const CancellationTokenPtr& token,
    const Progress& progress, std::size_t maximumQuads) {
    auto result = std::make_shared<MaskStackGeometry>();
    const int width = preview.gridSize.width();
    const int height = preview.gridSize.height();
    if (width <= 0 || height <= 0 || preview.columnLayers.size() != width * height ||
        preview.layerHeight <= 0.0 || preview.layerCount <= 0) return result;

    struct HeightRectangle { int x0, x1, y0, y1; quint16 level; };
    std::vector<HeightRectangle> rectangles;
    std::vector<std::uint8_t> consumed(static_cast<std::size_t>(width) * height);
    std::size_t estimatedQuads = 0;
    for (int y = 0; y < height; ++y) {
        if (token && token->isCancelled()) return {};
        for (int x = 0; x < width; ++x) {
            const quint16 level = levelAt(preview, x, y);
            const std::size_t cell = static_cast<std::size_t>(y) * width + x;
            if (level == 0 || consumed[cell]) continue;
            int endX = x + 1;
            while (endX < width && !consumed[static_cast<std::size_t>(y) * width + endX] &&
                   levelAt(preview, endX, y) == level) ++endX;
            int endY = y + 1;
            for (; endY < height; ++endY) {
                bool matches = true;
                for (int nextX = x; nextX < endX; ++nextX) {
                    const std::size_t next = static_cast<std::size_t>(endY) * width + nextX;
                    if (consumed[next] || levelAt(preview, nextX, endY) != level) {
                        matches = false;
                        break;
                    }
                }
                if (!matches) break;
            }
            for (int nextY = y; nextY < endY; ++nextY)
                for (int nextX = x; nextX < endX; ++nextX)
                    consumed[static_cast<std::size_t>(nextY) * width + nextX] = 1;
            rectangles.push_back({x, endX, y, endY, level});
            if (estimatedQuads > std::numeric_limits<std::size_t>::max() - level)
                estimatedQuads = std::numeric_limits<std::size_t>::max();
            else
                estimatedQuads += level;
        }
    }
    maximumQuads = std::max<std::size_t>(1, maximumQuads);
    const double requiredStride = std::ceil(double(estimatedQuads) / maximumQuads);
    result->layerStride = std::max<int>(1, static_cast<int>(
        std::min<double>(preview.layerCount, requiredStride)));

    const float pitchX = static_cast<float>(preview.physicalSize.width() / width);
    const float pitchY = static_cast<float>(preview.physicalSize.height() / height);
    const float layerHeight = static_cast<float>(preview.layerHeight);
    if (progress) progress({QStringLiteral("生成分层爆炸预览"), 0,
                            static_cast<qint64>(rectangles.size())});
    qint64 completed = 0;
    for (const HeightRectangle& rectangle : rectangles) {
        if (token && token->isCancelled()) return {};
        const float x0 = rectangle.x0 * pitchX;
        const float x1 = rectangle.x1 * pitchX;
        const float y0 = (height - rectangle.y1) * pitchY;
        const float y1 = (height - rectangle.y0) * pitchY;
        for (int layer = 1; layer <= rectangle.level; layer += result->layerStride) {
            const float z = layer * layerHeight;
            const float layerValue = static_cast<float>(layer);
            addQuad(result.get(), vertex(x0,y0,z,0,0,1,layerValue,2),
                    vertex(x1,y0,z,0,0,1,layerValue,2),
                    vertex(x1,y1,z,0,0,1,layerValue,2),
                    vertex(x0,y1,z,0,0,1,layerValue,2));
        }
        if (progress) progress({QStringLiteral("生成分层爆炸预览"), ++completed,
                                static_cast<qint64>(rectangles.size())});
    }
    return result;
}
