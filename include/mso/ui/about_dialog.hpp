#pragma once
/// @file
/// AboutDialog: version, license and the third-party notices of this build.
#include <QDialog>
#include <QString>

namespace mso::ui {

/// Shows the application's version and GPL-3.0 license and every third-party component with its
/// license notice. The list comes from the configure-time header mso/generated/dependencies.hpp
/// (generated from deps/manifest.json), so it cannot drift from what the build actually ships.
class AboutDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget* parent = nullptr);

    /// The dialog's HTML body (also used by tests to check that every dependency is listed).
    static QString html();
};

}  // namespace mso::ui
