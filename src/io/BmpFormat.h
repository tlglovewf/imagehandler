#pragma once

#include <QtGlobal>

namespace BmpFormat {
// The Windows BMP specification assigns 16 bpp BI_RGB to RGB555 and has no
// single-channel Gray16 identifier. ImageHandler marks its lossless Gray16
// BMP variant in the two reserved file-header words so it can be decoded
// unambiguously without treating ordinary RGB555 files as grayscale.
inline constexpr quint16 kGray16Reserved1 = 0x4849; // "IH" in little endian
inline constexpr quint16 kGray16Reserved2 = 0x3631; // "16" in little endian
}
