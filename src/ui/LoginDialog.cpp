#include "LoginDialog.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPixmap>
#include <QIcon>

namespace zhi::ui {

// 硬编码的账号密码（见头文件说明：这是"仪式感"登录，不是真正的访问控制）
static const QString kFixedUsername = "XIAOZHI";
static const QString kFixedPassword = "LOVE-XIAOZHI";

LoginDialog::LoginDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("ZHI-Flashtools 登录");
    setFixedSize(380, 440);
    setStyleSheet(BuildStyleSheet(false));
    // 去掉右上角的"?"帮助按钮，只留关闭
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(36, 30, 36, 30);
    layout->setSpacing(14);

    // ---- 图标 ----
    auto* iconLabel = new QLabel(this);
    QPixmap logo(":/resources/app-icon.ico");
    if (!logo.isNull()) {
        iconLabel->setPixmap(logo.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel);

    // ---- 标题 ----
    auto* title = new QLabel("ZHI-Flashtools", this);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:20px; font-weight:800; color:#14161b;");
    layout->addWidget(title);

    auto* subtitle = new QLabel("刷机 · 救砖 · 维修工具", this);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setStyleSheet("font-size:12px; color:#6b7280;");
    layout->addWidget(subtitle);

    layout->addSpacing(10);

    // ---- 账号 ----
    usernameEdit_ = new QLineEdit(this);
    usernameEdit_->setPlaceholderText("账号");
    usernameEdit_->setMinimumHeight(38);
    usernameEdit_->setStyleSheet(
        "QLineEdit{border:1px solid #c9cedb; border-radius:10px; padding:0 14px; "
        "font-size:13px; background:#ffffff;}"
        "QLineEdit:focus{border-color:#2454d6;}");
    layout->addWidget(usernameEdit_);

    // ---- 密码 ----
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setPlaceholderText("密码");
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setMinimumHeight(38);
    passwordEdit_->setStyleSheet(
        "QLineEdit{border:1px solid #c9cedb; border-radius:10px; padding:0 14px; "
        "font-size:13px; background:#ffffff;}"
        "QLineEdit:focus{border-color:#2454d6;}");
    layout->addWidget(passwordEdit_);

    // ---- 错误提示 ----
    errorLabel_ = new QLabel("", this);
    errorLabel_->setStyleSheet("color:#d3271f; font-size:11.5px;");
    errorLabel_->setAlignment(Qt::AlignCenter);
    errorLabel_->setFixedHeight(16);
    layout->addWidget(errorLabel_);

    // ---- 登录按钮 ----
    auto* loginBtn = new QPushButton("登 录", this);
    loginBtn->setObjectName("primary");
    loginBtn->setMinimumHeight(40);
    loginBtn->setCursor(Qt::PointingHandCursor);
    connect(loginBtn, &QPushButton::clicked, this, &LoginDialog::OnLoginClicked);
    layout->addWidget(loginBtn);

    // 回车键也能登录
    connect(passwordEdit_, &QLineEdit::returnPressed, this, &LoginDialog::OnLoginClicked);
    connect(usernameEdit_, &QLineEdit::returnPressed, this, &LoginDialog::OnLoginClicked);

    layout->addStretch();

    auto* footer = new QLabel("© ZHI-Flashtools · 公益免费", this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet("font-size:10px; color:#9aa1ad;");
    layout->addWidget(footer);
}

void LoginDialog::OnLoginClicked() {
    QString user = usernameEdit_->text().trimmed();
    QString pass = passwordEdit_->text();

    if (user == kFixedUsername && pass == kFixedPassword) {
        accept(); // 关闭对话框并返回 QDialog::Accepted
        return;
    }

    failCount_++;
    errorLabel_->setText("账号或密码错误");
    passwordEdit_->clear();
    passwordEdit_->setFocus();

    if (failCount_ >= 5) {
        errorLabel_->setText("多次错误，请确认账号密码后重试");
    }
}

} // namespace zhi::ui
