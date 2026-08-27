#pragma once

#include <QtGlobal>

enum class JobLabelLodLevel { Hidden, Range, Individual };

struct JobLabelLodDecision {
    JobLabelLodLevel level = JobLabelLodLevel::Hidden;
    qint64 rangeSize = 0;
    int maximumOrdinaryLabels = 4096;
};

class JobLabelLod {
public:
    static JobLabelLodDecision decide(double projectedPointSpacing, qint64 pointCount);
};
