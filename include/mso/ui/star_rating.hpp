#pragma once
/// @file
/// StarRating: geometry, hit-testing and painting of a 0–5 star strip, shared by the table's
/// rating column and the Inspector's StarRatingWidget so both map clicks to stars identically.
#include <QPalette>
#include <QPoint>
#include <QRect>
#include <QSize>

class QPainter;
class QPainterPath;

namespace mso::ui {

/// Stateless star-strip helper. The strip is drawn left-aligned and vertically centred in an
/// area; star `n` (1-based) occupies one square slot of `star_size` pixels plus spacing.
class StarRating {
public:
    static constexpr int kMaximumRating = 5;

    /// Pixel gap between two stars, proportional to the star size.
    static int spacing(int star_size);
    /// Size of the whole strip.
    static QSize strip_size(int star_size);
    /// Bounding square of star `index` (1..5) inside `area`.
    static QRect star_rect(const QRect& area, int star_size, int index);
    /// The star (1..5) under `position`, or 0 when the position is outside the strip. A position in
    /// the gap between two stars belongs to the star on its left, so the strip has no dead spots.
    static int star_at(const QRect& area, int star_size, const QPoint& position);

    /// What the strip should look like.
    struct Appearance {
        int rating = 0;             ///< filled stars
        int preview = 0;            ///< hover preview (0 = none): exactly this many stars, highlighted
        bool on_selection = false;  ///< drawn over the selection band: use the highlighted-text color
        bool enabled = true;
    };
    /// Paints the strip with palette colors only, so light and dark themes both work.
    static void paint(QPainter& painter, const QRect& area, int star_size, const Appearance& appearance,
                      const QPalette& palette, QPalette::ColorGroup group);

private:
    /// Five-pointed star outline fitted into `box`.
    static QPainterPath star_path(const QRectF& box);
};

}  // namespace mso::ui
