#include "window.hpp"
#include <QApplication>
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ProbeWindow window;
    window.resize(600, 160); window.show();
    return application.exec();
}
