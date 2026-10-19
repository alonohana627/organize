#pragma once
/// @file
/// WaveformView: paints a min/max/RMS overview with a playhead and lets the user seek by click,
/// drag or arrow keys. Shows explicit "not generated", "generating" and "failed" states.
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <optional>
#include <vector>

namespace mso::ui {

/// Overview bins for painting (copied from analysis::WaveformOverview).
struct WaveformBins {
    std::vector<float> minimum;  ///< [-1, 1]
    std::vector<float> maximum;  ///< [-1, 1]
    std::vector<float> rms;      ///< [0, 1]
};

/// QPainter waveform with seek interaction. Seeking works in every state when a length is known,
/// because the seek bar must not depend on a completed waveform.
///
/// The bins are rendered once into a pixmap at the screen's device-pixel ratio (one column per
/// physical pixel, sharp at 150 % and 200 %) and re-rendered only when the bins, size, palette or
/// screen change; a playhead move repaints just the two thin strips it leaves and enters. For
/// assistive technology the view is a slider whose value is the playhead position in percent.
class WaveformView final : public QWidget {
    Q_OBJECT

public:
    /// What the view currently displays.
    enum class State { NoSample, NotGenerated, Generating, Failed, Ready };

    /// An empty view showing "no sample".
    explicit WaveformView(QWidget* parent = nullptr);

    /// Nothing selected.
    void show_no_sample();
    /// A sample is selected but its waveform has not been decoded.
    void show_not_generated();
    /// Decoding is running; `progress` in [0,1].
    void show_generating(double progress);
    /// Decoding failed with a user-presentable `reason`.
    void show_failed(const QString& reason);
    /// Shows a decoded overview.
    void show_waveform(WaveformBins bins);
    /// What is currently shown.
    State state() const { return state_; }

    /// Playhead position as a fraction 0..1, or nullopt to hide it.
    void set_playhead(std::optional<double> fraction);
    /// Current playhead position, if shown.
    std::optional<double> playhead() const { return playhead_; }
    /// Whether clicking seeks (needs a loaded source with a known length).
    void set_seekable(bool seekable);
    /// Whether seeking is currently possible.
    bool is_seekable() const { return seekable_; }
    /// Asks to seek to `fraction` (0..1) if seeking is possible (mouse, keys, accessibility).
    void request_seek(double fraction);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// The user clicked, dragged or pressed an arrow key to seek to `fraction` of the length.
    void seekRequested(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    /// Area inside the 1-pixel border.
    QRect content_area() const;
    /// Horizontal pixel position of a playhead fraction.
    int playhead_x(double fraction) const;
    /// Repaints the thin strip around a playhead position.
    void update_playhead_strip(std::optional<double> fraction);
    /// Seeks to the fraction under horizontal position `x`.
    void seek_to_x(double x);
    /// Renders the bins into bins_pixmap_ at the current size and device-pixel ratio.
    void render_bins() const;
    /// Drops the cached rendering (bins, size, palette or screen changed).
    void invalidate_rendering();
    /// Describes the current state for screen readers.
    void update_accessible_description();

    /// What is shown.
    State state_ = State::NoSample;
    /// Overview bins (Ready state).
    WaveformBins bins_;
    double progress_ = 0;             ///< generating progress 0..1
    QString failure_;                 ///< reason shown in the Failed state
    std::optional<double> playhead_;  ///< playhead as a fraction of the length, if shown
    /// Whether clicks and keys seek.
    bool seekable_ = false;
    mutable QPixmap bins_pixmap_;             ///< cached rendering of bins_ (painted lazily)
    mutable bool bins_pixmap_valid_ = false;  ///< the cached pixmap matches the bins and size
};

}  // namespace mso::ui
