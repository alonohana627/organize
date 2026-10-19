#include "mso/ui/inspector_panel.hpp"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/sample_key.hpp"
#include "mso/ui/star_rating_widget.hpp"

namespace mso::ui {

namespace {

/// Index of the override modes in the BPM/key mode combo boxes (-1: the selection differs).
enum OverrideChoice { MixedChoice = -1, AutomaticChoice = 0, UnknownChoice = 1, ValueChoice = 2 };

constexpr double kMinimumBpm = 20.0;
constexpr double kMaximumBpm = 400.0;
constexpr double kDefaultBpm = 120.0;

/// Keys of `rows`.
std::set<SampleKey> keys_of(const std::vector<search::SampleRow>& rows) {
    std::set<SampleKey> keys;
    for (const auto& row : rows)
        keys.insert(SampleKey{row.library, row.sample_id});
    return keys;
}

/// Libraries of `rows`.
std::set<uuids::uuid> libraries_of(const std::vector<search::SampleRow>& rows) {
    std::set<uuids::uuid> libraries;
    for (const auto& row : rows)
        libraries.insert(row.library);
    return libraries;
}

/// Sets a line edit's text unless the user has typed into it (a draft), and without touching it
/// when the text is already right: refreshes never clobber a rename in progress.
void set_text_unless_edited(QLineEdit* edit, const QString& text) {
    if (edit->isModified() || edit->text() == text) return;
    const QSignalBlocker blocker(edit);
    edit->setText(text);
}

/// Rebuilds a list only when its entries (display text + value) differ, keeping the current item.
void set_list_entries(QListWidget* list, const std::vector<std::pair<QString, QString>>& entries) {
    bool same = list->count() == static_cast<int>(entries.size());
    for (int index = 0; same && index < list->count(); ++index) {
        const QListWidgetItem* item = list->item(index);
        same = item->text() == entries[static_cast<std::size_t>(index)].first &&
               item->data(Qt::UserRole).toString() == entries[static_cast<std::size_t>(index)].second;
    }
    if (same) return;
    const QString current = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    list->clear();
    for (const auto& [display, value] : entries) {
        auto* item = new QListWidgetItem(display, list);
        item->setData(Qt::UserRole, value);
        if (!current.isEmpty() && value == current) list->setCurrentItem(item);
    }
}

/// The common value of `extract` over all rows, or nullopt when the rows disagree.
template <class Extract>
auto common_value(const std::vector<search::SampleRow>& rows, Extract extract)
    -> std::optional<decltype(extract(rows.front()))> {
    if (rows.empty()) return std::nullopt;
    auto first = extract(rows.front());
    for (const auto& row : rows)
        if (!(extract(row) == first)) return std::nullopt;
    return first;
}

/// Override mode a row's value source implies ("user": a manual value, "user_unknown": unknown).
OverrideChoice choice_of_source(const std::string& source) {
    if (source == "user") return ValueChoice;
    if (source == "user_unknown") return UnknownChoice;
    return AutomaticChoice;
}

/// Override mode of a stored override state.
OverrideChoice choice_of_state(catalog::OverrideState state) {
    switch (state) {
        case catalog::OverrideState::Value:
            return ValueChoice;
        case catalog::OverrideState::Unknown:
            return UnknownChoice;
        case catalog::OverrideState::Automatic:
            break;
    }
    return AutomaticChoice;
}

/// Key combo box data of a key: pitch class, plus 12 for minor.
int key_code(const MusicalKey& key) {
    return key.pitch_class + (key.mode == KeyMode::Minor ? 12 : 0);
}

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

}  // namespace

InspectorPanel::InspectorPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    choices_timer_.setSingleShot(true);
    connect(&choices_timer_, &QTimer::timeout, this, &InspectorPanel::start_due_choices_read);
    build_ui();
    show_selection({}, 0);
}

// ---------------------------------------------------------------- construction

void InspectorPanel::build_ui() {
    auto* content = new QWidget;
    auto* form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    build_identity_fields(*form, *content);
    form->addRow(separator(content));
    build_rating_fields(*form, *content);
    build_notes_fields(*form, *content);
    build_tag_fields(*form, *content);
    build_category_fields(*form, *content);
    form->addRow(separator(content));
    build_override_fields(*form, *content);
    form->addRow(separator(content));
    build_collection_fields(*form, *content);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void InspectorPanel::build_identity_fields(QFormLayout& form, QWidget& content) {
    heading_label_ = new QLabel(&content);
    heading_label_->setWordWrap(true);
    heading_label_->setTextFormat(Qt::PlainText);  // sample names are user text, never markup
    QFont heading_font = heading_label_->font();
    heading_font.setBold(true);
    heading_label_->setFont(heading_font);
    heading_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form.addRow(heading_label_);
    partial_label_ = new QLabel(&content);
    partial_label_->setWordWrap(true);
    partial_label_->setTextFormat(Qt::PlainText);
    form.addRow(partial_label_);

    // Display name: applied only with Enter or the Rename button.
    name_edit_ = new QLineEdit(&content);
    name_edit_->setAccessibleName(tr("Display name"));
    rename_button_ = new QPushButton(tr("Rename"), &content);
    rename_button_->setAccessibleName(tr("Apply display name"));
    const auto rename = [this] {
        const QString name = name_edit_->text().trimmed();
        if (name.isEmpty()) return;
        catalog::EditCommand command;
        command.display_name = DisplayFormat::to_utf8(name);
        name_edit_->setModified(false);  // applied: later refreshes may update the field again
        request(std::move(command));
    };
    connect(rename_button_, &QPushButton::clicked, this, rename);
    connect(name_edit_, &QLineEdit::returnPressed, this, rename);
    auto* name_row = new QHBoxLayout;
    name_row->addWidget(name_edit_, 1);
    name_row->addWidget(rename_button_);
    form.addRow(tr("Name"), name_row);

    original_name_label_ = new QLabel(&content);
    original_name_label_->setTextFormat(Qt::PlainText);
    original_name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    original_name_label_->setWordWrap(true);
    original_name_label_->setAccessibleName(tr("Original file"));
    form.addRow(tr("Original file"), original_name_label_);
    format_label_ = new QLabel(&content);
    format_label_->setWordWrap(true);
    format_label_->setTextFormat(Qt::PlainText);
    format_label_->setAccessibleName(tr("Format"));
    form.addRow(tr("Format"), format_label_);
}

void InspectorPanel::build_rating_fields(QFormLayout& form, QWidget& content) {
    // Rating: click a star (clicking the current rating clears it). Only an explicit click or key
    // changes a multi-selection, which shows "Mixed" until then.
    rating_stars_ = new StarRatingWidget(&content);
    rating_label_ = new QLabel(&content);
    connect(rating_stars_, &StarRatingWidget::ratingChosen, this, [this](int rating) {
        catalog::EditCommand command;
        command.rating = rating;
        request(std::move(command));
    });
    auto* rating_row = new QHBoxLayout;
    rating_row->addWidget(rating_stars_);
    rating_row->addWidget(rating_label_, 1);
    form.addRow(tr("Rating"), rating_row);

    favorite_label_ = new QLabel(&content);
    favorite_set_button_ = new QPushButton(tr("Mark favorite"), &content);
    favorite_clear_button_ = new QPushButton(tr("Unmark"), &content);
    favorite_clear_button_->setAccessibleName(tr("Unmark favorite"));
    for (auto [button, value] : {std::pair{favorite_set_button_, true}, std::pair{favorite_clear_button_, false}}) {
        connect(button, &QPushButton::clicked, this, [this, value] {
            catalog::EditCommand command;
            command.favorite = value;
            request(std::move(command));
        });
    }
    auto* favorite_row = new QHBoxLayout;
    favorite_row->addWidget(favorite_label_, 1);
    favorite_row->addWidget(favorite_set_button_);
    favorite_row->addWidget(favorite_clear_button_);
    form.addRow(tr("Favorite"), favorite_row);
}

void InspectorPanel::build_notes_fields(QFormLayout& form, QWidget& content) {
    // Notes: saved only with the Save button; switching selection discards an unsaved draft.
    notes_edit_ = new QPlainTextEdit(&content);
    notes_edit_->setAccessibleName(tr("Notes"));
    notes_edit_->setTabChangesFocus(true);
    notes_edit_->setMaximumHeight(110);
    connect(notes_edit_, &QPlainTextEdit::textChanged, this, [this] {
        notes_dirty_ = true;  // programmatic changes are made with signals blocked
        update_enabled();
    });
    notes_save_button_ = new QPushButton(tr("Save notes"), &content);
    notes_clear_button_ = new QPushButton(tr("Clear notes"), &content);
    connect(notes_save_button_, &QPushButton::clicked, this, [this] {
        catalog::EditCommand command;
        command.notes = DisplayFormat::to_utf8(notes_edit_->toPlainText());
        notes_dirty_ = false;
        update_enabled();
        request(std::move(command));
    });
    connect(notes_clear_button_, &QPushButton::clicked, this, [this] {
        notes_dirty_ = false;  // the draft is replaced by the cleared notes
        {
            const QSignalBlocker blocker(notes_edit_);
            notes_edit_->clear();
        }
        update_enabled();
        catalog::EditCommand command;
        command.notes = std::string();
        request(std::move(command));
    });
    auto* notes_buttons = new QHBoxLayout;
    notes_buttons->addStretch(1);
    notes_buttons->addWidget(notes_save_button_);
    notes_buttons->addWidget(notes_clear_button_);
    auto* notes_box = new QVBoxLayout;
    notes_box->addWidget(notes_edit_);
    notes_box->addLayout(notes_buttons);
    form.addRow(tr("Notes"), notes_box);
}

void InspectorPanel::build_tag_fields(QFormLayout& form, QWidget& content) {
    // Tags: multi-selection shows the union with "n of m" counts.
    tags_list_ = new QListWidget(&content);
    tags_list_->setAccessibleName(tr("Tags"));
    tags_list_->setMaximumHeight(110);
    tag_edit_ = new QLineEdit(&content);
    tag_edit_->setPlaceholderText(tr("Tag path, e.g. Drums/Kick"));
    tag_edit_->setAccessibleName(tr("Tag to add"));
    tag_add_button_ = new QPushButton(tr("Add"), &content);
    tag_add_button_->setAccessibleName(tr("Add tag"));
    tag_remove_button_ = new QPushButton(tr("Remove selected tag"), &content);
    const auto add_tag = [this] {
        const QString tag = tag_edit_->text().trimmed();
        if (tag.isEmpty()) return;
        catalog::EditCommand command;
        command.tags_add.push_back(DisplayFormat::to_utf8(tag));
        // The typed tag stays (selected, so typing replaces it) until the user moves on: if the
        // save fails, it is not lost.
        tag_edit_->selectAll();
        request(std::move(command));
    };
    connect(tag_add_button_, &QPushButton::clicked, this, add_tag);
    connect(tag_edit_, &QLineEdit::returnPressed, this, add_tag);
    connect(tag_remove_button_, &QPushButton::clicked, this, [this] {
        const QListWidgetItem* item = tags_list_->currentItem();
        if (!item) return;
        catalog::EditCommand command;
        command.tags_remove.push_back(DisplayFormat::to_utf8(item->data(Qt::UserRole).toString()));
        request(std::move(command));
    });
    connect(tags_list_, &QListWidget::currentItemChanged, this, [this] { update_enabled(); });
    auto* tag_row = new QHBoxLayout;
    tag_row->addWidget(tag_edit_, 1);
    tag_row->addWidget(tag_add_button_);
    auto* tags_box = new QVBoxLayout;
    tags_box->addWidget(tags_list_);
    tags_box->addLayout(tag_row);
    tags_box->addWidget(tag_remove_button_, 0, Qt::AlignRight);
    form.addRow(tr("Tags"), tags_box);
}

void InspectorPanel::build_category_fields(QFormLayout& form, QWidget& content) {
    // Category: one exclusive primary category per sample.
    category_label_ = new QLabel(&content);
    category_label_->setTextFormat(Qt::PlainText);
    category_combo_ = new QComboBox(&content);
    category_combo_->setEditable(true);
    category_combo_->setInsertPolicy(QComboBox::NoInsert);
    category_combo_->setAccessibleName(tr("Category to set"));
    category_set_button_ = new QPushButton(tr("Set"), &content);
    category_set_button_->setAccessibleName(tr("Set category"));
    category_clear_button_ = new QPushButton(tr("Clear"), &content);
    category_clear_button_->setAccessibleName(tr("Clear category"));
    connect(category_set_button_, &QPushButton::clicked, this, [this] {
        const QString category = category_combo_->currentText().trimmed();
        if (category.isEmpty()) return;
        catalog::EditCommand command;
        command.category = std::optional<std::string>(DisplayFormat::to_utf8(category));
        request(std::move(command));
    });
    connect(category_clear_button_, &QPushButton::clicked, this, [this] {
        catalog::EditCommand command;
        command.category = std::optional<std::string>();
        request(std::move(command));
    });
    auto* category_row = new QHBoxLayout;
    category_row->addWidget(category_combo_, 1);
    category_row->addWidget(category_set_button_);
    category_row->addWidget(category_clear_button_);
    auto* category_box = new QVBoxLayout;
    category_box->addWidget(category_label_);
    category_box->addLayout(category_row);
    form.addRow(tr("Category"), category_box);
}

void InspectorPanel::build_override_fields(QFormLayout& form, QWidget& content) {
    // BPM: Automatic clears the override, Unknown suppresses inference, Value pins a tempo. Every
    // programmatic change happens with signals blocked, so these signals mean "the user changed it".
    bpm_label_ = new QLabel(&content);
    bpm_label_->setWordWrap(true);
    bpm_mode_combo_ = new QComboBox(&content);
    bpm_mode_combo_->addItems({tr("Automatic"), tr("Unknown"), tr("Value")});
    bpm_mode_combo_->setAccessibleName(tr("BPM override mode"));
    bpm_mode_combo_->setPlaceholderText(tr("Mixed"));
    bpm_spin_ = new QDoubleSpinBox(&content);
    bpm_spin_->setRange(kMinimumBpm, kMaximumBpm);
    bpm_spin_->setDecimals(2);
    bpm_spin_->setValue(kDefaultBpm);
    bpm_spin_->setAccessibleName(tr("BPM value"));
    bpm_apply_button_ = new QPushButton(tr("Apply"), &content);
    bpm_apply_button_->setAccessibleName(tr("Apply BPM"));
    connect(bpm_mode_combo_, &QComboBox::currentIndexChanged, this, [this] {
        bpm_draft_ = true;
        update_enabled();
    });
    connect(bpm_spin_, &QDoubleSpinBox::valueChanged, this, [this] { bpm_draft_ = true; });
    connect(bpm_apply_button_, &QPushButton::clicked, this, [this] {
        catalog::BpmOverrideEdit edit;
        switch (bpm_mode_combo_->currentIndex()) {
            case MixedChoice:
                return;  // nothing chosen for a selection whose overrides differ
            case UnknownChoice:
                edit.state = catalog::OverrideState::Unknown;
                break;
            case ValueChoice:
                edit.state = catalog::OverrideState::Value;
                edit.milli_bpm = std::llround(bpm_spin_->value() * 1000.0);
                break;
            default:
                edit.state = catalog::OverrideState::Automatic;
                break;
        }
        catalog::EditCommand command;
        command.bpm = edit;
        bpm_draft_ = false;
        request(std::move(command));
    });
    auto* bpm_row = new QHBoxLayout;
    bpm_row->addWidget(bpm_mode_combo_);
    bpm_row->addWidget(bpm_spin_, 1);
    bpm_row->addWidget(bpm_apply_button_);
    auto* bpm_box = new QVBoxLayout;
    bpm_box->addWidget(bpm_label_);
    bpm_box->addLayout(bpm_row);
    form.addRow(tr("BPM"), bpm_box);

    key_label_ = new QLabel(&content);
    key_label_->setWordWrap(true);
    key_mode_combo_ = new QComboBox(&content);
    key_mode_combo_->addItems({tr("Automatic"), tr("Unknown"), tr("Value")});
    key_mode_combo_->setAccessibleName(tr("Key override mode"));
    key_mode_combo_->setPlaceholderText(tr("Mixed"));
    key_combo_ = new QComboBox(&content);
    key_combo_->setAccessibleName(tr("Key value"));
    for (KeyMode mode : {KeyMode::Major, KeyMode::Minor}) {
        for (int pitch_class = 0; pitch_class < 12; ++pitch_class) {
            const MusicalKey key{pitch_class, mode};
            key_combo_->addItem(DisplayFormat::key(key), key_code(key));
        }
    }
    key_apply_button_ = new QPushButton(tr("Apply"), &content);
    key_apply_button_->setAccessibleName(tr("Apply key"));
    connect(key_mode_combo_, &QComboBox::currentIndexChanged, this, [this] {
        key_draft_ = true;
        update_enabled();
    });
    connect(key_combo_, &QComboBox::currentIndexChanged, this, [this] { key_draft_ = true; });
    connect(key_apply_button_, &QPushButton::clicked, this, [this] {
        catalog::KeyOverrideEdit edit;
        switch (key_mode_combo_->currentIndex()) {
            case MixedChoice:
                return;
            case UnknownChoice:
                edit.state = catalog::OverrideState::Unknown;
                break;
            case ValueChoice: {
                const int code = key_combo_->currentData().toInt();
                edit.state = catalog::OverrideState::Value;
                edit.key = MusicalKey{code % 12, code >= 12 ? KeyMode::Minor : KeyMode::Major};
                break;
            }
            default:
                edit.state = catalog::OverrideState::Automatic;
                break;
        }
        catalog::EditCommand command;
        command.key = edit;
        key_draft_ = false;
        request(std::move(command));
    });
    auto* key_row = new QHBoxLayout;
    key_row->addWidget(key_mode_combo_);
    key_row->addWidget(key_combo_, 1);
    key_row->addWidget(key_apply_button_);
    auto* key_box = new QVBoxLayout;
    key_box->addWidget(key_label_);
    key_box->addLayout(key_row);
    form.addRow(tr("Key"), key_box);
}

void InspectorPanel::build_collection_fields(QFormLayout& form, QWidget& content) {
    // Collections: manual collections of the selection's library.
    collections_list_ = new QListWidget(&content);
    collections_list_->setAccessibleName(tr("Collections of this sample"));
    collections_list_->setMaximumHeight(80);
    collection_combo_ = new QComboBox(&content);
    collection_combo_->setAccessibleName(tr("Collection"));
    collection_add_button_ = new QPushButton(tr("Add to"), &content);
    collection_add_button_->setAccessibleName(tr("Add to collection"));
    collection_remove_button_ = new QPushButton(tr("Remove from"), &content);
    collection_remove_button_->setAccessibleName(tr("Remove from collection"));
    for (auto [button, adding] :
         {std::pair{collection_add_button_, true}, std::pair{collection_remove_button_, false}}) {
        connect(button, &QPushButton::clicked, this, [this, adding] {
            const int index = collection_combo_->currentIndex();
            if (index < 0 || static_cast<std::size_t>(index) >= manual_collection_ids_.size()) return;
            catalog::EditCommand command;
            if (adding)
                command.add_to_collection = manual_collection_ids_[static_cast<std::size_t>(index)];
            else
                command.remove_from_collection = manual_collection_ids_[static_cast<std::size_t>(index)];
            request(std::move(command));
        });
    }
    auto* collection_row = new QHBoxLayout;
    collection_row->addWidget(collection_combo_, 1);
    collection_row->addWidget(collection_add_button_);
    collection_row->addWidget(collection_remove_button_);
    auto* collections_box = new QVBoxLayout;
    collections_box->addWidget(collections_list_);
    collections_box->addLayout(collection_row);
    form.addRow(tr("Collections"), collections_box);

    locations_group_ = new QGroupBox(tr("Locations"), &content);
    locations_list_ = new QListWidget(locations_group_);
    locations_list_->setAccessibleName(tr("File locations"));
    locations_list_->setMaximumHeight(90);
    auto* locations_layout = new QVBoxLayout(locations_group_);
    locations_layout->addWidget(locations_list_);
    form.addRow(locations_group_);
}

// ---------------------------------------------------------------- selection and details

void InspectorPanel::show_selection(std::vector<search::SampleRow> rows, std::size_t total_selected,
                                    std::optional<std::set<uuids::uuid>> all_libraries) {
    const std::set<SampleKey> keys = keys_of(rows);
    const bool same_samples = keys == selected_keys_ && total_selected == total_selected_;
    const std::set<uuids::uuid> libraries = all_libraries ? std::move(*all_libraries) : libraries_of(rows);
    const bool same_libraries = libraries == selected_libraries_;
    rows_ = std::move(rows);
    total_selected_ = total_selected;
    selected_keys_ = keys;
    selected_libraries_ = libraries;
    if (!same_samples) {
        // Another selection: drafts of the previous one are discarded, details re-read.
        detail_.reset();
        ++detail_request_;
        reset_drafts();
    }
    update_summary();
    if (rows_.size() == 1 && whole_selection_loaded() && !same_samples) load_detail(rows_.front());
    if (!same_libraries) reload_library_choices();
    update_enabled();
}

void InspectorPanel::reset_drafts() {
    notes_dirty_ = false;
    bpm_draft_ = false;
    key_draft_ = false;
    // Even a field that still has the focus shows the new selection: Enter or Apply must never
    // write the previous sample's draft to it.
    name_edit_->setModified(false);
    {
        const QSignalBlocker blocker(name_edit_);
        name_edit_->setText(rows_.size() == 1 ? DisplayFormat::from_utf8(rows_.front().display_name.empty()
                                                                             ? rows_.front().file_name
                                                                             : rows_.front().display_name)
                                              : QString());
    }
    {
        const QSignalBlocker blocker(notes_edit_);
        notes_edit_->clear();  // a single selection's notes arrive with its detail
    }
    show_row_overrides();
}

void InspectorPanel::show_row_overrides() {
    const QSignalBlocker bpm_mode_blocker(bpm_mode_combo_);
    const QSignalBlocker bpm_value_blocker(bpm_spin_);
    const QSignalBlocker key_mode_blocker(key_mode_combo_);
    const QSignalBlocker key_value_blocker(key_combo_);
    if (rows_.empty()) {
        bpm_mode_combo_->setCurrentIndex(AutomaticChoice);
        key_mode_combo_->setCurrentIndex(AutomaticChoice);
        return;
    }
    // A blank mode (MixedChoice) when the rows differ: Apply waits for an explicit choice.
    // Manual values that differ are mixed too: "Value" with one sample's number would offer to
    // write that number to all of them.
    const auto bpm_mode = common_value(rows_, [](const auto& row) { return choice_of_source(row.bpm_source); });
    const auto bpm = common_value(rows_, [](const auto& row) { return row.bpm_milli; });
    const bool bpm_mixed = !bpm_mode || (*bpm_mode == ValueChoice && !bpm);
    bpm_mode_combo_->setCurrentIndex(bpm_mixed ? MixedChoice : *bpm_mode);
    if (bpm && *bpm) bpm_spin_->setValue(static_cast<double>(**bpm) / 1000.0);
    const auto key_mode = common_value(rows_, [](const auto& row) { return choice_of_source(row.key_source); });
    const auto key = common_value(rows_, [](const auto& row) { return row.key; });
    const bool key_mixed = !key_mode || (*key_mode == ValueChoice && !key);
    key_mode_combo_->setCurrentIndex(key_mixed ? MixedChoice : *key_mode);
    if (key && *key && (*key)->mode != KeyMode::Unknown)
        key_combo_->setCurrentIndex(key_combo_->findData(key_code(**key)));
}

void InspectorPanel::reload_detail() {
    if (rows_.size() == 1 && whole_selection_loaded()) load_detail(rows_.front());
}

void InspectorPanel::library_structure_changed(const uuids::uuid& library) {
    // Other libraries' imports and edits never re-read these lists.
    if (selected_libraries_.size() != 1 || *selected_libraries_.begin() != library) return;
    choices_dirty_ = true;
    start_due_choices_read();
}

void InspectorPanel::start_due_choices_read() {
    if (!choices_dirty_ || choice_reads_in_flight_ > 0 || selected_libraries_.size() != 1) return;
    const qint64 waited =
        choices_last_started_.isValid() ? choices_last_started_.elapsed() : qint64{kChoicesReloadIntervalMilliseconds};
    if (waited < kChoicesReloadIntervalMilliseconds) {
        if (!choices_timer_.isActive())
            choices_timer_.start(static_cast<int>(kChoicesReloadIntervalMilliseconds - waited));
        return;
    }
    choices_dirty_ = false;
    load_library_choices(*selected_libraries_.begin());
}

void InspectorPanel::reload_library_choices() {
    ++choices_request_;  // a read for the previous library is dropped when it arrives
    choices_dirty_ = false;
    choices_timer_.stop();
    if (selected_libraries_.size() == 1) {
        load_library_choices(*selected_libraries_.begin());
        return;
    }
    choices_library_.reset();
    // Categories and collections belong to one library; a selection spanning several (or none)
    // cannot offer a common list.
    category_combo_->clear();
    collection_combo_->clear();
    manual_collection_ids_.clear();
    const QString reason = selected_libraries_.size() > 1
                               ? tr("Select samples of a single library to set a category or collection")
                               : QString();
    category_combo_->setToolTip(reason);
    collection_combo_->setToolTip(reason);
    update_enabled();
}

void InspectorPanel::set_read_only(bool read_only) {
    read_only_ = read_only;
    update_enabled();
}

void InspectorPanel::load_detail(const search::SampleRow& row) {
    if (!session_.is_ready()) return;
    const std::uint64_t request = ++detail_request_;
    const std::int64_t sample_id = row.sample_id;
    session_.run_with_library(
        this, row.library, [sample_id](catalog::Library& library) { return library.sample_detail(sample_id); },
        [this, request](Result<catalog::SampleDetail> detail) {
            if (request != detail_request_) return;
            if (!detail) {
                partial_label_->setText(tr("Details unavailable: %1").arg(DisplayFormat::error(detail.error())));
                partial_label_->setVisible(true);
                return;
            }
            show_detail(*detail);
        });
}

void InspectorPanel::show_detail(const catalog::SampleDetail& detail) {
    detail_ = detail;
    original_name_label_->setText(DisplayFormat::from_utf8(detail.original_name));
    QStringList format;
    format << DisplayFormat::from_utf8(detail.format).toUpper();
    if (!detail.codec.empty()) format << DisplayFormat::from_utf8(detail.codec);
    for (const QString& part : {DisplayFormat::sample_rate(detail.sample_rate),
                                DisplayFormat::channels(detail.channels), DisplayFormat::bit_depth(detail.bit_depth),
                                DisplayFormat::duration(detail.duration_us, detail.duration_exact),
                                DisplayFormat::byte_size(detail.byte_size)})
        if (!part.isEmpty()) format << part;
    format_label_->setText(format.join(QStringLiteral(" · ")));

    const QString notes = DisplayFormat::from_utf8(detail.notes);
    if (!notes_dirty_ && notes_edit_->toPlainText() != notes) {
        const QSignalBlocker blocker(notes_edit_);
        notes_edit_->setPlainText(notes);
    }
    std::vector<std::pair<QString, QString>> tags;
    for (const auto& tag : detail.tags)
        tags.emplace_back(DisplayFormat::from_utf8(tag.path), DisplayFormat::from_utf8(tag.path));
    set_list_entries(tags_list_, tags);

    const auto describe_override = [](catalog::OverrideState state) {
        switch (state) {
            case catalog::OverrideState::Value:
                return tr("manual value");
            case catalog::OverrideState::Unknown:
                return tr("manually set to unknown");
            case catalog::OverrideState::Automatic:
                break;
        }
        return tr("automatic");
    };
    bpm_label_->setText(detail.bpm_milli ? tr("%1 — %2; override: %3")
                                               .arg(DisplayFormat::bpm(detail.bpm_milli),
                                                    DisplayFormat::provenance(detail.bpm_source, detail.bpm_confidence),
                                                    describe_override(detail.bpm_override))
                                         : tr("Unknown; override: %1").arg(describe_override(detail.bpm_override)));
    key_label_->setText(detail.key ? tr("%1 — %2; override: %3")
                                         .arg(DisplayFormat::key(detail.key),
                                              DisplayFormat::provenance(detail.key_source, detail.key_confidence),
                                              describe_override(detail.key_override))
                                   : tr("Unknown; override: %1").arg(describe_override(detail.key_override)));
    // Override controls the user changed keep their (unapplied) values, focused or not.
    if (!bpm_draft_) {
        const QSignalBlocker mode_blocker(bpm_mode_combo_);
        const QSignalBlocker value_blocker(bpm_spin_);
        bpm_mode_combo_->setCurrentIndex(choice_of_state(detail.bpm_override));
        if (detail.bpm_override_milli)
            bpm_spin_->setValue(static_cast<double>(*detail.bpm_override_milli) / 1000.0);
        else if (detail.bpm_milli)
            bpm_spin_->setValue(static_cast<double>(*detail.bpm_milli) / 1000.0);
    }
    if (!key_draft_) {
        const QSignalBlocker mode_blocker(key_mode_combo_);
        const QSignalBlocker value_blocker(key_combo_);
        key_mode_combo_->setCurrentIndex(choice_of_state(detail.key_override));
        const auto key = detail.key_override_value ? detail.key_override_value : detail.key;
        if (key && key->mode != KeyMode::Unknown) key_combo_->setCurrentIndex(key_combo_->findData(key_code(*key)));
    }

    collections_list_->clear();
    for (const auto& collection : detail.collections)
        collections_list_->addItem(DisplayFormat::from_utf8(collection.name));

    locations_list_->clear();
    for (const auto& location : detail.locations) {
        locations_list_->addItem(tr("%1/%2 — %3")
                                     .arg(DisplayFormat::from_utf8(location.root_name),
                                          DisplayFormat::from_utf8(location.relative_path),
                                          DisplayFormat::location_state(location.state)));
    }
    locations_group_->setVisible(true);
    update_enabled();
}

void InspectorPanel::load_library_choices(const uuids::uuid& library) {
    if (!session_.is_ready()) return;
    const std::uint64_t request = choices_request_;
    ++choice_reads_in_flight_;
    ++choice_reads_started_;
    choices_last_started_.start();
    /// The category and manual-collection lists of one library.
    struct Choices {
        std::vector<catalog::TaxonomyNode> categories;
        std::vector<catalog::CollectionRecord> collections;
    };
    session_.run_with_library(
        this, library,
        [](catalog::Library& opened) -> Result<Choices> {
            Choices choices;
            auto categories = opened.list_categories();
            if (!categories) return std::unexpected(categories.error());
            auto collections = opened.list_collections();
            if (!collections) return std::unexpected(collections.error());
            choices.categories = std::move(*categories);
            choices.collections = std::move(*collections);
            return choices;
        },
        [this, request, library](Result<Choices> choices) {
            --choice_reads_in_flight_;
            if (request != choices_request_) {
                start_due_choices_read();
                return;
            }
            // Another library's lists are refilled even into a focused combo box: its ids (and a
            // half-typed category of another library) mean nothing for this selection.
            const bool library_changed = choices_library_ != library;
            choices_library_ = library;
            start_due_choices_read();  // a change that arrived meanwhile is read now (or soon)
            if (!choices) {
                category_combo_->setToolTip(
                    tr("Categories unavailable: %1").arg(DisplayFormat::error(choices.error())));
                return;
            }
            category_combo_->setToolTip({});
            collection_combo_->setToolTip({});
            if (library_changed || !category_combo_->hasFocus()) {  // never replace what the user types
                const QString typed = library_changed ? QString() : category_combo_->currentText();
                category_combo_->clear();
                for (const auto& category : choices->categories)
                    category_combo_->addItem(DisplayFormat::from_utf8(category.path));
                category_combo_->setEditText(typed);
            }
            if (library_changed || !collection_combo_->hasFocus()) {
                const QVariant chosen = library_changed ? QVariant() : collection_combo_->currentData();
                collection_combo_->clear();
                manual_collection_ids_.clear();
                for (const auto& collection : choices->collections) {
                    if (collection.smart) continue;  // smart membership follows their query
                    collection_combo_->addItem(DisplayFormat::from_utf8(collection.name),
                                               static_cast<qlonglong>(collection.id));
                    manual_collection_ids_.push_back(collection.id);
                }
                if (chosen.isValid()) collection_combo_->setCurrentIndex(collection_combo_->findData(chosen));
            }
            update_enabled();
        });
}

void InspectorPanel::update_summary() {
    const std::size_t count = rows_.size();
    // A partly loaded selection is summarized over its loaded rows, always as several samples.
    const bool single = count == 1 && whole_selection_loaded();
    const bool several = count > 1 || !whole_selection_loaded();
    const std::size_t selected = std::max(count, total_selected_);
    partial_label_->setText(
        whole_selection_loaded()
            ? QString()
            : tr("Showing the values of the %1 loaded of the %2 selected samples. Edits apply to all %2.")
                  .arg(DisplayFormat::count(static_cast<std::int64_t>(count)),
                       DisplayFormat::count(static_cast<std::int64_t>(total_selected_))));
    partial_label_->setVisible(!whole_selection_loaded());
    const QString mixed = tr("Mixed");

    if (!whole_selection_loaded()) {
        heading_label_->setText(tr("%n sample(s) selected", nullptr, static_cast<int>(total_selected_)));
    } else if (count == 0) {
        heading_label_->setText(tr("No sample selected"));
    } else if (single) {
        const auto& row = rows_.front();
        heading_label_->setText(DisplayFormat::from_utf8(row.display_name.empty() ? row.file_name : row.display_name));
    } else {
        heading_label_->setText(tr("%n sample(s) selected", nullptr, static_cast<int>(selected)));
    }

    set_text_unless_edited(name_edit_, single ? heading_label_->text() : QString());
    name_edit_->setPlaceholderText(several ? tr("Select one sample to rename") : QString());
    if (!single || !detail_) {
        // A single selection's file name and format come from its detail (show_detail), which a
        // row refresh must not blank; before the detail arrives the row's values stand in.
        original_name_label_->setText(single    ? DisplayFormat::from_utf8(rows_.front().file_name)
                                      : several ? mixed
                                                : QString());
        const auto format = common_value(rows_, [](const auto& row) { return row.format; });
        format_label_->setText(count == 0 ? QString() : format ? DisplayFormat::from_utf8(*format).toUpper() : mixed);
    }

    const auto rating = common_value(rows_, [](const auto& row) { return row.rating; });
    if (rating)
        rating_stars_->set_rating(*rating);
    else if (count == 0)
        rating_stars_->set_rating(0);  // nothing selected: no stars left over from the last sample
    else
        rating_stars_->set_mixed(true);
    rating_label_->setText(several && !rating ? mixed : QString());

    const auto favorite = common_value(rows_, [](const auto& row) { return row.favorite; });
    favorite_label_->setText(count == 0 ? QString() : favorite ? (*favorite ? tr("Yes") : tr("No")) : mixed);

    if (!single || !detail_) {
        if (!notes_dirty_ && !notes_edit_->toPlainText().isEmpty()) {
            const QSignalBlocker blocker(notes_edit_);
            notes_edit_->clear();
        }
        notes_edit_->setPlaceholderText(several ? tr("Mixed — Save notes replaces the notes of all %n sample(s)",
                                                     nullptr, static_cast<int>(selected))
                                                : QString());
        // Tag union with counts, from the rows' tag path lists (details exist for single
        // selections only). Paths come from the catalog as a list: never split display text.
        std::map<std::string, int> tag_counts;
        for (const auto& row : rows_)
            for (const std::string& path : row.tag_paths)
                ++tag_counts[path];
        std::vector<std::pair<QString, QString>> tags;
        for (const auto& [path, tag_count] : tag_counts) {
            const QString tag = DisplayFormat::from_utf8(path);
            // One multi-argument arg(): a "%2" inside a tag name is never substituted.
            tags.emplace_back(several && static_cast<std::size_t>(tag_count) < count
                                  ? tr("%1 (%2 of %3)")
                                        .arg(tag, DisplayFormat::count(tag_count),
                                             DisplayFormat::count(static_cast<std::int64_t>(count)))
                                  : tag,
                              tag);
        }
        set_list_entries(tags_list_, tags);
        collections_list_->clear();
        if (several) collections_list_->addItem(tr("Select one sample to see its collections"));
        locations_list_->clear();
        locations_group_->setVisible(single);
        const auto bpm = common_value(rows_, [](const auto& row) { return row.bpm_milli; });
        const auto key = common_value(rows_, [](const auto& row) { return row.key; });
        bpm_label_->setText(count == 0 ? QString()
                            : !bpm     ? mixed
                            : *bpm     ? DisplayFormat::bpm(*bpm)
                                       : DisplayFormat::unknown());
        key_label_->setText(count == 0 ? QString()
                            : !key     ? mixed
                            : *key     ? DisplayFormat::key(*key)
                                       : DisplayFormat::unknown());
    }
    const auto category = common_value(rows_, [](const auto& row) { return row.category; });
    category_label_->setText(count == 0          ? QString()
                             : !category         ? mixed
                             : category->empty() ? tr("None")
                                                 : DisplayFormat::from_utf8(*category));
}

void InspectorPanel::update_enabled() {
    const bool has_selection = !rows_.empty();
    const bool editable = (has_selection || total_selected_ > 0) && !read_only_;
    const bool single = rows_.size() == 1 && whole_selection_loaded();
    name_edit_->setEnabled(editable && single);
    rename_button_->setEnabled(editable && single);
    rating_stars_->setEnabled(editable);
    favorite_set_button_->setEnabled(editable);
    favorite_clear_button_->setEnabled(editable);
    notes_edit_->setEnabled(editable);
    notes_save_button_->setEnabled(editable && notes_dirty_);
    notes_clear_button_->setEnabled(editable);
    tag_edit_->setEnabled(editable);
    tag_add_button_->setEnabled(editable);
    tag_remove_button_->setEnabled(editable && tags_list_->currentItem() != nullptr);
    // Categories and collections are library-scoped: one library at a time.
    const bool single_library = selected_libraries_.size() == 1;
    category_combo_->setEnabled(editable && single_library);
    category_set_button_->setEnabled(editable && single_library);
    category_clear_button_->setEnabled(editable);
    bpm_mode_combo_->setEnabled(editable);
    bpm_spin_->setEnabled(editable && bpm_mode_combo_->currentIndex() == ValueChoice);
    bpm_apply_button_->setEnabled(editable && bpm_mode_combo_->currentIndex() != MixedChoice);
    key_mode_combo_->setEnabled(editable);
    key_combo_->setEnabled(editable && key_mode_combo_->currentIndex() == ValueChoice);
    key_apply_button_->setEnabled(editable && key_mode_combo_->currentIndex() != MixedChoice);
    const bool collections_available = editable && single_library && !manual_collection_ids_.empty();
    collection_combo_->setEnabled(collections_available);
    collection_add_button_->setEnabled(collections_available);
    collection_remove_button_->setEnabled(collections_available);
}

void InspectorPanel::request(catalog::EditCommand command) {
    if ((rows_.empty() && total_selected_ == 0) || read_only_) return;
    emit editRequested(command);
}

}  // namespace mso::ui
