#include <QApplication>
#include <QFont>

#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName("Janna");
    application.setOrganizationName("Janna");
    QFont applicationFont("Microsoft YaHei UI", 10);
    applicationFont.setStyleStrategy(static_cast<QFont::StyleStrategy>(QFont::PreferAntialias | QFont::PreferFullHinting));
    application.setFont(applicationFont);

    Janna::MainWindow window;
    window.show();
    return application.exec();
}
