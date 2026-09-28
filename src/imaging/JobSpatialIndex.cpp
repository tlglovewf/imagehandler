#include "imaging/JobSpatialIndex.h"

#include <QVector4D>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <numeric>
#include <limits>
#include <queue>

namespace {
constexpr quint32 kLeafPoints = 32;

class ClipFrustum {
public:
    explicit ClipFrustum(const QMatrix4x4& matrix) {
        const auto row = [&](int index) {
            return QVector4D(matrix(index, 0), matrix(index, 1),
                             matrix(index, 2), matrix(index, 3));
        };
        const QVector4D x = row(0), y = row(1), z = row(2), w = row(3);
        planes_ = {w + x, w - x, w + y, w - y, z, w - z};
    }

    [[nodiscard]] bool visible(const JobCoordinate& worldMinimum,
                               const JobCoordinate& worldMaximum,
                               const JobCoordinate& origin) const {
        const QVector3D minimum = worldMinimum.relativeTo(origin);
        const QVector3D maximum = worldMaximum.relativeTo(origin);
        const QVector3D center = (minimum + maximum) * 0.5F;
        const QVector3D extent = (maximum - minimum) * 0.5F;
        for (const QVector4D& plane : planes_) {
            const float distance = plane.x() * center.x() + plane.y() * center.y() +
                                   plane.z() * center.z() + plane.w();
            const float radius = std::abs(plane.x()) * extent.x() +
                                 std::abs(plane.y()) * extent.y() +
                                 std::abs(plane.z()) * extent.z();
            if (distance + radius < 0.0F) return false;
        }
        return true;
    }

private:
    std::array<QVector4D, 6> planes_;
};
}

std::shared_ptr<JobSpatialIndex> JobSpatialIndex::build(const std::vector<JobPoint>& points,
                                                        const CancellationTokenPtr& token) {
    if (!token || token->isCancelled() || points.empty() ||
        points.size() > std::numeric_limits<quint32>::max()) return {};
    auto index = std::shared_ptr<JobSpatialIndex>(new JobSpatialIndex);
    index->indices_.resize(points.size());
    std::iota(index->indices_.begin(), index->indices_.end(), 0U);
    index->nodes_.reserve(points.size() / kLeafPoints * 2 + 1);
    if (index->buildNode(points, 0, static_cast<quint32>(points.size()), token) < 0 || token->isCancelled())
        return {};
    return index;
}

bool JobSpatialIndex::prepare(JobDocument* document,const CancellationTokenPtr& token){
    if(!document||!token||token->isCancelled()||document->points.empty())return false;
    document->idPrefixSums.resize(document->points.size()+1);
    for(size_t i=0;i<document->points.size();++i){if((i&4095U)==0&&token->isCancelled())return false;const auto& previous=document->idPrefixSums[i];const auto& point=document->points[i].position;document->idPrefixSums[i+1]={previous.x+point.x,previous.y+point.y,previous.z+point.z};}
    document->spatialIndex=build(document->points,token);
    return document->spatialIndex!=nullptr;
}

qint32 JobSpatialIndex::buildNode(const std::vector<JobPoint>& points, quint32 begin, quint32 count,
                                  const CancellationTokenPtr& token) {
    if (token->isCancelled()) return -1;
    Node node;
    node.begin = begin; node.count = count;
    const JobCoordinate& first = points[indices_[begin]].position;
    node.minimum = node.maximum = first;
    for (quint32 offset = 1; offset < count; ++offset) {
        const JobCoordinate& point = points[indices_[begin + offset]].position;
        node.minimum.x = std::min(node.minimum.x, point.x); node.maximum.x = std::max(node.maximum.x, point.x);
        node.minimum.y = std::min(node.minimum.y, point.y); node.maximum.y = std::max(node.maximum.y, point.y);
        node.minimum.z = std::min(node.minimum.z, point.z); node.maximum.z = std::max(node.maximum.z, point.z);
    }
    const qint32 nodeIndex = static_cast<qint32>(nodes_.size());
    nodes_.push_back(node);
    if (count <= kLeafPoints) return nodeIndex;
    const double extent[3] = {node.maximum.x-node.minimum.x,node.maximum.y-node.minimum.y,node.maximum.z-node.minimum.z};
    const int axis = extent[1] > extent[0] ? (extent[2] > extent[1] ? 2 : 1) : (extent[2] > extent[0] ? 2 : 0);
    const quint32 leftCount = count / 2;
    auto coordinate = [&](quint32 pointIndex) {
        const auto& p = points[pointIndex].position;
        return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
    };
    std::nth_element(indices_.begin()+begin,indices_.begin()+begin+leftCount,indices_.begin()+begin+count,
                     [&](quint32 a,quint32 b){return coordinate(a)<coordinate(b);});
    const qint32 left = buildNode(points,begin,leftCount,token);
    const qint32 right = left < 0 ? -1 : buildNode(points,begin+leftCount,count-leftCount,token);
    if (left < 0 || right < 0) return -1;
    nodes_[nodeIndex].left=left;nodes_[nodeIndex].right=right;nodes_[nodeIndex].count=0;
    return nodeIndex;
}

float JobSpatialIndex::nodeRayPriority(const Node& node, const QVector3D& origin,
                                       const QVector3D& direction, float angularRadius,
                                       const JobCoordinate& coordinateOrigin) const {
    const QVector3D minimum=node.minimum.relativeTo(coordinateOrigin);
    const QVector3D maximum=node.maximum.relativeTo(coordinateOrigin);
    const QVector3D center=(minimum+maximum)*0.5F;
    const float radius=(maximum-minimum).length()*0.5F;
    const float along=QVector3D::dotProduct(center-origin,direction);
    if(along+radius<0.0F)return std::numeric_limits<float>::infinity();
    const QVector3D closest=origin+direction*std::max(0.0F,along);
    const float allowed=radius+std::max(0.0F,along)*angularRadius;
    if ((center-closest).lengthSquared()>allowed*allowed)
        return std::numeric_limits<float>::infinity();
    return std::max(0.0F, along-radius);
}

void JobSpatialIndex::queryRay(const QVector3D& rayOrigin,const QVector3D& rayDirection,
                               float angularRadius,const JobCoordinate& coordinateOrigin,
                               std::vector<quint32>* candidates,size_t maximumCandidates) const {
    if(!candidates)return;candidates->clear();if(nodes_.empty())return;
    struct PendingNode { float priority; qint32 index; };
    const auto farther=[](const PendingNode& left,const PendingNode& right){return left.priority>right.priority;};
    std::priority_queue<PendingNode,std::vector<PendingNode>,decltype(farther)> pending(farther);
    const float rootPriority=nodeRayPriority(nodes_[0],rayOrigin,rayDirection,angularRadius,coordinateOrigin);
    if(std::isfinite(rootPriority))pending.push({rootPriority,0});
    while(!pending.empty()&&candidates->size()<maximumCandidates){
        const qint32 index=pending.top().index;pending.pop();const Node& node=nodes_[index];
        if(node.leaf()){
            const size_t remaining=maximumCandidates-candidates->size();
            const size_t take=std::min<size_t>(node.count,remaining);
            candidates->insert(candidates->end(),indices_.begin()+node.begin,indices_.begin()+node.begin+take);
            continue;
        }
        for(const qint32 child:{node.left,node.right}){
            const float priority=nodeRayPriority(nodes_[child],rayOrigin,rayDirection,angularRadius,coordinateOrigin);
            if(std::isfinite(priority))pending.push({priority,child});
        }
    }
}

void JobSpatialIndex::queryVisible(const QMatrix4x4& viewProjection,const JobCoordinate& coordinateOrigin,
                                   std::vector<quint32>* candidates,size_t maximumCandidates) const {
    if(!candidates)return;candidates->clear();if(nodes_.empty())return;
    const ClipFrustum frustum(viewProjection);
    std::vector<qint32> stack{0};
    while(!stack.empty()&&candidates->size()<maximumCandidates){const qint32 index=stack.back();stack.pop_back();const Node& node=nodes_[index];if(!frustum.visible(node.minimum,node.maximum,coordinateOrigin))continue;if(node.leaf()){const size_t remaining=maximumCandidates-candidates->size();const size_t take=std::min<size_t>(node.count,remaining);candidates->insert(candidates->end(),indices_.begin()+node.begin,indices_.begin()+node.begin+take);}else{stack.push_back(node.left);stack.push_back(node.right);}}
}

std::shared_ptr<JobPlacementSpatialIndex> JobPlacementSpatialIndex::build(
    const JobDocument& document, const CancellationTokenPtr& token) {
    if (!token || token->isCancelled() ||
        document.points.size() > std::numeric_limits<quint32>::max()) return {};
    auto result = std::shared_ptr<JobPlacementSpatialIndex>(new JobPlacementSpatialIndex);
    result->bounds_.reserve(document.points.size());
    for (quint32 pointIndex = 0; pointIndex < document.points.size(); ++pointIndex) {
        if ((pointIndex & 4095U) == 0 && token->isCancelled()) return {};
        const JobPoint& point = document.points[pointIndex];
        if (point.placeId < 0 || point.placeId >= static_cast<qint64>(document.models.size())) continue;
        const JobModel& model = document.models[static_cast<std::size_t>(point.placeId)];
        if (!model.stack) continue;
        const double halfWidth = model.stack->physicalSize.width() * 0.5;
        const double halfHeight = model.stack->physicalSize.height() * 0.5;
        const JobCoordinate minimum{
            point.position.x - halfWidth,
            point.position.y - halfHeight,
            point.position.z};
        const JobCoordinate maximum{
            point.position.x + halfWidth,
            point.position.y + halfHeight,
            point.position.z + model.stack->layerCount * model.stack->layerHeight};
        result->bounds_.push_back({minimum, maximum, pointIndex});
    }
    if (result->bounds_.empty() || result->bounds_.size() > std::numeric_limits<quint32>::max())
        return {};
    result->indices_.resize(result->bounds_.size());
    std::iota(result->indices_.begin(), result->indices_.end(), 0U);
    result->nodes_.reserve(result->bounds_.size() / kLeafPoints * 2 + 1);
    if (result->buildNode(0, static_cast<quint32>(result->indices_.size()), token) < 0 ||
        token->isCancelled()) return {};
    return result;
}

qint32 JobPlacementSpatialIndex::buildNode(quint32 begin, quint32 count,
                                            const CancellationTokenPtr& token) {
    if (token->isCancelled()) return -1;
    const Bounds& first = bounds_[indices_[begin]];
    Node node{first.minimum, first.maximum, begin, count};
    for (quint32 offset = 1; offset < count; ++offset) {
        const Bounds& value = bounds_[indices_[begin + offset]];
        node.minimum.x = std::min(node.minimum.x, value.minimum.x);
        node.minimum.y = std::min(node.minimum.y, value.minimum.y);
        node.minimum.z = std::min(node.minimum.z, value.minimum.z);
        node.maximum.x = std::max(node.maximum.x, value.maximum.x);
        node.maximum.y = std::max(node.maximum.y, value.maximum.y);
        node.maximum.z = std::max(node.maximum.z, value.maximum.z);
    }
    const qint32 nodeIndex = static_cast<qint32>(nodes_.size());
    nodes_.push_back(node);
    if (count <= kLeafPoints) return nodeIndex;
    const double extent[3]{node.maximum.x - node.minimum.x,
                           node.maximum.y - node.minimum.y,
                           node.maximum.z - node.minimum.z};
    const int axis = extent[1] > extent[0] ? (extent[2] > extent[1] ? 2 : 1)
                                           : (extent[2] > extent[0] ? 2 : 0);
    const quint32 leftCount = count / 2;
    const auto center = [&](quint32 index) {
        const Bounds& value = bounds_[index];
        return axis == 0 ? value.minimum.x + value.maximum.x
             : axis == 1 ? value.minimum.y + value.maximum.y
                         : value.minimum.z + value.maximum.z;
    };
    std::nth_element(indices_.begin() + begin, indices_.begin() + begin + leftCount,
                     indices_.begin() + begin + count,
                     [&](quint32 left, quint32 right) { return center(left) < center(right); });
    const qint32 left = buildNode(begin, leftCount, token);
    const qint32 right = left < 0 ? -1 : buildNode(begin + leftCount, count - leftCount, token);
    if (left < 0 || right < 0) return -1;
    nodes_[nodeIndex].left = left;
    nodes_[nodeIndex].right = right;
    nodes_[nodeIndex].count = 0;
    return nodeIndex;
}

void JobPlacementSpatialIndex::queryVisible(const QMatrix4x4& viewProjection,
                                             const JobCoordinate& coordinateOrigin,
                                             std::vector<quint32>* candidates,
                                             size_t maximumCandidates) const {
    if (!candidates) return;
    candidates->clear();
    if (nodes_.empty()) return;
    const ClipFrustum frustum(viewProjection);
    // Breadth-first traversal keeps a bounded result spatially representative.
    // A depth-first prefix tends to return one corner of a dense Job when the
    // renderer deliberately caps the number of placements processed per frame.
    std::deque<qint32> pending{0};
    while (!pending.empty() && candidates->size() < maximumCandidates) {
        const qint32 index = pending.front();
        pending.pop_front();
        const Node& node = nodes_[index];
        if (!frustum.visible(node.minimum, node.maximum, coordinateOrigin)) continue;
        if (node.leaf()) {
            for (quint32 offset = 0; offset < node.count &&
                 candidates->size() < maximumCandidates; ++offset) {
                const Bounds& bounds = bounds_[indices_[node.begin + offset]];
                if (frustum.visible(bounds.minimum, bounds.maximum, coordinateOrigin))
                    candidates->push_back(bounds.pointIndex);
            }
        } else {
            pending.push_back(node.left);
            pending.push_back(node.right);
        }
    }
}
