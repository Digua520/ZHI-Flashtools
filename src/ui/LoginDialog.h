#pragma once
// ============================================================================
// LoginDialog —— 启动时的登录界面
//
// 说明（老实话）：按你的要求，这是一个"仪式感"登录框，账号密码是硬编码在
// 程序里的（XIAOZHI / LOVE-XIAOZHI）。必须清楚一点：硬编码的密码用任何
// 十六进制/字符串查看工具（比如 Windows 下的 strings 工具）都能从 exe 里
// 直接读出明文，所以这个登录框拦不住真想绕过的人，它的作用是"门面/仪式感"，
// 不是真正的访问控制。如果之后要做真正能管用户的登录（每人一个号、能封号），
// 需要接后端服务器验证（项目里已经有一个 zhi-auth-server 的雏形），那才是
// 密码不落地到客户端的正确做法。
// ============================================================================

#include <QDialog>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QLabel;
QT_END_NAMESPACE

namespace zhi::ui {

class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);

private slots:
    void OnLoginClicked();

private:
    QLineEdit* usernameEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    int failCount_ = 0;
};

} // namespace zhi::ui
