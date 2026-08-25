#pragma once

#include "domain/Models.h"
#include <QString>
#include <functional>

namespace VipsSupport {
bool initialize(const char* argv0, QString* error);
void shutdown();
QString lastError();
VipsImagePtr adopt(VipsImage* image);
VipsImagePtr ref(VipsImage* image);
QImage renderRegion(const ImageDocumentPtr& document, qint64 x, qint64 y,
                    qint64 width, qint64 height, double scale, QString* error);
}

