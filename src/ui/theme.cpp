#include "theme.h"
#include "ui/icons.h"

namespace testtools::ui {

QIcon navigationChevron(bool expanded, bool selected)
{
    return appIcon(expanded ? Icon::ChevronDown : Icon::ChevronRight,
                   selected ? IconTone::Accent : IconTone::Normal);
}

QString applicationStyleSheet()
{
    const QString chevronPath = QStringLiteral(":/testtools/icons/controls/chevron.svg");
    const QString checkPath = QStringLiteral(":/testtools/icons/controls/check.svg");
    const QString mixedPath = QStringLiteral(":/testtools/icons/controls/mixed.svg");

    return QStringLiteral(R"(
        QWidget {
            color: #302f2d;
            font-family: "Microsoft YaHei UI";
            font-size: 14px;
            font-weight: 400;
        }
        QMainWindow, QWidget#appSurface { background: #f7f6f4; }
        QDialog#atCommandDialog { background: #fbfaf9; }
        QTableWidget, QTreeWidget, QListWidget {
            background: #ffffff; alternate-background-color: #faf9f8;
            border: 1px solid #dedad5; gridline-color: #ebe8e4;
            border-radius: 8px;
            selection-background-color: #f0e6e1; selection-color: #302f2d;
        }
        QTableWidget::item, QTreeWidget::item, QListWidget::item { padding: 7px 9px; border: 0; }
        QTableWidget::item:selected, QTreeWidget::item:selected, QListWidget::item:selected { background: #f0e6e1; color: #302f2d; }
        QHeaderView::section {
            background: #f6f3f0; color: #67615c; padding: 7px 8px;
            border: 0; border-bottom: 1px solid #dedad5;
            font-size: 12px; font-weight: 500;
        }
        QFrame#filePane, QFrame#mirrorGuide {
            background: #ffffff; border: 1px solid #dedad5; border-radius: 8px;
        }
        QTreeView#fileLocalTable, QTreeView#fileRemoteTable {
            background: #ffffff; alternate-background-color: #faf9f8;
            border: 0; border-top: 1px solid #ebe8e4; border-bottom: 1px solid #ebe8e4;
            selection-background-color: #e9ddd5; selection-color: #302f2d;
        }
        QTreeView#fileLocalTable::item, QTreeView#fileRemoteTable::item {
            min-height: 28px; padding: 3px 5px;
        }
        QLabel#filePaneMessage { color: #88633f; padding: 6px; background: #f9f3e9; border-radius: 4px; }
        QToolButton { border: 1px solid transparent; border-radius: 4px; padding: 3px; }
        QToolButton:hover { background: #ede8e2; border-color: #dedad5; }
        QToolButton:focus { border-color: #9b7c64; }
    )" R"(
        QFrame#topBar {
            background: #ffffff;
            border: 0;
            border-bottom: 1px solid #e2dfdb;
        }
        QLabel#appTitle { color: #302f2d; font-size: 18px; font-weight: 600; }
        QLabel#appSubtitle { color: #77716c; font-size: 12px; }
        QLabel#topCaption { color: #77716c; font-size: 12px; font-weight: 400; }
        QLabel#topStatusNeutral, QLabel#topStatusGood, QLabel#topStatusWarn, QLabel#topStatusError {
            padding: 3px 10px;
            border-radius: 12px;
            font-size: 12px;
        }
        QLabel#topStatusNeutral {
            background: #f0eeeb;
            color: #67615c;
            border: 1px solid #e4e0da;
            font-weight: 400;
        }
        QLabel#topStatusGood {
            background: #e7f0e8;
            color: #3f6244;
            border: 1px solid #cfe0d1;
            font-weight: 500;
        }
        QLabel#topStatusWarn {
            background: #f6efe2;
            color: #87652f;
            border: 1px solid #e6d6bc;
            font-weight: 500;
        }
        QLabel#topStatusError {
            background: #f7e9e8;
            color: #a34a42;
            border: 1px solid #e8c9c6;
            font-weight: 500;
        }
        QFrame#navigationRail {
            background: #f2f1ef;
            border: 0;
            border-right: 1px solid #e0ddd9;
        }
        QPushButton#navigationButton {
            min-height: 38px;
            padding: 0 12px;
            border: 0;
            border-left: 3px solid transparent;
            border-radius: 8px;
            background: transparent;
            color: #625d59;
            text-align: left;
            font-weight: 400;
        }
        QPushButton#navigationButton:hover { background: #ece9e6; color: #302f2d; }
        QPushButton#navigationButton:checked {
            background: #eee4df;
            color: #754837;
            border-left-color: #b56e52;
            font-weight: 600;
        }
        QPushButton#navigationGroupButton {
            min-height: 38px; padding: 0 10px; border: 0; border-radius: 6px;
            background: transparent; text-align: left; color: #494541; font-size: 14px; font-weight: 600;
        }
        QFrame#navigationGroupRow { background: transparent; border: 0; border-radius: 6px; }
        QFrame#navigationGroupRow:hover { background: #ece9e6; }
        QFrame#navigationGroupRow[active="true"] { background: #e9e5e0; }
        QPushButton#navigationGroupButton:hover { background: transparent; }
        QPushButton#navigationGroupButton:checked { background: transparent; color: #694d3f; }
        QFrame#navigationGroupRow QToolButton { background: transparent; border: 1px solid transparent; }
        QFrame#navigationGroupRow QToolButton:focus { border-color: #b56e52; }
        QPushButton#navigationChildButton {
            min-height: 36px; padding: 0 10px; border: 0; border-left: 3px solid transparent;
            border-radius: 6px; background: transparent; color: #77716c; text-align: left; font-size: 13px;
        }
        QPushButton#navigationChildButton:hover { background: #ece9e6; color: #302f2d; }
        QPushButton#navigationChildButton:checked { background: #eee0d7; border-left-color: #ad694e; color: #754837; font-weight: 600; }
        QPushButton#navigationGroupButton:focus, QPushButton#navigationChildButton:focus { border: 1px solid #b56e52; }
        QPushButton#serviceStatusButton { min-height: 30px; padding: 0 12px; background: #f2f0ed; border: 1px solid #e1ddd8; border-radius: 8px; color: #67615c; font-size: 12px; }
        QLabel#topServiceStatus { font-size: 12px; color: #67615c; }
        QLabel#topServiceStatus[serviceState="ready"] { color: #5d715f; }
        QLabel#topServiceStatus[serviceState="working"] { color: #87652f; }
        QLabel#topServiceStatus[serviceState="failed"] { color: #a34a42; }
        QPushButton#serviceStatusButton[serviceState="failed"] { background: #f7eeeb; color: #a34a42; border-color: #e8c9c6; }
        QPushButton#serviceStatusButton[needsConnection="true"]:enabled { background: #ad694e; border-color: #ad694e; color: #ffffff; font-weight: 600; }
        QPushButton#serviceStatusButton[needsConnection="true"]:enabled:hover { background: #985b43; border-color: #985b43; color: #ffffff; }
        QPushButton#serviceStatusButton:hover { background: #e9e4de; border-color: #b9a99c; color: #493e36; }
        QPushButton#serviceStatusButton:pressed { background: #ded3c9; border-color: #ad694e; }
        QPushButton#serviceStatusButton:disabled { background: #f3f1ee; border-color: #e1ddd8; color: #8b837b; }
        QFrame#serialServiceHint { background: transparent; border: 0; }
        QLabel#serialServiceMessage { color: #87652f; font-size: 12px; }
        QGroupBox#endpointCard[singleChannel="true"] { background: transparent; border: 0; border-radius: 0; }
    )" R"(
        QScrollArea#pageScroll { border: 0; background: transparent; }
        QWidget#pageSurface { background: transparent; }
        QLabel#pageTitle { color: #302f2d; font-size: 20px; font-weight: 600; }
        QLabel#pageSubtitle { color: #77716c; font-size: 13px; }
        QFrame#activityBar {
            background: #ffffff;
            border: 0;
            border-top: 1px solid #e2dfdb;
        }
        QLabel#activityTitle { color: #302f2d; font-size: 12px; font-weight: 600; }
        QLabel#activityMessage { color: #67615c; font-size: 12px; }
        QFrame#contextPanel, QFrame#featureCard, QGroupBox#endpointCard {
            background: #ffffff;
            border: 1px solid #dfdbd6;
            border-radius: 10px;
        }
        QFrame#taskModeBar {
            background: transparent;
            border: 0;
        }
        QFrame#segmentedBar {
            background: #eae6e1;
            border: 0;
            border-radius: 8px;
        }
        QPushButton#modeButton {
            min-height: 28px;
            padding: 0 14px;
            border: 0;
            border-radius: 6px;
            background: transparent;
            color: #625d59;
            font-size: 13px;
        }
        QPushButton#modeButton:hover { color: #302f2d; }
        QPushButton#modeButton:checked {
            background: #ffffff;
            color: #754837;
            font-weight: 600;
        }
        QPushButton#modeButton:focus { border: 1px solid #b56e52; }
        QLabel#modeDescription { color: #817a74; font-size: 12px; }
        QLabel#modeDescription[notice="true"] { color: #715239; font-weight: 600; }
        QFrame#serialFeedback {
            background: #f6efe2; border: 1px solid #e5d8c1; border-radius: 6px;
        }
        QFrame#serialFeedback[tone="error"] { background: #f7e9e8; border-color: #e9c9c4; }
        QLabel#serialFeedbackText { color: #70583a; font-size: 12px; }
        QFrame#serialFeedback[tone="error"] QLabel#serialFeedbackText { color: #963f38; }
        QFrame#summaryCard {
            background: transparent;
            border: 0;
        }
        QFrame#summaryDivider { background: #e4e1dd; border: 0; }
        QFrame#connectionRow {
            background: transparent;
            border: 0;
            border-bottom: 1px solid #ece9e4;
        }
        QFrame#terminalFrame {
            background: #faf9f7;
            border: 1px solid #d8d3cd;
            border-radius: 8px;
        }
        QFrame#terminalHeader {
            background: #f3f1ee;
            border: 0;
            border-bottom: 1px solid #e0dbd5;
            border-top-left-radius: 7px;
            border-top-right-radius: 7px;
        }
        QFrame#atCommandBar {
            background: #f7f3f0;
            border: 1px solid #e4dcd5;
            border-radius: 8px;
        }
        QLabel#shortcutTag {
            background: #ede1db;
            color: #754837;
            border-radius: 5px;
            padding: 3px 8px;
            font-size: 12px;
            font-weight: 600;
        }
        QFrame#shortcutSidebar { background: #ffffff; border: 1px solid #dedad5; border-radius: 8px; }
        QListWidget#shortcutList { background: transparent; border: 0; }
        QListWidget#shortcutList::item { padding: 0; border-radius: 5px; }
        QListWidget#shortcutList::item:selected { background: #f0e6e1; }
        QLabel#shortcutName { font-size: 13px; font-weight: 600; background: transparent; }
        QLabel#shortcutPreview { font-size: 12px; color: #77716c; background: transparent; }
        QFrame#composerFrame {
            background: #ffffff;
            border: 1px solid #cfc9c2;
            border-radius: 8px;
        }
        QFrame#composerFooter {
            background: #f5f3f0;
            border: 0;
            border-top: 1px solid #e4e0da;
            border-bottom-left-radius: 7px;
            border-bottom-right-radius: 7px;
        }
        QFrame#inspectionToolbar {
            background: transparent;
            border: 0;
            border-bottom: 1px solid #e2dfdb;
        }
        QGroupBox#detailGroup {
            background: #ffffff;
            border: 1px solid #dfdbd6;
            border-radius: 10px;
            margin-top: 12px;
        }
        QGroupBox#detailGroup::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
            color: #625d59;
        }
        QFrame#linkBar, QFrame#serialResults {
            background: #ffffff;
            border: 1px solid #dfdbd6;
            border-radius: 10px;
        }
        QLabel#cardEyebrow { color: #817a74; font-size: 12px; font-weight: 400; }
        QLabel#cardValue { color: #302f2d; font-size: 18px; font-weight: 600; }
        QLabel#cardBody { color: #625d59; }
        QLabel#routeSummary { color: #302f2d; font-size: 14px; font-weight: 600; }
        QLabel#sectionLabel { color: #302f2d; font-size: 14px; font-weight: 600; }
        QLabel#fieldLabel { color: #77716c; font-size: 12px; font-weight: 400; }
        QLabel#mutedLabel { color: #77716c; font-size: 12px; background: transparent; border: 0; padding: 0; }
        QLabel#counterLabel {
            background: #eae6e1;
            color: #4f4b47;
            border-radius: 5px;
            padding: 3px 9px;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
        QLabel#endpointBadgeA, QLabel#endpointBadgeB {
            background: #ebe8e5;
            color: #625d59;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 600;
        }
        QLabel#endpointTitle { color: #302f2d; font-size: 15px; font-weight: 600; }
        QLabel#connectionStatusIdle, QLabel#connectionStatusWorking,
        QLabel#connectionStatusGood, QLabel#connectionStatusError {
            padding: 3px 10px;
            border-radius: 12px;
            font-size: 12px;
            font-weight: 500;
        }
        QLabel#connectionStatusIdle {
            background: #f0eeeb;
            color: #67615c;
            border: 1px solid #e2ddd7;
        }
        QLabel#connectionStatusWorking {
            background: #f6efe2;
            color: #87652f;
            border: 1px solid #e6d6bc;
        }
        QLabel#connectionStatusGood {
            background: #e7f0e8;
            color: #3f6244;
            border: 1px solid #cfe0d1;
        }
        QLabel#connectionStatusError {
            background: #f7e9e8;
            color: #a34a42;
            border: 1px solid #e8c9c6;
        }
        QLabel#topStatusNeutral, QLabel#topStatusGood, QLabel#topStatusWarn, QLabel#topStatusError,
        QLabel#connectionStatusIdle, QLabel#connectionStatusWorking,
        QLabel#connectionStatusGood, QLabel#connectionStatusError {
            background: transparent; border: 0; border-radius: 0; padding: 3px 2px;
        }
        QLabel#testStatusNeutral, QLabel#testStatusWorking, QLabel#testStatusWarn,
        QLabel#testStatusGood, QLabel#testStatusError {
            background: transparent;
            padding: 0;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#testStatusNeutral { color: #817a74; }
        QLabel#testStatusWorking { color: #87652f; }
        QLabel#testStatusWarn { color: #87652f; font-weight: 600; }
        QLabel#testStatusGood { color: #5d715f; }
        QLabel#testStatusError { color: #a34a42; }
        QFrame#serialResults QLabel { font-size: 13px; }
        QComboBox, QLineEdit, QSpinBox {
            min-height: 32px;
            padding: 0 10px;
            background: #ffffff;
            border: 1px solid #cfc9c2;
            border-radius: 7px;
            font-size: 13px;
            selection-background-color: #f0e6e1;
            selection-color: #302f2d;
        }
        QComboBox {
            padding-right: 26px;
        }
        QComboBox QLineEdit {
            border: 0;
            padding: 0;
            background: transparent;
            min-height: 0;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 24px;
            border: 0;
            background: transparent;
        }
        QComboBox::down-arrow {
            image: url(%1);
            width: 11px;
            height: 11px;
        }
        QComboBox QAbstractItemView {
            background: #ffffff;
            border: 1px solid #cfc9c2;
            border-radius: 7px;
            padding: 4px;
            selection-background-color: #f0e6e1;
            selection-color: #302f2d;
            outline: 0;
        }
        QComboBox:hover, QLineEdit:hover, QSpinBox:hover { border-color: #a79e96; }
        QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border: 1px solid #b56e52; }
        QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled {
            background: #eeece9; color: #918a84; border-color: #ddd8d3;
        }
        QFrame#topBar QComboBox {
            min-width: 260px;
            max-width: 340px;
            background: #ffffff;
            color: #302f2d;
            border-color: #cfc9c2;
        }
        QPlainTextEdit {
            border: 1px solid #cfc9c2;
            border-radius: 8px;
            padding: 8px;
            background: #ffffff;
            selection-background-color: #f0e6e1;
            selection-color: #302f2d;
            placeholder-text-color: #918a84;
        }
        QPlainTextEdit:focus { border: 1px solid #b56e52; }
        QPlainTextEdit#receiveConsole {
            background: #faf9f7;
            color: #302f2d;
            border: 0;
            border-bottom-left-radius: 7px;
            border-bottom-right-radius: 7px;
            padding: 8px 12px;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 13px;
        }
        QPlainTextEdit#receiveConsole:focus { border: 0; }
        QPlainTextEdit#sendEditor {
            background: #ffffff;
            border: 0;
            border-top-left-radius: 7px;
            border-top-right-radius: 7px;
            padding: 6px 10px;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 13px;
        }
        QPlainTextEdit#sendEditor:focus { border: 0; }
        QPushButton {
            min-height: 32px;
            padding: 0 12px;
            border: 1px solid #cfc9c2;
            border-radius: 7px;
            background: #ffffff;
            color: #302f2d;
            font-size: 13px;
            font-weight: 400;
        }
        QPushButton:hover { background: #f3f0ed; border-color: #a79e96; }
        QPushButton:pressed { background: #ebe7e3; }
        QPushButton:focus { border: 1px solid #b56e52; }
        QPushButton:disabled { color: #918a84; background: #eeece9; border-color: #ddd8d3; }
        QPushButton#primaryButton, QPushButton[buttonRole="primary"] { background: #ad694e; color: #ffffff; border-color: #ad694e; font-weight: 600; }
        QPushButton#primaryButton:hover, QPushButton[buttonRole="primary"]:hover { background: #995b43; border-color: #995b43; }
        QPushButton#primaryButton:pressed, QPushButton[buttonRole="primary"]:pressed { background: #874e39; border-color: #874e39; }
        QPushButton#primaryButton:disabled, QPushButton[buttonRole="primary"]:disabled { color: #918a84; background: #e8e4e1; border-color: #d9d4cf; }
        QPushButton#verifyButton {
            min-height: 32px;
            padding: 0 11px;
            border-radius: 6px;
            background: #ffffff;
            color: #302f2d;
            border: 1px solid #cfc9c2;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton#verifyButton:hover { background: #f3f0ed; border-color: #a79e96; }
        QPushButton#verifyButton:disabled { color: #918a84; background: #eeece9; border-color: #ddd8d3; }
        QPushButton#ghostButton {
            min-height: 28px;
            padding: 0 10px;
            border-radius: 6px;
            background: #f6f4f1;
            border: 1px solid #ded9d3;
            color: #4f4b47;
            font-size: 12px;
        }
        QPushButton#ghostButton:hover { background: #ece9e4; color: #302f2d; border-color: #c7c1bb; }
        QPushButton#ghostButton:disabled { color: #918a84; background: #eeece9; border-color: #e2ddd7; }
        QPushButton#clearButton {
            min-height: 28px;
            padding: 0 10px;
            border-radius: 6px;
            background: #ffffff;
            color: #4f4b47;
            border: 1px solid #cfc9c2;
            font-size: 12px;
            font-weight: 500;
        }
        QPushButton#clearButton:hover { background: #f3f0ed; color: #302f2d; border-color: #a79e96; }
        QPushButton#clearButton:disabled { color: #918a84; background: #eeece9; border-color: #ddd8d3; }
        QPushButton#serialRecordButton, QPushButton#serialHighlightButton { min-height: 32px; padding: 0 10px; font-size: 12px; }
        QPushButton#serialRecordButton[recordState="recording"] { color: #854f39; border-color: #c8b2a6; }
        QPushButton#serialRecordButton[recordState="failed"] { color: #a34a42; background: #f7eeeb; border-color: #e8c9c6; }
        QPushButton#serialHighlightButton:checked { color: #754837; background: #eee0d7; border-color: #c9b0a1; }
        QPushButton#topGhostButton {
            min-height: 28px;
            padding: 0 10px;
            border-radius: 6px;
            background: #f6f4f1;
            border: 1px solid #ded9d3;
            color: #4f4b47;
            font-size: 12px;
        }
        QPushButton#topGhostButton:hover { background: #ece9e4; color: #302f2d; border-color: #c7c1bb; }
        QPushButton#topGhostButton:disabled { color: #918a84; background: #eeece9; border-color: #ddd8d3; }
        QPushButton#ghostButton, QPushButton#clearButton, QPushButton#topGhostButton {
            min-height: 32px;
        }
        QPushButton[buttonRole="tool"] { min-height: 30px; padding: 0 3px; font-size: 12px; }
        QCheckBox { color: #55504c; spacing: 6px; font-size: 13px; }
        QCheckBox:focus { color: #7b4634; }
        QCheckBox::indicator {
            width: 15px;
            height: 15px;
            border-radius: 4px;
            border: 1px solid #b8b0a8;
            background: #ffffff;
        }
        QCheckBox::indicator:hover { border-color: #8f867d; }
        QCheckBox::indicator:checked {
            background: #ad694e;
            border-color: #ad694e;
            image: url(%2);
        }
        QCheckBox::indicator:indeterminate {
            background: #a69b91;
            border-color: #a69b91;
            image: url(%3);
        }
        QCheckBox:disabled { color: #918a84; }
        QSplitter::handle { background: transparent; width: 8px; }
        QToolTip { background: #302f2d; color: #ffffff; border: 0; padding: 6px; }
    )").arg(chevronPath, checkPath, mixedPath);
}

} // namespace testtools::ui
