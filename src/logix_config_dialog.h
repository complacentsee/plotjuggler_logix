/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

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

namespace logix
{

/// Configuration result from the dialog
struct LogixConfig
{
  std::string ip_address;
  std::string route;                // Comma-separated CIP route, empty = direct connection
  uint32_t sample_rate_us = 10000;  // 10ms default
  std::vector<std::pair<std::string, uint16_t>> selected_tags;  // (name, data_type)
};

/// Qt dialog for configuring the Logix trend connection
class LogixConfigDialog : public QDialog
{
  Q_OBJECT

public:
  explicit LogixConfigDialog(QWidget* parent = nullptr, const LogixConfig* previous = nullptr,
                             const std::vector<TagInfo>& cached_tags = {},
                             const TagBrowser& cached_browser = TagBrowser());

  LogixConfig getConfig() const
  {
    return config_;
  }
  const std::vector<TagInfo>& getTags() const
  {
    return all_tags_;
  }
  const TagBrowser& getBrowser() const
  {
    return browser_;
  }

private slots:
  void onBrowse();
  void onConnectionFieldsChanged();
  void onSelectAll();
  void onDeselectAll();
  void onFilterChanged(const QString& text);
  void onAccept();
  void updateRamEstimate();

private:
  void setupUi();
  void populateTagTree(const std::vector<TagInfo>& tags);
  void applyFilter(const QString& filter);
  void collectSelectedTags();
  void restoreTagSelections();

  /// Estimate PLC buffer bytes for one trend instance with num_tags tags
  static uint32_t estimateTagBufferSize(uint32_t sample_rate_us, size_t num_tags,
                                        uint32_t connection_size);

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

  // RAM estimate
  QLabel* ram_label_ = nullptr;

  // Action buttons
  QPushButton* ok_btn_ = nullptr;
  QPushButton* cancel_btn_ = nullptr;

  // State
  LogixConfig config_;
  TagBrowser browser_;
  std::vector<TagInfo> all_tags_;
  std::vector<std::pair<std::string, uint16_t>> previous_tags_;  // selections to restore
};

}  // namespace logix
