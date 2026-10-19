#include "mso/ui/waveform_view.hpp"

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

}  // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Waveform and seek position"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    update_accessible_description();
}

void WaveformView::show_no_sample() {
    state_ = State::NoSample;
    bins_ = {};
    playhead_.reset();
    update_accessible_description();
    update();
}

void WaveformView::show_not_generated() {
    state_ = State::NotGenerated;
    bins_ = {};
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
    update_accessible_description();
    update();
}

void WaveformView::show_waveform(WaveformBins bins) {
    state_ = State::Ready;
    bins_ = std::move(bins);
    update_accessible_description();
    update();
}

void WaveformView::set_playhead(std::optional<double> fraction) {
    if (fraction) fraction = std::clamp(*fraction, 0.0, 1.0);
    if (fraction == playhead_) return;
    playhead_ = fraction;
    update();
}

void WaveformView::set_seekable(bool seekable) {
    seekable_ = seekable;
    setCursor(seekable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

QSize WaveformView::sizeHint() const {
    return {480, 96};
}
QSize WaveformView::minimumSizeHint() const {
    return {120, 48};
}

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRect area = rect().adjusted(1, 1, -1, -1);
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
            paint_bins(painter, area);
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
        const int x = area.left() + static_cast<int>(std::lround(*playhead_ * area.width()));
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.drawLine(x, area.top(), x, area.bottom());
    }
    if (hasFocus()) {
        // Visible keyboard focus, independent of the platform style.
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DashLine));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
}

void WaveformView::paint_bins(QPainter& painter, const QRect& area) const {
    const std::size_t bin_count = std::min({bins_.minimum.size(), bins_.maximum.size(), bins_.rms.size()});
    if (bin_count == 0 || area.width() <= 0) return;
    const double middle = area.center().y();
    const double half_height = area.height() / 2.0;
    const QColor peak_color = palette().color(QPalette::Text);
    QColor rms_color = palette().color(QPalette::Highlight);
    rms_color.setAlpha(170);
    // One column per pixel: fold the bins that fall into each column into their extremes.
    for (int column = 0; column < area.width(); ++column) {
        const auto first = static_cast<std::size_t>(static_cast<double>(column) * bin_count / area.width());
        auto last = static_cast<std::size_t>(static_cast<double>(column + 1) * bin_count / area.width());
        last = std::clamp(last, first + 1, bin_count);
        float low = 1.0f, high = -1.0f, rms = 0.0f;
        for (std::size_t bin = first; bin < last; ++bin) {
            low = std::min(low, bins_.minimum[bin]);
            high = std::max(high, bins_.maximum[bin]);
            rms = std::max(rms, bins_.rms[bin]);
        }
        const int x = area.left() + column;
        painter.setPen(peak_color);
        painter.drawLine(x, static_cast<int>(middle - high * half_height), x,
                         static_cast<int>(middle - low * half_height));
        painter.setPen(rms_color);
        painter.drawLine(x, static_cast<int>(middle - rms * half_height), x,
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
    emit seekRequested(std::clamp(playhead_.value_or(0.0) + direction * step, 0.0, 1.0));
}

void WaveformView::seek_to_x(double x) {
    if (!seekable_ || width() <= 2) return;
    emit seekRequested(std::clamp((x - 1.0) / (width() - 2.0), 0.0, 1.0));
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
