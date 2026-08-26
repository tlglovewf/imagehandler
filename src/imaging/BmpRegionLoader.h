#pragma once

#include <QString>

struct _VipsImage;
using VipsImage = _VipsImage;

class BmpRegionLoader {
public:
    enum class Status { NotBmp, Loaded, UseGenericLoader, Error };

    struct Result {
        Status status = Status::NotBmp;
        VipsImage* image = nullptr;
        int sourceBitsPerPixel = 0;
        bool grayscalePalette = false;
        QString error;
    };

    // Creates a lazy, region-readable one-band image for uncompressed 1-bit
    // and grayscale 8-bit BMP files. No full-size pixel buffer is allocated.
    static Result load(const QString& path);
};
