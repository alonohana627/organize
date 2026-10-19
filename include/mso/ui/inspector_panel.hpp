#pragma once
/// @file
/// InspectorPanel: details and organizing fields of the selected samples.
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
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
class QFormLayout;
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
///
/// Drafts: what the user typed or chose and has not applied (name, notes, BPM and key overrides)
/// survives refreshes of the same selection, focused or not, and is discarded when the selection
/// changes — then every field shows the new selection, even one that still has the focus, so an
/// Apply never writes the previous sample's draft to the new one.
///
/// Large selections: the table keeps the keys of every selected row but the data of only about
/// 2,000, so the panel summarizes the loaded rows and says so; an edit still applies to the whole
/// selection (the main window resolves every key). Categories and collections need the libraries
/// of the whole selection, which the window passes along.
///
/// The category and collection lists follow structure changes of the selection's library only,
/// at most one read in flight and at most one start per kChoicesReloadIntervalMilliseconds (an
/// import changes the structure several times a second); a change meanwhile is read afterwards.
class InspectorPanel final : public QWidget {
    Q_OBJECT

public:
    /// Category and collection list reads start at most this often per structure-change burst.
    static constexpr int kChoicesReloadIntervalMilliseconds = 1000;

    /// A panel reading details through `session`, which must outlive it.
    explicit InspectorPanel(ApplicationSession& session, QWidget* parent = nullptr);

    /// Shows the selected rows (loaded rows only). `total_selected` counts loaded and unloaded
    /// rows; when it is larger the panel says that it summarizes the loaded ones. `libraries` are
    /// the libraries of the whole selection (nullopt: those of `rows`, i.e. all rows are loaded).
    void show_selection(std::vector<search::SampleRow> rows, std::size_t total_selected,
                        std::optional<std::set<uuids::uuid>> libraries = std::nullopt);
    /// Disables editing (read-only library or no selection).
    void set_read_only(bool read_only);
    /// Re-reads the single selected sample's details (its row changed in the catalog). Fields the
    /// user is editing keep their values.
    void reload_detail();
    /// Tags, categories or collections of `library` changed: re-reads the category and collection
    /// lists when it is the selection's library (coalesced, see the class comment).
    void library_structure_changed(const uuids::uuid& library);
    /// Category and collection list reads started since construction (tests check the coalescing).
    std::uint64_t choice_reads_started() const { return choice_reads_started_; }

signals:
    /// Apply these fields to the current selection (sample_ids are filled in by the receiver).
    void editRequested(const mso::catalog::EditCommand& command);

private:
    /// Creates every widget and connects the edit buttons, one field group after another.
    void build_ui();
    /// Heading, display name, original file name and format.
    void build_identity_fields(QFormLayout& form, QWidget& content);
    /// Rating stars and favorite.
    void build_rating_fields(QFormLayout& form, QWidget& content);
    /// Notes with Save and Clear.
    void build_notes_fields(QFormLayout& form, QWidget& content);
    /// Tag list with Add and Remove.
    void build_tag_fields(QFormLayout& form, QWidget& content);
    /// Category with Set and Clear.
    void build_category_fields(QFormLayout& form, QWidget& content);
    /// BPM and key overrides.
    void build_override_fields(QFormLayout& form, QWidget& content);
    /// Collections and file locations.
    void build_collection_fields(QFormLayout& form, QWidget& content);
    /// Reads one sample's full detail off the UI thread.
    void load_detail(const search::SampleRow& row);
    /// Shows a sample's detail, leaving drafts untouched.
    void show_detail(const catalog::SampleDetail& detail);
    /// Re-reads the category and collection lists at once (the selection's library changed).
    void reload_library_choices();
    /// Starts the owed list read when none is in flight and the interval has passed.
    void start_due_choices_read();
    /// Reads one library's categories and manual collections for the combo boxes.
    void load_library_choices(const uuids::uuid& library);
    /// Shows what the selected rows have in common ("Mixed" where they differ), diffing against
    /// what is displayed so unchanged fields and drafts are not rewritten.
    void update_summary();
    /// A new selection: discards every draft and shows the new selection in every field.
    void reset_drafts();
    /// Sets the BPM and key override controls from the rows (blank where they differ).
    void show_row_overrides();
    /// Enables the controls that make sense for the selection and read-only state.
    void update_enabled();
    /// Emits editRequested() unless nothing is selected or the library is read-only.
    void request(catalog::EditCommand command);
    /// True when every selected row is loaded (otherwise the summary covers the loaded ones).
    bool whole_selection_loaded() const { return total_selected_ <= rows_.size(); }

    /// Reads details and choices through the Application.
    ApplicationSession& session_;
    std::vector<search::SampleRow> rows_;              ///< selected rows (loaded ones)
    std::set<SampleKey> selected_keys_;                ///< keys of rows_
    std::set<uuids::uuid> selected_libraries_;         ///< libraries of rows_
    std::size_t total_selected_ = 0;                   ///< selected rows, loaded or not
    std::optional<catalog::SampleDetail> detail_;      ///< detail of a single selection, once read
    std::uint64_t detail_request_ = 0;                 ///< drops stale detail deliveries
    std::uint64_t choices_request_ = 0;                ///< drops stale category/collection deliveries
    std::optional<uuids::uuid> choices_library_;       ///< library the combo boxes were last filled for
    int choice_reads_in_flight_ = 0;                   ///< list reads not yet delivered
    bool choices_dirty_ = false;                       ///< the library's structure changed since the last read
    QElapsedTimer choices_last_started_;               ///< when the last list read started
    QTimer choices_timer_;                             ///< starts a deferred list read
    std::uint64_t choice_reads_started_ = 0;           ///< list reads started (tests)
    bool read_only_ = false;                           ///< the selection's library is read-only
    bool notes_dirty_ = false;                         ///< notes typed but not saved
    bool bpm_draft_ = false;                           ///< the BPM override was changed but not applied
    bool key_draft_ = false;                           ///< the key override was changed but not applied
    std::vector<std::int64_t> manual_collection_ids_;  ///< parallel to collection_combo_ entries

    // Widgets (owned through Qt's parent tree), in the order they appear.
    QLabel* heading_label_ = nullptr;        ///< name of the sample or "n samples selected"
    QLabel* partial_label_ = nullptr;        ///< why editing is off (partial selection, errors)
    QLineEdit* name_edit_ = nullptr;         ///< display name
    QPushButton* rename_button_ = nullptr;   ///< applies the display name
    QLabel* original_name_label_ = nullptr;  ///< file name on disk
    QLabel* format_label_ = nullptr;         ///< format, codec, rate, channels, depth, length, size

    StarRatingWidget* rating_stars_ = nullptr;      ///< rating; a click applies it
    QLabel* rating_label_ = nullptr;                ///< "Mixed" for a multi-selection with differing ratings
    QLabel* favorite_label_ = nullptr;              ///< Yes, No or Mixed
    QPushButton* favorite_set_button_ = nullptr;    ///< marks favorite
    QPushButton* favorite_clear_button_ = nullptr;  ///< unmarks favorite

    QPlainTextEdit* notes_edit_ = nullptr;       ///< notes draft
    QPushButton* notes_save_button_ = nullptr;   ///< saves the notes draft
    QPushButton* notes_clear_button_ = nullptr;  ///< clears the notes

    QListWidget* tags_list_ = nullptr;          ///< tags (with "n of m" for a multi-selection)
    QLineEdit* tag_edit_ = nullptr;             ///< tag path to add
    QPushButton* tag_add_button_ = nullptr;     ///< adds the typed tag
    QPushButton* tag_remove_button_ = nullptr;  ///< removes the selected tag

    QLabel* category_label_ = nullptr;              ///< current category or Mixed
    QComboBox* category_combo_ = nullptr;           ///< category to set (editable)
    QPushButton* category_set_button_ = nullptr;    ///< sets the category
    QPushButton* category_clear_button_ = nullptr;  ///< clears the category

    QLabel* bpm_label_ = nullptr;              ///< BPM with provenance
    QComboBox* bpm_mode_combo_ = nullptr;      ///< Automatic / Unknown / Value (blank when mixed)
    QDoubleSpinBox* bpm_spin_ = nullptr;       ///< manual BPM value
    QPushButton* bpm_apply_button_ = nullptr;  ///< applies the BPM override

    QLabel* key_label_ = nullptr;              ///< key with provenance
    QComboBox* key_mode_combo_ = nullptr;      ///< Automatic / Unknown / Value (blank when mixed)
    QComboBox* key_combo_ = nullptr;           ///< manual key value
    QPushButton* key_apply_button_ = nullptr;  ///< applies the key override

    QListWidget* collections_list_ = nullptr;          ///< collections of a single selection
    QComboBox* collection_combo_ = nullptr;            ///< manual collection to add to or remove from
    QPushButton* collection_add_button_ = nullptr;     ///< adds the selection to the collection
    QPushButton* collection_remove_button_ = nullptr;  ///< removes the selection from the collection

    QGroupBox* locations_group_ = nullptr;   ///< file locations of a single selection
    QListWidget* locations_list_ = nullptr;  ///< one line per location
};

}  // namespace mso::ui
