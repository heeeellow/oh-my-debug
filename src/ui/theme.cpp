#include "theme.h"

namespace testtools::ui {

QString applicationStyleSheet()
{
    return QStringLiteral(R"(
        QWidget {
            color: #20242a;
            font-family: "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI";
            font-size: 14px;
            font-weight: 400;
        }
        QMainWindow, QWidget#appSurface { background: #f6f7f9; }
        QFrame#topBar {
            background: #ffffff;
            border: 0;
            border-bottom: 1px solid #dce1e6;
        }
        QLabel#appTitle { color: #20242a; font-size: 18px; font-weight: 500; }
        QLabel#appSubtitle { color: #4f5964; font-size: 12px; }
        QLabel#topCaption { color: #4f5964; font-size: 12px; font-weight: 400; }
        QLabel#topStatusNeutral, QLabel#topStatusGood {
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
        }
        QLabel#topStatusNeutral {
            color: #4f5964;
            font-weight: 400;
        }
        QLabel#topStatusGood {
            color: #24705a;
            font-weight: 500;
        }
        QLabel#topStatusWarn {
            color: #805a18;
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#topStatusError {
            color: #ad3b32;
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
            font-weight: 500;
        }
        QFrame#navigationRail {
            background: #f1f3f5;
            border: 0;
            border-right: 1px solid #dce1e6;
        }
        QPushButton#navigationButton {
            min-height: 38px;
            padding: 0 12px;
            border: 0;
            border-left: 2px solid transparent;
            border-radius: 5px;
            background: transparent;
            color: #4f5964;
            text-align: left;
            font-weight: 400;
        }
        QPushButton#navigationButton:hover { background: #e7ebef; color: #20242a; }
        QPushButton#navigationButton:checked {
            background: #e7eff7;
            color: #285d8a;
            border-left-color: #356a9a;
            font-weight: 500;
        }
        QScrollArea#pageScroll { border: 0; background: transparent; }
        QWidget#pageSurface { background: transparent; }
        QLabel#pageTitle { color: #20242a; font-size: 22px; font-weight: 500; }
        QLabel#pageSubtitle { color: #4f5964; font-size: 14px; }
        QFrame#activityBar {
            background: #ffffff;
            border: 0;
            border-top: 1px solid #dce1e6;
        }
        QLabel#activityTitle { color: #20242a; font-weight: 500; }
        QLabel#activityMessage { color: #4f5964; }
        QFrame#contextPanel, QFrame#featureCard, QGroupBox#endpointCard {
            background: #ffffff;
            border: 1px solid #dce1e6;
            border-radius: 8px;
        }
        QFrame#taskModeBar {
            background: #ffffff;
            border: 1px solid #dce1e6;
            border-radius: 8px;
        }
        QPushButton#modeButton {
            min-height: 32px;
            padding: 0 14px;
            border: 0;
            border-radius: 6px;
            background: transparent;
            color: #4f5964;
        }
        QPushButton#modeButton:hover { background: #f1f3f5; color: #20242a; }
        QPushButton#modeButton:checked {
            background: #e7eff7;
            color: #285d8a;
            font-weight: 500;
        }
        QPushButton#modeButton:focus { border: 1px solid #356a9a; }
        QLabel#modeDescription { color: #626d78; font-size: 12px; }
        QFrame#summaryCard {
            background: transparent;
            border: 0;
        }
        QFrame#summaryDivider { background: #dce1e6; border: 0; }
        QFrame#linkBar {
            background: transparent;
            border: 0;
            border-bottom: 1px solid #dce1e6;
        }
        QLabel#cardEyebrow { color: #626d78; font-size: 12px; font-weight: 400; }
        QLabel#cardValue { color: #20242a; font-size: 18px; font-weight: 500; }
        QLabel#cardBody { color: #4f5964; }
        QLabel#routeSummary { color: #20242a; font-size: 14px; font-weight: 500; }
        QLabel#sectionLabel { color: #20242a; font-size: 16px; font-weight: 500; }
        QLabel#fieldLabel { color: #4f5964; font-size: 12px; font-weight: 400; }
        QLabel#mutedLabel {
            color: #626d78;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
        QLabel#endpointBadgeA, QLabel#endpointBadgeB {
            background: #e9edf1;
            color: #4f5964;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 500;
        }
        QLabel#endpointTitle { color: #20242a; font-size: 16px; font-weight: 500; }
        QLabel#connectionStatusIdle, QLabel#connectionStatusWorking,
        QLabel#connectionStatusGood, QLabel#connectionStatusError {
            background: transparent;
            padding: 3px 5px;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#connectionStatusIdle { color: #626d78; }
        QLabel#connectionStatusWorking { color: #356a9a; }
        QLabel#connectionStatusGood { color: #24705a; }
        QLabel#connectionStatusError { color: #ad3b32; }
        QLabel#testStatusNeutral, QLabel#testStatusWorking,
        QLabel#testStatusGood, QLabel#testStatusError {
            background: transparent;
            padding: 0;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#testStatusNeutral { color: #626d78; }
        QLabel#testStatusWorking { color: #805a18; }
        QLabel#testStatusGood { color: #24705a; }
        QLabel#testStatusError { color: #ad3b32; }
        QComboBox {
            min-height: 34px;
            padding: 0 9px;
            background: #ffffff;
            border: 1px solid #89939e;
            border-radius: 6px;
            selection-background-color: #dce9f5;
        }
        QComboBox:hover { border-color: #596572; }
        QComboBox:focus { border: 2px solid #356a9a; }
        QComboBox:disabled { background: #eef1f4; color: #626d78; border-color: #c9d0d7; }
        QFrame#topBar QComboBox {
            min-width: 280px;
            background: #ffffff;
            color: #20242a;
            border-color: #89939e;
        }
        QPlainTextEdit {
            border: 1px solid #89939e;
            border-radius: 6px;
            padding: 8px;
            background: #ffffff;
            selection-background-color: #dce9f5;
            placeholder-text-color: #626d78;
        }
        QPlainTextEdit:focus { border: 2px solid #356a9a; }
        QPlainTextEdit#receiveConsole {
            background: #f1f3f5;
            color: #20242a;
            border-color: #dce1e6;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 13px;
        }
        QPlainTextEdit#sendEditor { background: #ffffff; }
        QPushButton {
            min-height: 34px;
            padding: 0 12px;
            border: 1px solid #89939e;
            border-radius: 6px;
            background: #ffffff;
            color: #20242a;
            font-weight: 400;
        }
        QPushButton:hover { background: #f1f3f5; border-color: #596572; }
        QPushButton:pressed { background: #e7ebef; }
        QPushButton:focus { border: 2px solid #356a9a; }
        QPushButton:disabled { color: #626d78; background: #eef1f4; border-color: #c9d0d7; }
        QPushButton#primaryButton { background: #356a9a; color: #ffffff; border-color: #356a9a; font-weight: 500; }
        QPushButton#primaryButton:hover { background: #2e5f8a; border-color: #2e5f8a; }
        QPushButton#verifyButton { background: #ffffff; color: #285d8a; border-color: #89939e; font-weight: 500; }
        QPushButton#verifyButton:hover { background: #edf3f8; border-color: #596572; }
        QPushButton#ghostButton { background: transparent; border-color: transparent; color: #4f5964; padding: 0 7px; }
        QPushButton#ghostButton:hover { background: #e7ebef; color: #20242a; }
        QPushButton#topGhostButton {
            background: transparent;
            border-color: transparent;
            color: #4f5964;
        }
        QPushButton#topGhostButton:hover { background: #e7ebef; border-color: transparent; }
        QCheckBox { color: #4f5964; spacing: 6px; }
        QCheckBox:focus { color: #285d8a; }
        QCheckBox::indicator { width: 15px; height: 15px; }
        QSplitter::handle { background: transparent; width: 8px; }
        QToolTip { background: #20242a; color: #ffffff; border: 0; padding: 6px; }
    )");
}

} // namespace testtools::ui
