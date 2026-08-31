#include "ui/MainWindow.h"
#include "imaging/VipsSupport.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ImageHandler"));
    QApplication::setOrganizationName(QStringLiteral("ImageHandler"));
    QApplication::setApplicationVersion(QStringLiteral(IMAGE_HANDLER_VERSION));
    QString error;
    if (!VipsSupport::initialize(argv[0], &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("初始化失败"), error);
        return 1;
    }
    int result = 0;
    {
        MainWindow window; window.show();
        result = app.exec();
    }
    VipsSupport::shutdown();
    return result;
}
