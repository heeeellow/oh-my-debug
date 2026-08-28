#include "theme.h"

namespace testtools::ui {

QString applicationStyleSheet()
{
    return QStringLiteral(R"(
        QWidget {
            color: #24272c;
            font-family: "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI";
            font-size: 13px;
            font-weight: 400;
        }
        QMainWindow, QWidget#appSurface { background: #f6f7f8; }
        QFrame#topBar {
            background: #fbfbfc;
            border: 0;
            border-bottom: 1px solid #e6e8eb;
        }
        QLabel#appTitle { color: #1f2328; font-size: 17px; font-weight: 500; }
        QLabel#appSubtitle { color: #666c75; font-size: 12px; }
        QLabel#topCaption { color: #686e77; font-size: 12px; font-weight: 400; }
        QLabel#topStatusNeutral, QLabel#topStatusGood {
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
        }
        QLabel#topStatusNeutral {
            color: #686e76;
            font-weight: 400;
        }
        QLabel#topStatusGood {
            color: #256b52;
            font-weight: 500;
        }
        QLabel#topStatusWarn {
            color: #7a5a18;
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#topStatusError {
            color: #a63a32;
            background: transparent;
            padding: 3px 4px;
            font-size: 12px;
            font-weight: 500;
        }
        QFrame#navigationRail {
            background: #f2f3f5;
            border: 0;
            border-right: 1px solid #e3e5e8;
        }
        QPushButton#navigationButton {
            min-height: 36px;
            padding: 0 12px;
            border: 0;
            border-left: 2px solid transparent;
            border-radius: 5px;
            background: transparent;
            color: #555b64;
            text-align: left;
            font-weight: 400;
        }
        QPushButton#navigationButton:hover { background: #e9ebee; color: #24272c; }
        QPushButton#navigationButton:checked {
            background: #e8eef5;
            color: #28577e;
            border-left-color: #4c769b;
            font-weight: 500;
        }
        QScrollArea#pageScroll { border: 0; background: transparent; }
        QWidget#pageSurface { background: transparent; }
        QLabel#pageTitle { color: #202328; font-size: 20px; font-weight: 500; }
        QLabel#pageSubtitle { color: #686e76; font-size: 13px; }
        QFrame#activityBar {
            background: #fbfbfc;
            border: 0;
            border-top: 1px solid #e5e7ea;
        }
        QLabel#activityTitle { color: #444a52; font-weight: 500; }
        QLabel#activityMessage { color: #686e76; }
        QFrame#contextPanel, QFrame#featureCard, QGroupBox#endpointCard {
            background: #fdfdfe;
            border: 1px solid #e5e7ea;
            border-radius: 8px;
        }
        QFrame#summaryCard {
            background: transparent;
            border: 0;
        }
        QFrame#summaryDivider { background: #eceef0; border: 0; }
        QFrame#linkBar {
            background: transparent;
            border: 0;
            border-bottom: 1px solid #e5e7ea;
        }
        QLabel#cardEyebrow { color: #686e76; font-size: 12px; font-weight: 400; }
        QLabel#cardValue { color: #24272c; font-size: 17px; font-weight: 500; }
        QLabel#cardBody { color: #686e76; }
        QLabel#routeSummary { color: #2c3036; font-size: 14px; font-weight: 500; }
        QLabel#sectionLabel { color: #34383e; font-size: 13px; font-weight: 500; }
        QLabel#fieldLabel { color: #666c75; font-size: 12px; font-weight: 400; }
        QLabel#mutedLabel {
            color: #68707a;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 11px;
        }
        QLabel#endpointBadgeA, QLabel#endpointBadgeB {
            background: #eef0f2;
            color: #555c65;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 500;
        }
        QLabel#endpointTitle { color: #282c31; font-size: 15px; font-weight: 500; }
        QLabel#connectionStatusIdle, QLabel#connectionStatusWorking,
        QLabel#connectionStatusGood, QLabel#connectionStatusError {
            background: transparent;
            padding: 3px 5px;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#connectionStatusIdle { color: #68707a; }
        QLabel#connectionStatusWorking { color: #3b6589; }
        QLabel#connectionStatusGood { color: #256b52; }
        QLabel#connectionStatusError { color: #a63a32; }
        QLabel#testStatusNeutral, QLabel#testStatusWorking,
        QLabel#testStatusGood, QLabel#testStatusError {
            background: transparent;
            padding: 0;
            font-size: 12px;
            font-weight: 400;
        }
        QLabel#testStatusNeutral { color: #68707a; }
        QLabel#testStatusWorking { color: #7a5a18; }
        QLabel#testStatusGood { color: #256b52; }
        QLabel#testStatusError { color: #a63a32; }
        QComboBox {
            min-height: 30px;
            padding: 0 9px;
            background: #fdfdfe;
            border: 1px solid #d7dade;
            border-radius: 5px;
            selection-background-color: #dfe9f3;
        }
        QComboBox:hover { border-color: #b2b7be; }
        QComboBox:focus { border-color: #7293b0; }
        QComboBox:disabled { background: #f1f2f3; color: #9a9fa6; }
        QFrame#topBar QComboBox {
            min-width: 280px;
            background: #ffffff;
            color: #2c3036;
            border-color: #d5d8dc;
        }
        QPlainTextEdit {
            border: 1px solid #dfe2e5;
            border-radius: 6px;
            padding: 8px;
            background: #fdfdfe;
            selection-background-color: #cdddec;
        }
        QPlainTextEdit#receiveConsole {
            background: #f3f5f6;
            color: #33453d;
            border-color: #e0e3e6;
            font-family: "Cascadia Mono", Consolas, "Microsoft YaHei UI";
            font-size: 12px;
        }
        QPlainTextEdit#sendEditor { background: #fafbfc; }
        QPushButton {
            min-height: 30px;
            padding: 0 12px;
            border: 1px solid #d5d8dc;
            border-radius: 5px;
            background: #fdfdfe;
            color: #3c4148;
            font-weight: 400;
        }
        QPushButton:hover { background: #f4f5f6; border-color: #b7bbc1; }
        QPushButton:pressed { background: #eceef0; }
        QPushButton:disabled { color: #a3a8af; background: #f1f2f3; border-color: #e2e4e7; }
        QPushButton#primaryButton { background: #3f709d; color: #ffffff; border-color: #3f709d; font-weight: 500; }
        QPushButton#primaryButton:hover { background: #35668f; border-color: #35668f; }
        QPushButton#verifyButton { background: #edf2f6; color: #315f86; border-color: #d6e0e9; font-weight: 500; }
        QPushButton#verifyButton:hover { background: #e4ebf1; border-color: #c3d1de; }
        QPushButton#ghostButton { background: transparent; border-color: transparent; color: #68707a; padding: 0 7px; }
        QPushButton#ghostButton:hover { background: #eff1f3; color: #34383e; }
        QPushButton#topGhostButton {
            background: transparent;
            border-color: transparent;
            color: #4f5660;
        }
        QPushButton#topGhostButton:hover { background: #eff1f3; border-color: transparent; }
        QCheckBox { color: #555c65; spacing: 6px; }
        QCheckBox::indicator { width: 15px; height: 15px; }
        QSplitter::handle { background: transparent; width: 8px; }
        QToolTip { background: #34383e; color: #ffffff; border: 0; padding: 5px; }
    )");
}

} // namespace testtools::ui
