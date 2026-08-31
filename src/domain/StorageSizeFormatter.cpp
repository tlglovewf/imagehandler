#include "domain/StorageSizeFormatter.h"

QString formatStorageSize(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("未知");
    static constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    QString number = unit == 0 ? QString::number(bytes) : QString::number(value, 'f', 2);
    if (unit > 0) {
        while (number.endsWith(QLatin1Char('0'))) number.chop(1);
        if (number.endsWith(QLatin1Char('.'))) number.chop(1);
    }
    return QStringLiteral("%1 %2").arg(number, QString::fromLatin1(units[unit]));
}
