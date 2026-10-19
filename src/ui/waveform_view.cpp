#include "mso/ui/waveform_view.hpp"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace mso::ui {

namespace {

/// Fraction moved by one arrow-key press (Shift: fine steps).
constexpr double kKeyboardSeekStep = 0.05;
constexpr double kFineKeyboardSeekStep = 0.01;
/// Half width, in pixels, of the strip repainted around the playhead (its pen is 2 px wide).
constexpr int kPlayheadStripHalfWidth = 3;

/// Exposes the waveform to assistive technology as a slider whose value is the playhead position
/// in percent; setting the value seeks.
class AccessibleWaveform final : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit AccessibleWaveform(WaveformView* view) : QAccessibleWidget(view, QAccessible::Slider) {}

    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::ValueInterface) return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QVariant currentValue() const override {
        return static_cast<int>(std::lround(view()->playhead().value_or(0.0) * 100.0));
    }
    void setCurrentValue(const QVariant& value) override { view()->request_seek(value.toDouble() / 100.0); }
    QVariant maximumValue() const override { return 100; }
    QVariant minimumValue() const override { return 0; }
    QVariant minimumStepSize() const override { return 1; }

private:
    WaveformView* view() const { return static_cast<WaveformView*>(widget()); }
};

/// QAccessible factory for WaveformView (installed once, on first construction).
QAccessibleInterface* accessible_waveform_factory(const QString& class_name, QObject* object) {
    if (class_name == QLatin1String("mso::ui::WaveformView") && object && object->isWidgetType())
        return new AccessibleWaveform(static_cast<WaveformView*>(object));
    return nullptr;
}

}  // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    static const bool accessibility_installed = [] {
        QAccessible::installFactory(&accessible_waveform_factory);
        return true;
    }();
    Q_UNUSED(accessibility_installed);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Waveform and seek position"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    update_accessible_description();
}

void WaveformView::show_no_sample() {
    state_ = State::NoSample;
    bins_ = {};
    playhead_.reset();
    invalidate_rendering();
    update_accessible_description();
    update();
}

void WaveformView::show_not_generated() {
    state_ = State::NotGenerated;
    bins_ = {};
    invalidate_rendering();
    update_accessible_description();
    update();
}

void WaveformView::show_generating(double progress) {
    state_ = State::Generating;
    progress_ = std::clamp(progress, 0.0, 1.0);
    update_accessible_description();
    update();
}

void WaveformView::show_failed(const QString& reason) {
    state_ = State::Failed;
    failure_ = reason;
    bins_ = {};
    invalidate_rendering();
    update_accessible_description();
    update();
}

void WaveformView::show_waveform(WaveformBins bins) {
    state_ = State::Ready;
    bins_ = std::move(bins);
    invalidate_rendering();
    update_accessible_description();
    update();
}

void WaveformView::set_playhead(std::optional<double> fraction) {
    if (fraction) fraction = std::clamp(*fraction, 0.0, 1.0);
    if (fraction == playhead_) return;
    // Repaint only where the playhead was and where it is now, not the whole waveform.
    update_playhead_strip(playhead_);
    playhead_ = fraction;
    update_playhead_strip(playhead_);
}

void WaveformView::set_seekable(bool seekable) {
    if (seekable == seekable_) return;
    seekable_ = seekable;
    setCursor(seekable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void WaveformView::request_seek(double fraction) {
    if (seekable_) emit seekRequested(std::clamp(fraction, 0.0, 1.0));
}

QSize WaveformView::sizeHint() const {
    return {480, 96};
}

QSize WaveformView::minimumSizeHint() const {
    return {120, 48};
}

QRect WaveformView::content_area() const {
    return rect().adjusted(1, 1, -1, -1);
}

int WaveformView::playhead_x(double fraction) const {
    const QRect area = content_area();
    return area.left() + static_cast<int>(std::lround(fraction * area.width()));
}

void WaveformView::update_playhead_strip(std::optional<double> fraction) {
    if (!fraction) return;
    const int x = playhead_x(*fraction);
    update(QRect(x - kPlayheadStripHalfWidth, 0, 2 * kPlayheadStripHalfWidth + 1, height()));
}

void WaveformView::invalidate_rendering() {
    bins_pixmap_valid_ = false;
    bins_pixmap_ = QPixmap();
}

void WaveformView::resizeEvent(QResizeEvent* event) {
    invalidate_rendering();
    QWidget::resizeEvent(event);
}

void WaveformView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::DevicePixelRatioChange)
        invalidate_rendering();
    QWidget::changeEvent(event);
}

void WaveformView::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setClipRegion(event->region());
    const QRect area = content_area();
    painter.fillRect(rect(), palette().color(QPalette::Base));

    QString message;
    switch (state_) {
        case State::NoSample:
            message = tr("No sample selected");
            break;
        case State::NotGenerated:
            message = tr("Waveform not generated — play the sample or click Generate waveform");
            break;
        case State::Generating:
            message = tr("Generating waveform… %1%").arg(std::lround(progress_ * 100.0));
            break;
        case State::Failed:
            message = tr("Waveform failed: %1").arg(failure_);
            break;
        case State::Ready:
            if (!bins_pixmap_valid_ || bins_pixmap_.devicePixelRatio() != devicePixelRatioF()) render_bins();
            painter.drawPixmap(area.topLeft(), bins_pixmap_);
            break;
    }
    if (state_ == State::Generating) {
        QRect bar = area;
        bar.setTop(area.bottom() - 3);
        bar.setWidth(static_cast<int>(area.width() * progress_));
        painter.fillRect(bar, palette().color(QPalette::Highlight));
    }
    if (!message.isEmpty()) {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap, message);
    }
    if (playhead_) {
        const int x = playhead_x(*playhead_);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawLine(x, area.top(), x, area.bottom());
    }
    if (hasFocus()) {
        // Visible keyboard focus, independent of the platform style.
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void WaveformView::render_bins() const {
    const QRect area = content_area();
    const qreal ratio = devicePixelRatioF();
    const int columns = static_cast<int>(std::lround(area.width() * ratio));
    const int rows = static_cast<int>(std::lround(area.height() * ratio));
    bins_pixmap_valid_ = true;
    if (columns <= 0 || rows <= 0) {
        bins_pixmap_ = QPixmap();
        return;
    }
    bins_pixmap_ = QPixmap(columns, rows);
    bins_pixmap_.setDevicePixelRatio(ratio);
    bins_pixmap_.fill(palette().color(QPalette::Base));

    const std::size_t bin_count = std::min({bins_.minimum.size(), bins_.maximum.size(), bins_.rms.size()});
    if (bin_count == 0) return;
    QPainter painter(&bins_pixmap_);
    painter.scale(1.0 / ratio, 1.0 / ratio);  // draw in physical pixels: one column each
    const double middle = rows / 2.0;
    const double half_height = rows / 2.0;
    const QColor peak_color = palette().color(QPalette::Text);
    QColor rms_color = palette().color(QPalette::Highlight);
    rms_color.setAlpha(170);
    // Fold the bins that fall into each physical column into their extremes.
    for (int column = 0; column < columns; ++column) {
        const auto first = static_cast<std::size_t>(static_cast<double>(column) * bin_count / columns);
        auto last = static_cast<std::size_t>(static_cast<double>(column + 1) * bin_count / columns);
        last = std::clamp(last, first + 1, bin_count);
        float low = 1.0f, high = -1.0f, rms = 0.0f;
        for (std::size_t bin = first; bin < last; ++bin) {
            low = std::min(low, bins_.minimum[bin]);
            high = std::max(high, bins_.maximum[bin]);
            rms = std::max(rms, bins_.rms[bin]);
        }
        painter.setPen(peak_color);
        painter.drawLine(column, static_cast<int>(middle - high * half_height), column,
                         static_cast<int>(middle - low * half_height));
        painter.setPen(rms_color);
        painter.drawLine(column, static_cast<int>(middle - rms * half_height), column,
                         static_cast<int>(middle + rms * half_height));
    }
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) seek_to_x(event->position().x());
    QWidget::mousePressEvent(event);
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) seek_to_x(event->position().x());
    QWidget::mouseMoveEvent(event);
}

void WaveformView::keyPressEvent(QKeyEvent* event) {
    if (!seekable_ || (event->key() != Qt::Key_Left && event->key() != Qt::Key_Right)) {
        QWidget::keyPressEvent(event);
        return;
    }
    const double step = (event->modifiers() & Qt::ShiftModifier) ? kFineKeyboardSeekStep : kKeyboardSeekStep;
    const double direction = event->key() == Qt::Key_Left ? -1.0 : 1.0;
    request_seek(playhead_.value_or(0.0) + direction * step);
}

void WaveformView::seek_to_x(double x) {
    const QRect area = content_area();
    if (area.width() <= 0) return;
    request_seek((x - area.left()) / area.width());
}

void WaveformView::update_accessible_description() {
    switch (state_) {
        case State::NoSample:
            setAccessibleDescription(tr("No sample selected"));
            break;
        case State::NotGenerated:
            setAccessibleDescription(tr("Waveform not generated"));
            break;
        case State::Generating:
            setAccessibleDescription(tr("Generating waveform"));
            break;
        case State::Failed:
            setAccessibleDescription(tr("Waveform failed: %1").arg(failure_));
            break;
        case State::Ready:
            setAccessibleDescription(tr("Waveform shown; use the arrow keys to seek"));
            break;
    }
}

}  // namespace mso::ui
