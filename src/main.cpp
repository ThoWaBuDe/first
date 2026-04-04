#include <QApplication>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // High-DPI Scaling für moderne Displays
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QApplication app(argc, argv);
    app.setApplicationName("LlamaQt");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("thomas");

    MainWindow w;
    w.show();

    return app.exec();
}
