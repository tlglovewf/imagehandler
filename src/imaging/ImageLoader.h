#pragma once

#include "domain/Models.h"

class ImageLoader {
public:
    static ImageDocumentPtr load(const QString& path, quint64 generation, QString* error);
    static bool isProcessable(const ImageMetadata& metadata);
};
