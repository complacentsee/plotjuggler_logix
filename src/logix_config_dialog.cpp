#include "logix_config_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QMessageBox>
#include <QApplication>

#include <cmath>

namespace logix {

constexpr uint32_t kMaxTrendInstances = 32;

LogixConfigDialog::LogixConfigDialog(QWidget* parent, const LogixConfig* previous)
    : QDialog(parent) {
    setWindowTitle("Logix (CIP 0xB2) Configuration");
    setMinimumSize(600, 500);
    setupUi();

    // Restore previous config if available
    if (previous) {
        ip_edit_->setText(QString::fromStdString(previous->ip_address));
        route_edit_->setText(QString::fromStdString(previous->route));

        // Set rate combo
        uint32_t rate = previous->sample_rate_us;
        int idx = rate_combo_->findData(rate);
        if (idx >= 0) {
            rate_combo_->setCurrentIndex(idx);
        } else {
            rate_combo_->setCurrentIndex(rate_combo_->count() - 1); // "Custom"
            rate_custom_spin_->setValue(static_cast<int>(rate / 1000));
            rate_custom_spin_->setVisible(true);
        }
    }
}

void LogixConfigDialog::setupUi() {
    auto* main_layout = new QVBoxLayout(this);

    // Helper to create bold section headers
    auto makeSectionLabel = [](const QString& text) {
        auto* label = new QLabel(text);
        QFont bold_font = label->font();
        bold_font.setPointSize(12);
        bold_font.setBold(true);
        label->setFont(bold_font);
        return label;
    };

    // ── Connection ──────────────────────────────────────────────────────
    main_layout->addWidget(makeSectionLabel("PLC Connection"));

    auto* conn_layout = new QHBoxLayout();
    conn_layout->addWidget(new QLabel("IP Address:"));
    ip_edit_ = new QLineEdit("192.168.1.1");
    ip_edit_->setPlaceholderText("e.g. 192.168.1.1");
    conn_layout->addWidget(ip_edit_);

    conn_layout->addWidget(new QLabel("CIP Route:"));
    route_edit_ = new QLineEdit();
    route_edit_->setPlaceholderText("e.g. 1,0 or 1,4,2,10.10.10.9");
    route_edit_->setToolTip("Optional CIP route as comma-separated port,link pairs.\n"
                            "Leave empty for direct connection.\n"
                            "Examples:\n"
                            "  1,0 - backplane port 1, slot 0\n"
                            "  1,4,2,10.10.10.9 - backplane slot 4, then ethernet to IP");
    conn_layout->addWidget(route_edit_);

    connect_btn_ = new QPushButton("Connect && Browse Tags");
    conn_layout->addWidget(connect_btn_);

    status_label_ = new QLabel("");
    conn_layout->addWidget(status_label_);

    main_layout->addLayout(conn_layout);

    // ── Tag Selection ───────────────────────────────────────────────────
    main_layout->addWidget(makeSectionLabel("Tag Selection"));

    auto* filter_layout = new QHBoxLayout();
    filter_layout->addWidget(new QLabel("Filter:"));
    filter_edit_ = new QLineEdit();
    filter_edit_->setPlaceholderText("Type to filter tags...");
    filter_layout->addWidget(filter_edit_);
    select_all_btn_ = new QPushButton("Select All");
    deselect_all_btn_ = new QPushButton("Deselect All");
    filter_layout->addWidget(select_all_btn_);
    filter_layout->addWidget(deselect_all_btn_);
    main_layout->addLayout(filter_layout);

    tag_tree_ = new QTreeWidget();
    tag_tree_->setHeaderLabels({"Tag Name", "Data Type", "Array"});
    tag_tree_->header()->setStretchLastSection(false);
    tag_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tag_tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tag_tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    main_layout->addWidget(tag_tree_);

    // ── Sample Rate ─────────────────────────────────────────────────────
    main_layout->addWidget(makeSectionLabel("Sample Rate"));

    auto* rate_layout = new QHBoxLayout();
    rate_layout->addWidget(new QLabel("Rate:"));
    rate_combo_ = new QComboBox();
    rate_combo_->addItem("1 ms", 1000u);
    rate_combo_->addItem("5 ms", 5000u);
    rate_combo_->addItem("10 ms", 10000u);
    rate_combo_->addItem("50 ms", 50000u);
    rate_combo_->addItem("100 ms", 100000u);
    rate_combo_->addItem("500 ms", 500000u);
    rate_combo_->addItem("1 s", 1000000u);
    rate_combo_->addItem("Custom...", 0u);
    rate_combo_->setCurrentIndex(2); // default 10ms

    rate_layout->addWidget(rate_combo_);

    rate_custom_spin_ = new QSpinBox();
    rate_custom_spin_->setRange(1, 60000);
    rate_custom_spin_->setValue(10);
    rate_custom_spin_->setSuffix(" ms");
    rate_custom_spin_->setVisible(false);
    rate_layout->addWidget(rate_custom_spin_);
    rate_layout->addStretch();

    main_layout->addLayout(rate_layout);

    // ── RAM Estimate ────────────────────────────────────────────────────
    ram_label_ = new QLabel();
    ram_label_->setTextFormat(Qt::RichText);
    main_layout->addWidget(ram_label_);

    // ── Buttons ─────────────────────────────────────────────────────────
    auto* btn_layout = new QHBoxLayout();
    btn_layout->addStretch();
    ok_btn_ = new QPushButton("Start Trending");
    ok_btn_->setEnabled(false);
    cancel_btn_ = new QPushButton("Cancel");
    btn_layout->addWidget(ok_btn_);
    btn_layout->addWidget(cancel_btn_);
    main_layout->addLayout(btn_layout);

    // ── Connections ─────────────────────────────────────────────────────
    connect(connect_btn_, &QPushButton::clicked, this, &LogixConfigDialog::onConnect);
    connect(select_all_btn_, &QPushButton::clicked, this, &LogixConfigDialog::onSelectAll);
    connect(deselect_all_btn_, &QPushButton::clicked, this, &LogixConfigDialog::onDeselectAll);
    connect(filter_edit_, &QLineEdit::textChanged, this, &LogixConfigDialog::onFilterChanged);
    connect(ok_btn_, &QPushButton::clicked, this, &LogixConfigDialog::onAccept);
    connect(cancel_btn_, &QPushButton::clicked, this, &QDialog::reject);

    connect(rate_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int) {
                bool is_custom = (rate_combo_->currentData().toUInt() == 0);
                rate_custom_spin_->setVisible(is_custom);
                updateRamEstimate();
            });
    connect(rate_custom_spin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LogixConfigDialog::updateRamEstimate);
    connect(tag_tree_, &QTreeWidget::itemChanged,
            this, &LogixConfigDialog::updateRamEstimate);
}

void LogixConfigDialog::onConnect() {
    status_label_->setText("Connecting...");
    QApplication::processEvents();

    std::string ip = ip_edit_->text().toStdString();
    std::string route_str = route_edit_->text().toStdString();

    try {
        auto route = EipConnection::parseRouteString(route_str);
        conn_ = std::make_unique<EipConnection>();
        conn_->connect(ip, route);

        status_label_->setText("Browsing tags...");
        QApplication::processEvents();

        all_tags_ = browser_.browse(*conn_);

        populateTagTree(all_tags_);
        status_label_->setText(QString("Found %1 tags").arg(all_tags_.size()));
        ok_btn_->setEnabled(true);

    } catch (const std::exception& e) {
        status_label_->setText("Error");
        QMessageBox::critical(this, "Connection Error",
                              QString("Failed to connect:\n%1").arg(e.what()));
        conn_.reset();
    }
}

void LogixConfigDialog::populateTagTree(const std::vector<TagInfo>& tags) {
    tag_tree_->clear();

    // Group tags by program (or "Controller" for non-program tags)
    std::map<std::string, QTreeWidgetItem*> program_items;

    for (const auto& tag : tags) {
        // Determine group
        std::string group = "Controller Tags";
        std::string display_name = tag.name;

        auto prog_pos = tag.name.find("Program:");
        if (prog_pos == 0) {
            auto dot_pos = tag.name.find('.', 8);
            if (dot_pos != std::string::npos) {
                group = tag.name.substr(0, dot_pos);
                display_name = tag.name.substr(dot_pos + 1);
            }
        }

        // Get or create group item
        if (program_items.find(group) == program_items.end()) {
            auto* group_item = new QTreeWidgetItem(tag_tree_);
            group_item->setText(0, QString::fromStdString(group));
            group_item->setFlags(group_item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            group_item->setCheckState(0, Qt::Unchecked);
            group_item->setExpanded(false);
            program_items[group] = group_item;
        }

        QTreeWidgetItem* parent = program_items[group];

        if (tag.is_struct) {
            // Create expandable struct node
            auto* struct_item = new QTreeWidgetItem(parent);
            struct_item->setText(0, QString::fromStdString(display_name));
            struct_item->setText(1, QString::fromStdString(tag.data_type_name));
            struct_item->setFlags(struct_item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            struct_item->setCheckState(0, Qt::Unchecked);

            // Add numeric members as checkable children
            auto members = browser_.expandStructMembers(tag);
            for (const auto& [member_name, member_type] : members) {
                // Extract just the member portion after the base tag name
                std::string member_display = member_name;
                auto base_dot = tag.name.size();
                if (base_dot < member_name.size()) {
                    member_display = member_name.substr(base_dot + 1);
                }

                auto* member_item = new QTreeWidgetItem(struct_item);
                member_item->setText(0, QString::fromStdString(member_display));
                member_item->setText(1, QString::fromStdString(cipTypeName(member_type)));
                member_item->setFlags(member_item->flags() | Qt::ItemIsUserCheckable);
                member_item->setCheckState(0, Qt::Unchecked);
                // Store full tag path and type in user data
                member_item->setData(0, Qt::UserRole, QString::fromStdString(member_name));
                member_item->setData(1, Qt::UserRole, member_type);
            }

            if (members.empty()) {
                // No numeric members — show as non-checkable
                struct_item->setText(2, "(no numeric members)");
            }
        } else if (isNumericType(tag.symbol_type)) {
            if (tag.array_dims > 0 && tag.array_size > 0) {
                // Array of numeric type — expand into individual elements
                auto* array_item = new QTreeWidgetItem(parent);
                array_item->setText(0, QString::fromStdString(display_name));
                array_item->setText(1, QString::fromStdString(tag.data_type_name));
                array_item->setText(2, QString("[%1]").arg(tag.array_size));
                array_item->setFlags(array_item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                array_item->setCheckState(0, Qt::Unchecked);

                int max_elements = std::min(tag.array_size, 64);
                for (int i = 0; i < max_elements; i++) {
                    auto* elem_item = new QTreeWidgetItem(array_item);
                    std::string elem_name = tag.name + "[" + std::to_string(i) + "]";
                    elem_item->setText(0, QString("[%1]").arg(i));
                    elem_item->setText(1, QString::fromStdString(tag.data_type_name));
                    elem_item->setFlags(elem_item->flags() | Qt::ItemIsUserCheckable);
                    elem_item->setCheckState(0, Qt::Unchecked);
                    elem_item->setData(0, Qt::UserRole, QString::fromStdString(elem_name));
                    elem_item->setData(1, Qt::UserRole, tag.symbol_type);
                }

                if (tag.array_size > 64) {
                    auto* truncated = new QTreeWidgetItem(array_item);
                    truncated->setText(0, QString("... %1 more elements").arg(tag.array_size - 64));
                }
            } else {
                // Scalar numeric tag — directly checkable
                auto* tag_item = new QTreeWidgetItem(parent);
                tag_item->setText(0, QString::fromStdString(display_name));
                tag_item->setText(1, QString::fromStdString(tag.data_type_name));
                tag_item->setFlags(tag_item->flags() | Qt::ItemIsUserCheckable);
                tag_item->setCheckState(0, Qt::Unchecked);
                tag_item->setData(0, Qt::UserRole, QString::fromStdString(tag.name));
                tag_item->setData(1, Qt::UserRole, tag.symbol_type);
            }
        }
        // Non-numeric, non-struct tags (strings etc.) are skipped
    }
}

void LogixConfigDialog::onSelectAll() {
    std::function<void(QTreeWidgetItem*)> setAll = [&](QTreeWidgetItem* item) {
        if (item->flags() & Qt::ItemIsUserCheckable) {
            if (!item->isHidden()) {
                item->setCheckState(0, Qt::Checked);
            }
        }
        for (int i = 0; i < item->childCount(); i++) {
            setAll(item->child(i));
        }
    };

    for (int i = 0; i < tag_tree_->topLevelItemCount(); i++) {
        setAll(tag_tree_->topLevelItem(i));
    }
}

void LogixConfigDialog::onDeselectAll() {
    std::function<void(QTreeWidgetItem*)> clearAll = [&](QTreeWidgetItem* item) {
        if (item->flags() & Qt::ItemIsUserCheckable) {
            item->setCheckState(0, Qt::Unchecked);
        }
        for (int i = 0; i < item->childCount(); i++) {
            clearAll(item->child(i));
        }
    };

    for (int i = 0; i < tag_tree_->topLevelItemCount(); i++) {
        clearAll(tag_tree_->topLevelItem(i));
    }
}

void LogixConfigDialog::onFilterChanged(const QString& text) {
    applyFilter(text);
}

void LogixConfigDialog::applyFilter(const QString& filter) {
    std::function<bool(QTreeWidgetItem*)> filterItem = [&](QTreeWidgetItem* item) -> bool {
        bool any_child_visible = false;

        for (int i = 0; i < item->childCount(); i++) {
            if (filterItem(item->child(i))) {
                any_child_visible = true;
            }
        }

        bool matches = filter.isEmpty() ||
                       item->text(0).contains(filter, Qt::CaseInsensitive);
        bool visible = matches || any_child_visible;
        item->setHidden(!visible);

        if (!filter.isEmpty() && visible) {
            item->setExpanded(true);
        }

        return visible;
    };

    for (int i = 0; i < tag_tree_->topLevelItemCount(); i++) {
        filterItem(tag_tree_->topLevelItem(i));
    }
}

void LogixConfigDialog::collectSelectedTags() {
    config_.selected_tags.clear();

    std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
        if ((item->flags() & Qt::ItemIsUserCheckable) &&
            item->checkState(0) == Qt::Checked) {
            QString tag_name = item->data(0, Qt::UserRole).toString();
            uint16_t data_type = item->data(1, Qt::UserRole).toUInt();
            if (!tag_name.isEmpty()) {
                config_.selected_tags.push_back({tag_name.toStdString(), data_type});
            }
        }
        for (int i = 0; i < item->childCount(); i++) {
            collect(item->child(i));
        }
    };

    for (int i = 0; i < tag_tree_->topLevelItemCount(); i++) {
        collect(tag_tree_->topLevelItem(i));
    }
}

void LogixConfigDialog::onAccept() {
    collectSelectedTags();

    if (config_.selected_tags.empty()) {
        QMessageBox::warning(this, "No Tags Selected",
                             "Please select at least one tag to trend.");
        return;
    }

    if (config_.selected_tags.size() > kMaxTrendInstances) {
        QMessageBox::warning(this, "Too Many Tags",
                             QString("Maximum %1 tags can be trended simultaneously.\n"
                                     "You selected %2. Please deselect some tags.")
                                 .arg(kMaxTrendInstances)
                                 .arg(config_.selected_tags.size()));
        return;
    }

    config_.ip_address = ip_edit_->text().toStdString();
    config_.route = route_edit_->text().toStdString();

    uint32_t rate = rate_combo_->currentData().toUInt();
    if (rate == 0) {
        rate = static_cast<uint32_t>(rate_custom_spin_->value()) * 1000;
    }
    config_.sample_rate_us = rate;

    // Warn if estimated PLC RAM exceeds 50 KB
    uint32_t conn_size = conn_ ? conn_->connectionSize() : 4002;
    uint32_t total_ram = 0;
    for (const auto& [name, data_type] : config_.selected_tags) {
        total_ram += estimateTagBufferSize(rate, data_type, conn_size) + 164;
    }
    if (total_ram > 50 * 1024) {
        double kb = total_ram / 1024.0;
        auto reply = QMessageBox::warning(this, "High PLC Memory Usage",
            QString("Estimated PLC memory: %1 KB (%2 tags).\n"
                    "This may impact PLCs with limited RAM.\n\nContinue?")
                .arg(kb, 0, 'f', 1)
                .arg(config_.selected_tags.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    accept();
}

// ─── RAM Estimate ───────────────────────────────────────────────────────────

uint32_t LogixConfigDialog::estimateTagBufferSize(uint32_t sample_rate_us,
                                                    uint16_t data_type,
                                                    uint32_t connection_size) {
    int value_size = cipTypeSize(data_type);
    if (value_size == 0) value_size = 4;
    int sample_size = 6 + value_size;

    // Same formula as computeBufferParams: fill max CIP response
    uint32_t max_payload = connection_size > 20 ? connection_size - 20 : 480;
    uint32_t max_samples = max_payload / sample_size;
    uint32_t buffer_size = max_samples * sample_size;
    return std::max(buffer_size, static_cast<uint32_t>(sample_size * 4));
}

void LogixConfigDialog::updateRamEstimate() {
    uint32_t rate = rate_combo_->currentData().toUInt();
    if (rate == 0) {
        rate = static_cast<uint32_t>(rate_custom_spin_->value()) * 1000;
    }
    if (rate == 0) rate = 10000;

    uint32_t conn_size = conn_ ? conn_->connectionSize() : 4002;

    uint32_t tag_count = 0;
    uint32_t total_ram = 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if ((item->flags() & Qt::ItemIsUserCheckable) &&
            item->checkState(0) == Qt::Checked) {
            QString tag_name = item->data(0, Qt::UserRole).toString();
            uint16_t data_type = item->data(1, Qt::UserRole).toUInt();
            if (!tag_name.isEmpty() && data_type > 0) {
                total_ram += estimateTagBufferSize(rate, data_type, conn_size) + 164;
                tag_count++;
            }
        }
        for (int i = 0; i < item->childCount(); i++) {
            walk(item->child(i));
        }
    };
    for (int i = 0; i < tag_tree_->topLevelItemCount(); i++) {
        walk(tag_tree_->topLevelItem(i));
    }

    if (tag_count == 0) {
        ram_label_->setText("");
        return;
    }

    double kb = total_ram / 1024.0;
    QString text;
    if (tag_count > kMaxTrendInstances) {
        text = QString("<b style='color: red;'>%1/%2 tags (max %2) | PLC Memory: %3 KB</b>")
                   .arg(tag_count).arg(kMaxTrendInstances).arg(kb, 0, 'f', 1);
    } else if (total_ram > 50 * 1024) {
        text = QString("<b style='color: red;'>%1/%2 tags | PLC Memory: %3 KB "
                        "— exceeds 50 KB limit</b>")
                   .arg(tag_count).arg(kMaxTrendInstances).arg(kb, 0, 'f', 1);
    } else {
        text = QString("%1/%2 tags | PLC Memory: %3 KB")
                   .arg(tag_count).arg(kMaxTrendInstances).arg(kb, 0, 'f', 1);
    }
    ram_label_->setText(text);
}

} // namespace logix
