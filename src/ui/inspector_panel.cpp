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

#include "mso/ui/application_session.hpp"
#include "mso/ui/display_format.hpp"
#include "mso/ui/star_rating_widget.hpp"

namespace mso::ui {

namespace {

/// Index of the override modes in the BPM/key mode combo boxes.
enum OverrideChoice { AutomaticChoice = 0, UnknownChoice = 1, ValueChoice = 2 };

constexpr double kMinimumBpm = 20.0;
constexpr double kMaximumBpm = 400.0;

/// Splits SampleRow::tags (display paths joined with ", ") back into paths.
QStringList split_tags(const std::string& joined) {
    if (joined.empty()) return {};
    return DisplayFormat::from_utf8(joined).split(QStringLiteral(", "), Qt::SkipEmptyParts);
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

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

}  // namespace

InspectorPanel::InspectorPanel(ApplicationSession& session, QWidget* parent) : QWidget(parent), session_(session) {
    build_ui();
    show_selection({}, 0);
}

void InspectorPanel::build_ui() {
    auto* content = new QWidget;
    auto* form = new QFormLayout(content);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    heading_label_ = new QLabel(content);
    heading_label_->setWordWrap(true);
    QFont heading_font = heading_label_->font();
    heading_font.setBold(true);
    heading_label_->setFont(heading_font);
    heading_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(heading_label_);
    partial_label_ = new QLabel(content);
    partial_label_->setWordWrap(true);
    form->addRow(partial_label_);

    // Display name: applied only with Enter or the Rename button.
    name_edit_ = new QLineEdit(content);
    name_edit_->setAccessibleName(tr("Display name"));
    rename_button_ = new QPushButton(tr("Rename"), content);
    rename_button_->setAccessibleName(tr("Apply display name"));
    const auto rename = [this] {
        const QString name = name_edit_->text().trimmed();
        if (name.isEmpty()) return;
        catalog::EditCommand command;
        command.display_name = DisplayFormat::to_utf8(name);
        request(std::move(command));
    };
    connect(rename_button_, &QPushButton::clicked, this, rename);
    connect(name_edit_, &QLineEdit::returnPressed, this, rename);
    auto* name_row = new QHBoxLayout;
    name_row->addWidget(name_edit_, 1);
    name_row->addWidget(rename_button_);
    form->addRow(tr("Name"), name_row);

    original_name_label_ = new QLabel(content);
    original_name_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    original_name_label_->setWordWrap(true);
    form->addRow(tr("Original file"), original_name_label_);
    format_label_ = new QLabel(content);
    format_label_->setWordWrap(true);
    form->addRow(tr("Format"), format_label_);
    form->addRow(separator(content));

    // Rating: click a star (clicking the current rating clears it). Only an explicit click or key
    // changes a multi-selection, which shows "Mixed" until then.
    rating_stars_ = new StarRatingWidget(content);
    rating_label_ = new QLabel(content);
    connect(rating_stars_, &StarRatingWidget::ratingChosen, this, [this](int rating) {
        catalog::EditCommand command;
        command.rating = rating;
        request(std::move(command));
    });
    auto* rating_row = new QHBoxLayout;
    rating_row->addWidget(rating_stars_);
    rating_row->addWidget(rating_label_, 1);
    form->addRow(tr("Rating"), rating_row);

    favorite_label_ = new QLabel(content);
    favorite_set_button_ = new QPushButton(tr("Mark favorite"), content);
    favorite_clear_button_ = new QPushButton(tr("Unmark"), content);
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
    form->addRow(tr("Favorite"), favorite_row);

    // Notes: saved only with the Save button; switching selection discards an unsaved draft.
    notes_edit_ = new QPlainTextEdit(content);
    notes_edit_->setAccessibleName(tr("Notes"));
    notes_edit_->setTabChangesFocus(true);
    notes_edit_->setMaximumHeight(110);
    connect(notes_edit_, &QPlainTextEdit::textChanged, this, [this] {
        notes_dirty_ = true;
        update_enabled();
    });
    notes_save_button_ = new QPushButton(tr("Save notes"), content);
    notes_clear_button_ = new QPushButton(tr("Clear notes"), content);
    connect(notes_save_button_, &QPushButton::clicked, this, [this] {
        catalog::EditCommand command;
        command.notes = DisplayFormat::to_utf8(notes_edit_->toPlainText());
        notes_dirty_ = false;
        request(std::move(command));
    });
    connect(notes_clear_button_, &QPushButton::clicked, this, [this] {
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
    form->addRow(tr("Notes"), notes_box);

    // Tags: multi-selection shows the union with "n of m" counts.
    tags_list_ = new QListWidget(content);
    tags_list_->setAccessibleName(tr("Tags"));
    tags_list_->setMaximumHeight(110);
    tag_edit_ = new QLineEdit(content);
    tag_edit_->setPlaceholderText(tr("Tag path, e.g. Drums/Kick"));
    tag_edit_->setAccessibleName(tr("Tag to add"));
    tag_add_button_ = new QPushButton(tr("Add"), content);
    tag_add_button_->setAccessibleName(tr("Add tag"));
    tag_remove_button_ = new QPushButton(tr("Remove selected tag"), content);
    const auto add_tag = [this] {
        const QString tag = tag_edit_->text().trimmed();
        if (tag.isEmpty()) return;
        catalog::EditCommand command;
        command.tags_add.push_back(DisplayFormat::to_utf8(tag));
        tag_edit_->clear();
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
    form->addRow(tr("Tags"), tags_box);

    // Category: one exclusive primary category per sample.
    category_label_ = new QLabel(content);
    category_combo_ = new QComboBox(content);
    category_combo_->setEditable(true);
    category_combo_->setInsertPolicy(QComboBox::NoInsert);
    category_combo_->setAccessibleName(tr("Category to set"));
    category_set_button_ = new QPushButton(tr("Set"), content);
    category_set_button_->setAccessibleName(tr("Set category"));
    category_clear_button_ = new QPushButton(tr("Clear"), content);
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
    form->addRow(tr("Category"), category_box);
    form->addRow(separator(content));

    // BPM: Automatic clears the override, Unknown suppresses inference, Value pins a tempo.
    bpm_label_ = new QLabel(content);
    bpm_label_->setWordWrap(true);
    bpm_mode_combo_ = new QComboBox(content);
    bpm_mode_combo_->addItems({tr("Automatic"), tr("Unknown"), tr("Value")});
    bpm_mode_combo_->setAccessibleName(tr("BPM override mode"));
    bpm_spin_ = new QDoubleSpinBox(content);
    bpm_spin_->setRange(kMinimumBpm, kMaximumBpm);
    bpm_spin_->setDecimals(2);
    bpm_spin_->setValue(120.0);
    bpm_spin_->setAccessibleName(tr("BPM value"));
    bpm_apply_button_ = new QPushButton(tr("Apply"), content);
    bpm_apply_button_->setAccessibleName(tr("Apply BPM"));
    connect(bpm_mode_combo_, &QComboBox::currentIndexChanged, this, [this] { update_enabled(); });
    connect(bpm_apply_button_, &QPushButton::clicked, this, [this] {
        catalog::BpmOverrideEdit edit;
        switch (bpm_mode_combo_->currentIndex()) {
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
        request(std::move(command));
    });
    auto* bpm_row = new QHBoxLayout;
    bpm_row->addWidget(bpm_mode_combo_);
    bpm_row->addWidget(bpm_spin_, 1);
    bpm_row->addWidget(bpm_apply_button_);
    auto* bpm_box = new QVBoxLayout;
    bpm_box->addWidget(bpm_label_);
    bpm_box->addLayout(bpm_row);
    form->addRow(tr("BPM"), bpm_box);

    key_label_ = new QLabel(content);
    key_label_->setWordWrap(true);
    key_mode_combo_ = new QComboBox(content);
    key_mode_combo_->addItems({tr("Automatic"), tr("Unknown"), tr("Value")});
    key_mode_combo_->setAccessibleName(tr("Key override mode"));
    key_combo_ = new QComboBox(content);
    key_combo_->setAccessibleName(tr("Key value"));
    for (KeyMode mode : {KeyMode::Major, KeyMode::Minor}) {
        for (int pitch_class = 0; pitch_class < 12; ++pitch_class) {
            const MusicalKey key{pitch_class, mode};
            key_combo_->addItem(DisplayFormat::key(key), pitch_class + (mode == KeyMode::Minor ? 12 : 0));
        }
    }
    key_apply_button_ = new QPushButton(tr("Apply"), content);
    key_apply_button_->setAccessibleName(tr("Apply key"));
    connect(key_mode_combo_, &QComboBox::currentIndexChanged, this, [this] { update_enabled(); });
    connect(key_apply_button_, &QPushButton::clicked, this, [this] {
        catalog::KeyOverrideEdit edit;
        switch (key_mode_combo_->currentIndex()) {
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
        request(std::move(command));
    });
    auto* key_row = new QHBoxLayout;
    key_row->addWidget(key_mode_combo_);
    key_row->addWidget(key_combo_, 1);
    key_row->addWidget(key_apply_button_);
    auto* key_box = new QVBoxLayout;
    key_box->addWidget(key_label_);
    key_box->addLayout(key_row);
    form->addRow(tr("Key"), key_box);
    form->addRow(separator(content));

    // Collections: manual collections of the selection's library.
    collections_list_ = new QListWidget(content);
    collections_list_->setAccessibleName(tr("Collections of this sample"));
    collections_list_->setMaximumHeight(80);
    collection_combo_ = new QComboBox(content);
    collection_combo_->setAccessibleName(tr("Collection"));
    collection_add_button_ = new QPushButton(tr("Add to"), content);
    collection_add_button_->setAccessibleName(tr("Add to collection"));
    collection_remove_button_ = new QPushButton(tr("Remove from"), content);
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
    form->addRow(tr("Collections"), collections_box);

    locations_group_ = new QGroupBox(tr("Locations"), content);
    locations_list_ = new QListWidget(locations_group_);
    locations_list_->setAccessibleName(tr("File locations"));
    locations_list_->setMaximumHeight(90);
    auto* locations_layout = new QVBoxLayout(locations_group_);
    locations_layout->addWidget(locations_list_);
    form->addRow(locations_group_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);
}

void InspectorPanel::show_selection(std::vector<search::SampleRow> rows, std::size_t total_selected) {
    const bool same_single = rows.size() == 1 && rows_.size() == 1 &&
                             rows.front().sample_id == rows_.front().sample_id &&
                             rows.front().library == rows_.front().library;
    const bool same_library_set = !rows.empty() && !rows_.empty() && rows.front().library == rows_.front().library;
    rows_ = std::move(rows);
    total_selected_ = total_selected;
    if (!same_single) {
        detail_.reset();
        ++detail_request_;
        notes_dirty_ = false;
    }
    update_summary();
    if (rows_.size() == 1) load_detail(rows_.front());
    const bool single_library =
        !rows_.empty() && common_value(rows_, [](const auto& row) { return row.library; }).has_value();
    if (single_library && !same_library_set) load_library_choices(rows_.front().library);
    if (!single_library) {
        ++choices_request_;
        category_combo_->clear();
        collection_combo_->clear();
        manual_collection_ids_.clear();
    }
    update_enabled();
}

void InspectorPanel::set_read_only(bool read_only) {
    read_only_ = read_only;
    update_enabled();
}

void InspectorPanel::load_detail(const search::SampleRow& row) {
    const std::uint64_t request = ++detail_request_;
    const uuids::uuid library = row.library;
    const std::int64_t sample_id = row.sample_id;
    session_.run(
        this,
        [library, sample_id](app::Application& application) -> Result<catalog::SampleDetail> {
            catalog::Library* opened = application.library(library);
            if (!opened) return fail(ErrorCode::NotFound, "the library is no longer open");
            return opened->sample_detail(sample_id);
        },
        [this, request](Result<catalog::SampleDetail> detail) {
            if (request != detail_request_) return;
            if (!detail) {
                partial_label_->setText(tr("Details unavailable: %1").arg(DisplayFormat::error(detail.error())));
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

    if (!notes_dirty_) {
        const QSignalBlocker blocker(notes_edit_);
        notes_edit_->setPlainText(DisplayFormat::from_utf8(detail.notes));
    }
    tags_list_->clear();
    for (const auto& tag : detail.tags) {
        auto* item = new QListWidgetItem(DisplayFormat::from_utf8(tag.path), tags_list_);
        item->setData(Qt::UserRole, DisplayFormat::from_utf8(tag.path));
    }

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
    {
        const QSignalBlocker bpm_blocker(bpm_mode_combo_);
        const QSignalBlocker key_blocker(key_mode_combo_);
        const auto choice = [](catalog::OverrideState state) {
            return state == catalog::OverrideState::Value     ? ValueChoice
                   : state == catalog::OverrideState::Unknown ? UnknownChoice
                                                              : AutomaticChoice;
        };
        bpm_mode_combo_->setCurrentIndex(choice(detail.bpm_override));
        key_mode_combo_->setCurrentIndex(choice(detail.key_override));
        if (detail.bpm_override_milli)
            bpm_spin_->setValue(static_cast<double>(*detail.bpm_override_milli) / 1000.0);
        else if (detail.bpm_milli)
            bpm_spin_->setValue(static_cast<double>(*detail.bpm_milli) / 1000.0);
        const auto key = detail.key_override_value ? detail.key_override_value : detail.key;
        if (key && key->mode != KeyMode::Unknown)
            key_combo_->setCurrentIndex(
                key_combo_->findData(key->pitch_class + (key->mode == KeyMode::Minor ? 12 : 0)));
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
    const std::uint64_t request = ++choices_request_;
    struct Choices {
        std::vector<catalog::TaxonomyNode> categories;
        std::vector<catalog::CollectionRecord> collections;
    };
    session_.run(
        this,
        [library](app::Application& application) {
            Choices choices;
            if (catalog::Library* opened = application.library(library)) {
                if (auto categories = opened->list_categories()) choices.categories = std::move(*categories);
                if (auto collections = opened->list_collections()) choices.collections = std::move(*collections);
            }
            return choices;
        },
        [this, request](Choices choices) {
            if (request != choices_request_) return;
            const QString typed = category_combo_->currentText();
            category_combo_->clear();
            for (const auto& category : choices.categories)
                category_combo_->addItem(DisplayFormat::from_utf8(category.path));
            category_combo_->setEditText(typed);
            collection_combo_->clear();
            manual_collection_ids_.clear();
            for (const auto& collection : choices.collections) {
                if (collection.smart) continue;  // smart membership follows their query
                collection_combo_->addItem(DisplayFormat::from_utf8(collection.name));
                manual_collection_ids_.push_back(collection.id);
            }
            update_enabled();
        });
}

void InspectorPanel::update_summary() {
    const std::size_t count = rows_.size();
    partial_label_->setText(total_selected_ > count
                                ? tr("%1 of %2 selected samples are loaded; edits apply to the loaded ones.")
                                      .arg(DisplayFormat::count(static_cast<std::int64_t>(count)),
                                           DisplayFormat::count(static_cast<std::int64_t>(total_selected_)))
                                : QString());
    partial_label_->setVisible(total_selected_ > count);
    const QString mixed = tr("Mixed");

    if (count == 0) {
        heading_label_->setText(tr("No sample selected"));
    } else if (count == 1) {
        const auto& row = rows_.front();
        heading_label_->setText(DisplayFormat::from_utf8(row.display_name.empty() ? row.file_name : row.display_name));
    } else {
        heading_label_->setText(tr("%1 samples selected").arg(DisplayFormat::count(static_cast<std::int64_t>(count))));
    }

    {
        const QSignalBlocker blocker(name_edit_);
        name_edit_->setText(count == 1 ? heading_label_->text() : QString());
        name_edit_->setPlaceholderText(count > 1 ? tr("Select one sample to rename") : QString());
    }
    original_name_label_->setText(count == 1  ? DisplayFormat::from_utf8(rows_.front().file_name)
                                  : count > 1 ? mixed
                                              : QString());
    const auto format = common_value(rows_, [](const auto& row) { return row.format; });
    format_label_->setText(count <= 1 ? QString() : format ? DisplayFormat::from_utf8(*format).toUpper() : mixed);

    const auto rating = common_value(rows_, [](const auto& row) { return row.rating; });
    if (rating)
        rating_stars_->set_rating(*rating);
    else
        rating_stars_->set_mixed(count > 1);
    rating_label_->setText(count > 1 && !rating ? mixed : QString());

    const auto favorite = common_value(rows_, [](const auto& row) { return row.favorite; });
    favorite_label_->setText(count == 0 ? QString() : favorite ? (*favorite ? tr("Yes") : tr("No")) : mixed);

    if (count != 1 || !detail_) {
        if (!notes_dirty_) {
            const QSignalBlocker blocker(notes_edit_);
            notes_edit_->clear();
        }
        notes_edit_->setPlaceholderText(count > 1 ? tr("Mixed — Save notes replaces the notes of all %1 samples")
                                                        .arg(DisplayFormat::count(static_cast<std::int64_t>(count)))
                                                  : QString());
        // Tag union with counts, from the rows (details are loaded for single selections only).
        tags_list_->clear();
        std::map<QString, int> tag_counts;
        for (const auto& row : rows_)
            for (const QString& tag : split_tags(row.tags))
                ++tag_counts[tag];
        for (const auto& [tag, tag_count] : tag_counts) {
            const QString text = count > 1 && static_cast<std::size_t>(tag_count) < count
                                     ? tr("%1 (%2 of %3)").arg(tag).arg(tag_count).arg(count)
                                     : tag;
            auto* item = new QListWidgetItem(text, tags_list_);
            item->setData(Qt::UserRole, tag);
        }
        collections_list_->clear();
        if (count > 1) collections_list_->addItem(tr("Select one sample to see its collections"));
        locations_list_->clear();
        locations_group_->setVisible(count == 1);
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
    const bool editable = has_selection && !read_only_;
    const bool single = rows_.size() == 1;
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
    category_combo_->setEnabled(editable);
    category_set_button_->setEnabled(editable);
    category_clear_button_->setEnabled(editable);
    bpm_mode_combo_->setEnabled(editable);
    bpm_spin_->setEnabled(editable && bpm_mode_combo_->currentIndex() == ValueChoice);
    bpm_apply_button_->setEnabled(editable);
    key_mode_combo_->setEnabled(editable);
    key_combo_->setEnabled(editable && key_mode_combo_->currentIndex() == ValueChoice);
    key_apply_button_->setEnabled(editable);
    const bool collections_available = editable && !manual_collection_ids_.empty();
    collection_combo_->setEnabled(collections_available);
    collection_add_button_->setEnabled(collections_available);
    collection_remove_button_->setEnabled(collections_available);
}

void InspectorPanel::request(catalog::EditCommand command) {
    if (rows_.empty() || read_only_) return;
    emit editRequested(command);
}

}  // namespace mso::ui
