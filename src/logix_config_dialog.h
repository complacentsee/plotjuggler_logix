#pragma once

#include "logix_tag_browser.h"
#include "logix_eip.h"

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QStringList>

#include <memory>
#include <vector>
#include <string>

namespace logix {

/// Configuration result from the dialog
struct LogixConfig {
    std::string ip_address;
    std::string route;  // Comma-separated CIP route, empty = direct connection
    uint32_t sample_rate_us = 10000; // 10ms default
    std::vector<std::pair<std::string, uint16_t>> selected_tags; // (name, data_type)
};

/// Qt dialog for configuring the Logix trend connection
class LogixConfigDialog : public QDialog {
    Q_OBJECT

public:
    explicit LogixConfigDialog(QWidget* parent = nullptr,
                                const LogixConfig* previous = nullptr);

    LogixConfig getConfig() const { return config_; }

private slots:
    void onConnect();
    void onSelectAll();
    void onDeselectAll();
    void onFilterChanged(const QString& text);
    void onAccept();

private:
    void setupUi();
    void populateTagTree(const std::vector<TagInfo>& tags);
    void applyFilter(const QString& filter);
    void collectSelectedTags();

    // Connection widgets
    QLineEdit* ip_edit_ = nullptr;
    QLineEdit* route_edit_ = nullptr;
    QPushButton* connect_btn_ = nullptr;
    QLabel* status_label_ = nullptr;

    // Tag tree
    QLineEdit* filter_edit_ = nullptr;
    QTreeWidget* tag_tree_ = nullptr;
    QPushButton* select_all_btn_ = nullptr;
    QPushButton* deselect_all_btn_ = nullptr;

    // Sample rate
    QComboBox* rate_combo_ = nullptr;
    QSpinBox* rate_custom_spin_ = nullptr;

    // Action buttons
    QPushButton* ok_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;

    // State
    LogixConfig config_;
    std::unique_ptr<EipConnection> conn_;
    TagBrowser browser_;
    std::vector<TagInfo> all_tags_;
};

} // namespace logix
