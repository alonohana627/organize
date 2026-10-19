#pragma once
/// @file
/// InspectorPanel: details and organizing fields of the selected samples.
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "mso/catalog/edit_command.hpp"
#include "mso/catalog/types.hpp"
#include "mso/search/search_service.hpp"
#include "mso/ui/sample_key.hpp"

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace mso::ui {

class ApplicationSession;
class StarRatingWidget;

/// Inspector dock (docs/01_REQUIREMENTS.md §6).
///
/// A single selection shows every detail (format, provenance of BPM/key, locations, collections)
/// read asynchronously with Library::sample_detail(). A multi-selection shows "Mixed" wherever
/// values differ and offers explicit Set / Add / Remove / Clear buttons. Nothing is ever written
/// on focus loss: every edit is an explicit button press (or Enter in a single-line field), which
/// emits editRequested() with the fields to change; the main window applies it to the selection.
class InspectorPanel final : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Shows the selected rows (loaded rows only). `total_selected` may be larger when some
    /// selected rows are not loaded; edits then apply to the loaded ones, which the panel says.
    void show_selection(std::vector<search::SampleRow> rows, std::size_t total_selected);
    /// Disables editing (read-only library or no selection).
    void set_read_only(bool read_only);
    /// Re-reads the single selected sample's details (its row changed in the catalog). Fields the
    /// user is editing keep their values.
    void reload_detail();
    /// Re-reads the category and collection lists (taxonomy changed, or the selection's library).
    void reload_library_choices();

signals:
    /// Apply these fields to the current selection (sample_ids are filled in by the receiver).
    void editRequested(const mso::catalog::EditCommand& command);

private:
    /// Creates every widget and connects the edit buttons.
    void build_ui();
    /// Reads one sample's full detail off the UI thread.
    void load_detail(const search::SampleRow& row);
    /// Shows a sample's detail, leaving fields the user is editing untouched.
    void show_detail(const catalog::SampleDetail& detail);
    /// Reads one library's categories and manual collections for the combo boxes.
    void load_library_choices(const uuids::uuid& library);
    /// Shows what the selected rows have in common ("Mixed" where they differ), diffing against
    /// what is displayed so unchanged and focused fields are not rewritten.
    void update_summary();
    /// Enables the controls that make sense for the selection and read-only state.
    void update_enabled();
    /// Emits editRequested() unless nothing is selected or the library is read-only.
    void request(catalog::EditCommand command);

    /// Reads details and choices through the Application.
    ApplicationSession& session_;
    std::vector<search::SampleRow> rows_;          ///< selected rows (loaded ones)
    std::set<SampleKey> selected_keys_;            ///< keys of rows_
    std::set<uuids::uuid> selected_libraries_;     ///< libraries of rows_
    std::size_t total_selected_ = 0;               ///< selected rows, loaded or not
    std::optional<catalog::SampleDetail> detail_;  ///< detail of a single selection, once read
    std::uint64_t detail_request_ = 0;             ///< drops stale detail deliveries
    std::uint64_t choices_request_ = 0;            ///< drops stale category/collection deliveries
    bool read_only_ = false;
    bool notes_dirty_ = false;                         ///< notes typed but not saved
    std::vector<std::int64_t> manual_collection_ids_;  ///< parallel to collection_combo_ entries

    /// Widgets (owned through Qt's parent tree), in the order they appear.
    QLabel* heading_label_ = nullptr;
    QLabel* partial_label_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QPushButton* rename_button_ = nullptr;
    QLabel* original_name_label_ = nullptr;
    QLabel* format_label_ = nullptr;

    StarRatingWidget* rating_stars_ = nullptr;
    QLabel* rating_label_ = nullptr;  ///< "Mixed" for a multi-selection with differing ratings
    QLabel* favorite_label_ = nullptr;
    QPushButton* favorite_set_button_ = nullptr;
    QPushButton* favorite_clear_button_ = nullptr;

    QPlainTextEdit* notes_edit_ = nullptr;
    QPushButton* notes_save_button_ = nullptr;
    QPushButton* notes_clear_button_ = nullptr;

    QListWidget* tags_list_ = nullptr;
    QLineEdit* tag_edit_ = nullptr;
    QPushButton* tag_add_button_ = nullptr;
    QPushButton* tag_remove_button_ = nullptr;

    QLabel* category_label_ = nullptr;
    QComboBox* category_combo_ = nullptr;
    QPushButton* category_set_button_ = nullptr;
    QPushButton* category_clear_button_ = nullptr;

    QLabel* bpm_label_ = nullptr;
    QComboBox* bpm_mode_combo_ = nullptr;
    QDoubleSpinBox* bpm_spin_ = nullptr;
    QPushButton* bpm_apply_button_ = nullptr;

    QLabel* key_label_ = nullptr;
    QComboBox* key_mode_combo_ = nullptr;
    QComboBox* key_combo_ = nullptr;
    QPushButton* key_apply_button_ = nullptr;

    QListWidget* collections_list_ = nullptr;
    QComboBox* collection_combo_ = nullptr;
    QPushButton* collection_add_button_ = nullptr;
    QPushButton* collection_remove_button_ = nullptr;

    QGroupBox* locations_group_ = nullptr;
    QListWidget* locations_list_ = nullptr;
};

}  // namespace mso::ui
