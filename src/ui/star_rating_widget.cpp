#include "mso/ui/star_rating_widget.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionFocusRect>

#include <algorithm>

#include "mso/ui/display_format.hpp"
#include "mso/ui/star_rating.hpp"

namespace mso::ui {

namespace {

constexpr int kFocusMargin = 3;

/// Rating for a digit key 0–5, or -1 for any other key.
int rating_for_key(int key) {
    if (key >= Qt::Key_0 && key <= Qt::Key_5) return key - Qt::Key_0;
    return -1;
}

/// Exposes the stars to assistive technology as a slider from 0 to 5; setting the value rates.
class AccessibleStarRating final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit AccessibleStarRating(StarRatingWidget* stars) : QAccessibleWidget(stars, QAccessible::Slider) {}

    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface) return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QVariant currentValue() const override { return stars()->rating(); }
    void setCurrentValue(const QVariant& value) override {
        emit stars() -> ratingChosen(std::clamp(value.toInt(), 0, StarRating::kMaximumRating));
    }
    QVariant maximumValue() const override { return StarRating::kMaximumRating; }
    QVariant minimumValue() const override { return 0; }
    QVariant minimumStepSize() const override { return 1; }

private:
    StarRatingWidget* stars() const { return static_cast<StarRatingWidget*>(widget()); }
};

/// QAccessible factory for StarRatingWidget (installed once, on first construction).
QAccessibleInterface* accessible_star_rating_factory(const QString& class_name, QObject* object) {
    if (class_name == QLatin1String("mso::ui::StarRatingWidget") && object && object->isWidgetType())
        return new AccessibleStarRating(static_cast<StarRatingWidget*>(object));
    return nullptr;
}

}  // namespace

StarRatingWidget::StarRatingWidget(QWidget* parent) : QWidget(parent) {
    static const bool accessibility_installed = [] {
        QAccessible::installFactory(&accessible_star_rating_factory);
        return true;
    }();
    Q_UNUSED(accessibility_installed);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(tr("Rating"));
    setToolTip(tr("Click a star to rate; click the current rating again to clear it. Keys 0–5 and arrows work too."));
    update_accessible_text();
}

void StarRatingWidget::set_rating(int rating) {
    rating = std::clamp(rating, 0, StarRating::kMaximumRating);
    if (rating == rating_ && !mixed_) return;
    rating_ = rating;
    mixed_ = false;
    update_accessible_text();
    QAccessibleValueChangeEvent changed(this, rating_);
    QAccessible::updateAccessibility(&changed);
    update();
}

void StarRatingWidget::set_mixed(bool mixed) {
    if (mixed == mixed_ && (!mixed || rating_ == 0)) return;
    mixed_ = mixed;
    if (mixed) rating_ = 0;
    update_accessible_text();
    update();
}

int StarRatingWidget::star_size() const {
    return std::max(12, fontMetrics().height());
}

QRect StarRatingWidget::strip_area() const {
    return rect().adjusted(kFocusMargin, kFocusMargin, -kFocusMargin, -kFocusMargin);
}

QSize StarRatingWidget::sizeHint() const {
    return StarRating::strip_size(star_size()) + QSize(2 * kFocusMargin, 2 * kFocusMargin);
}

QSize StarRatingWidget::minimumSizeHint() const {
    return sizeHint();
}

bool StarRatingWidget::event(QEvent* event) {
    // Digits and arrows belong to this control while it has focus, not to the window's rating
    // shortcuts (which act on the whole selection the same way, but would bypass hover state).
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        const int key = key_event->key();
        if (key_event->modifiers() == Qt::NoModifier &&
            (rating_for_key(key) >= 0 || key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
             key == Qt::Key_Down)) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void StarRatingWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    StarRating::Appearance appearance;
    appearance.rating = mixed_ ? 0 : rating_;
    appearance.preview = isEnabled() ? hovered_star_ : 0;
    appearance.enabled = isEnabled();
    const QPalette::ColorGroup group = isActiveWindow() ? QPalette::Active : QPalette::Inactive;
    StarRating::paint(painter, strip_area(), star_size(), appearance, palette(), group);
    if (hasFocus()) {
        QStyleOptionFocusRect option;
        option.initFrom(this);
        option.backgroundColor = palette().color(QPalette::Window);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
    }
}

void StarRatingWidget::mouseMoveEvent(QMouseEvent* event) {
    const int star = StarRating::star_at(strip_area(), star_size(), event->position().toPoint());
    if (star != hovered_star_) {
        hovered_star_ = star;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void StarRatingWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const int star = StarRating::star_at(strip_area(), star_size(), event->position().toPoint());
        if (star > 0) {
            choose(star);
            event->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(event);
}

void StarRatingWidget::leaveEvent(QEvent* event) {
    hovered_star_ = 0;
    update();
    QWidget::leaveEvent(event);
}

void StarRatingWidget::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (const int rating = rating_for_key(key); rating >= 0) {
        emit ratingChosen(rating);  // a typed digit is always explicit, never a toggle
    } else if (key == Qt::Key_Left || key == Qt::Key_Down) {
        emit ratingChosen(std::max(0, rating_ - 1));
    } else if (key == Qt::Key_Right || key == Qt::Key_Up) {
        emit ratingChosen(std::min(StarRating::kMaximumRating, rating_ + 1));
    } else {
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void StarRatingWidget::choose(int star) {
    // Clicking the current rating clears it, so a rating can be removed with the mouse alone.
    emit ratingChosen(!mixed_ && star == rating_ ? 0 : star);
}

void StarRatingWidget::update_accessible_text() {
    setAccessibleDescription(mixed_ ? tr("Mixed ratings") : DisplayFormat::rating_accessible(rating_));
}

}  // namespace mso::ui
