#pragma once
/// @file
/// WaveformView: paints a min/max/RMS overview with a playhead and lets the user seek by click,
/// drag or arrow keys. Shows explicit "not generated", "generating" and "failed" states.
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
class WaveformView final : public QWidget {
    Q_OBJECT

public:
    /// What the view currently displays.
    enum class State { NoSample, NotGenerated, Generating, Failed, Ready };

    explicit WaveformView(QWidget* parent = nullptr);

    void show_no_sample();
    void show_not_generated();
    /// `progress` in [0,1].
    void show_generating(double progress);
    void show_failed(const QString& reason);
    void show_waveform(WaveformBins bins);
    State state() const { return state_; }

    /// Playhead position as a fraction 0..1, or nullopt to hide it.
    void set_playhead(std::optional<double> fraction);
    /// Whether clicking seeks (needs a loaded source with a known length).
    void set_seekable(bool seekable);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    /// The user clicked, dragged or pressed an arrow key to seek to `fraction` of the length.
    void seekRequested(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void seek_to_x(double x);
    void paint_bins(QPainter& painter, const QRect& area) const;
    void update_accessible_description();

    State state_ = State::NoSample;
    WaveformBins bins_;
    double progress_ = 0;
    QString failure_;
    std::optional<double> playhead_;
    bool seekable_ = false;
};

}  // namespace mso::ui
