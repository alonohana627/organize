#pragma once
/// @file
/// StarRatingWidget: a clickable 0–5 star rating control (Inspector).
#include <QWidget>

namespace mso::ui {

/// Five stars. Hovering previews a rating, clicking a star chooses it, and clicking the star of the
/// current rating clears it to 0. With keyboard focus, 0–5 choose directly and Left/Right (or
/// Down/Up) step by one. The control only reports choices (ratingChosen); the owner applies them
/// through the undoable edit path and then calls set_rating() with the stored value.
class StarRatingWidget final : public QWidget {
    Q_OBJECT

public:
    /// Five empty stars.
    explicit StarRatingWidget(QWidget* parent = nullptr);

    int rating() const { return rating_; }
    /// Shows a stored rating (0..5) and clears the mixed state.
    void set_rating(int rating);
    /// Multi-selection with differing ratings: all stars hollow, described as "Mixed".
    void set_mixed(bool mixed);
    bool is_mixed() const { return mixed_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// The user chose `rating` (0..5) by mouse or keyboard.
    void ratingChosen(int rating);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Pixel size of one star, derived from the font so it follows the DPI and the user's font.
    int star_size() const;
    /// Where the stars are drawn (inside the focus margin).
    QRect strip_area() const;
    /// Emits ratingChosen for a click on `star`; clicking the current rating clears it to 0.
    void choose(int star);
    /// Describes the rating for screen readers.
    void update_accessible_text();

    /// Shown rating 0..5.
    int rating_ = 0;
    /// Star under the pointer (hover preview), or 0.
    int hovered_star_ = 0;
    /// A multi-selection with differing ratings.
    bool mixed_ = false;
};

}  // namespace mso::ui
