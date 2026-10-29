#include "window.hpp"
#include <QApplication>
#include <QTimer>
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Window window;
    if (application.arguments().contains("--smoke-test")) QTimer::singleShot(200,&application,&QCoreApplication::quit);
    else window.show();
    return application.exec();
}
