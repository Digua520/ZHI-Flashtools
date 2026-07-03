#include "MainWindow.h"
#include "LoginDialog.h"
#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("ZHI-Flashtools");

    // 先弹登录框，验证通过（点了"登录"且账号密码正确 -> accept）才进主界面；
    // 用户直接关掉登录框（reject）就退出程序，不进主界面。
    zhi::ui::LoginDialog login;
    if (login.exec() != QDialog::Accepted) {
        return 0;
    }

    zhi::ui::MainWindow window;
    window.show();

    return app.exec();
}
