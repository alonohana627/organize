#include "mso/ui/notification_bar.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QToolButton>

namespace mso::ui {

NotificationBar::NotificationBar(QWidget* parent) : QFrame(parent) {
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::ToolTipBase);
    setForegroundRole(QPalette::ToolTipText);
    setAccessibleName(tr("Notification"));

    icon_label_ = new QLabel(this);
    const int icon_size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    icon_label_->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(icon_size, icon_size));
    text_label_ = new QLabel(this);
    text_label_->setWordWrap(true);
    text_label_->setTextFormat(Qt::RichText);
    text_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text_label_->setForegroundRole(QPalette::ToolTipText);
    close_button_ = new QToolButton(this);
    close_button_->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    close_button_->setAutoRaise(true);
    close_button_->setAccessibleName(tr("Dismiss notification"));
    connect(close_button_, &QToolButton::clicked, this, &NotificationBar::dismiss);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 4, 4);
    layout->addWidget(icon_label_, 0, Qt::AlignTop);
    layout->addWidget(text_label_, 1);
    layout->addWidget(close_button_, 0, Qt::AlignTop);
    hide();
}

void NotificationBar::show_error(const QString& title, const QString& details) {
    text_label_->setText(
        QStringLiteral("<b>%1</b><br>%2")
            .arg(title.toHtmlEscaped(), details.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
    setAccessibleDescription(QStringLiteral("%1. %2").arg(title, details));
    show();
}

void NotificationBar::dismiss() {
    text_label_->clear();
    setAccessibleDescription({});
    hide();
}

QString NotificationBar::message() const {
    return isHidden() ? QString() : accessibleDescription();
}

}  // namespace mso::ui
