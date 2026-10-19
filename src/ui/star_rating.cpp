#include "mso/ui/star_rating.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mso::ui {

int StarRating::spacing(int star_size) {
    return std::max(1, star_size / 5);
}

QSize StarRating::strip_size(int star_size) {
    return {kMaximumRating * star_size + (kMaximumRating - 1) * spacing(star_size), star_size};
}

QRect StarRating::star_rect(const QRect& area, int star_size, int index) {
    const int top = area.top() + (area.height() - star_size) / 2;
    const int left = area.left() + (index - 1) * (star_size + spacing(star_size));
    return {left, top, star_size, star_size};
}

int StarRating::star_at(const QRect& area, int star_size, const QPoint& position) {
    const int strip_width = strip_size(star_size).width();
    const int offset = position.x() - area.left();
    if (!area.contains(position) || offset < 0 || offset >= strip_width) return 0;
    return std::clamp(offset / (star_size + spacing(star_size)) + 1, 1, kMaximumRating);
}

QPainterPath StarRating::star_path(const QRectF& box) {
    // Ten vertices alternating between the outer and inner radius, first point straight up.
    constexpr int kPoints = 5;
    constexpr double kInnerRatio = 0.45;
    const QPointF centre = box.center();
    const double outer = std::min(box.width(), box.height()) / 2.0;
    QPainterPath path;
    for (int vertex = 0; vertex < 2 * kPoints; ++vertex) {
        const double radius = vertex % 2 == 0 ? outer : outer * kInnerRatio;
        const double angle = -std::numbers::pi / 2.0 + vertex * std::numbers::pi / kPoints;
        const QPointF point(centre.x() + radius * std::cos(angle),
                            centre.y() + radius * std::sin(angle) + outer * 0.06);
        if (vertex == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

void StarRating::paint(QPainter& painter, const QRect& area, int star_size, const Appearance& appearance,
                       const QPalette& palette, QPalette::ColorGroup group) {
    const QPalette::ColorGroup color_group = appearance.enabled ? group : QPalette::Disabled;
    const QColor filled =
        palette.color(color_group, appearance.on_selection ? QPalette::HighlightedText : QPalette::Text);
    QColor outline = filled;
    outline.setAlphaF(0.45f);
    const QColor preview = appearance.on_selection ? filled : palette.color(color_group, QPalette::Highlight);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int index = 1; index <= kMaximumRating; ++index) {
        const QPainterPath path = star_path(QRectF(star_rect(area, star_size, index)).adjusted(0.5, 0.5, -0.5, -0.5));
        // While previewing, show exactly the rating a click would set.
        const bool previewed = appearance.preview > 0 && index <= appearance.preview;
        const bool rated = appearance.preview == 0 && index <= appearance.rating;
        if (previewed) {
            painter.fillPath(path, preview);
        } else if (rated) {
            painter.fillPath(path, filled);
        } else {
            painter.setPen(QPen(outline, 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
    }
    painter.restore();
}

}  // namespace mso::ui
