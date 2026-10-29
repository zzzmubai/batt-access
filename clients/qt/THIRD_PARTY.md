# Client dependencies

batt-ops dynamically links the unmodified Qt 6.8.3 Core, GUI, Widgets and Network libraries from the official Windows MinGW distribution. Qt source, notices and license texts are available from [Qt 6.8.3 source archive](https://download.qt.io/archive/qt/6.8/6.8.3/single/) and [Qt licensing](https://doc.qt.io/qt-6/licensing.html).

The local development package includes Qt DLLs/plugins and the MinGW 13.1.0 runtime identified by windeployqt. Its deployment.txt lists the copied components. Qt source can be rebuilt separately and the dynamically linked libraries replaced with compatible builds. Do not describe this artifact as a signed installer or store release.

The source repository does not vendor the Qt SDK, MySQL binary distribution, private credentials or the generated application package. Check the licenses of the selected dependency distributions before distributing a derived product externally. Source and third-party license materials must accompany any external distribution as required by those licenses.
