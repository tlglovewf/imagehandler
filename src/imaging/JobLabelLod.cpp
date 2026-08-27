#include "imaging/JobLabelLod.h"

#include <algorithm>
#include <cmath>
#include <limits>

JobLabelLodDecision JobLabelLod::decide(double projectedPointSpacing, qint64 pointCount) {
    JobLabelLodDecision result;
    if (!std::isfinite(projectedPointSpacing) || projectedPointSpacing < 4.0 || pointCount <= 0)
        return result;
    if (projectedPointSpacing >= 32.0) {
        result.level = JobLabelLodLevel::Individual;
        result.rangeSize = 1;
        return result;
    }
    result.level = JobLabelLodLevel::Range;
    result.rangeSize = 10;
    const double desired = std::pow(80.0 / projectedPointSpacing, 2.0);
    while (result.rangeSize < desired && result.rangeSize < pointCount &&
           result.rangeSize <= std::numeric_limits<qint64>::max() / 10) {
        result.rangeSize *= 10;
    }
    result.rangeSize = std::min(result.rangeSize, std::max<qint64>(1, pointCount));
    return result;
}
