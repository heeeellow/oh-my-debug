#include "serial_workspace.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets/serial_receive_view.h"
#include "ui/widgets/serial_connection_bar.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/serial_keyword_highlighter.h"

#include "application/serial/serial_transport_service.h"
#include "core/serial/codec.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QClipboard>
#include <QtGui/QPainter>
#include <QtGui/QActionGroup>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtGui/QTextCharFormat>
#include <QtGui/QShortcut>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace serialdemo;

namespace {

using testtools::ui::fromUtf8;
using testtools::ui::toUtf8;

QString channelName(int index)
{
    return index == 0 ? QStringLiteral("端点 A") : QStringLiteral("端点 B");
}

QString selectedEndpoint(const QComboBox* combo)
{
    // 已手动编辑的端点文本不得继承旧选项隐藏的绑定数据。
    const auto text = combo->currentText().trimmed();
    const auto data = combo->currentData().toString();
    return !data.isEmpty() && combo->currentText() == combo->itemText(combo->currentIndex())
        ? data : text;
}

LineEnding lineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return LineEnding::Lf;
    case 2:
        return LineEnding::Cr;
    case 3:
        return LineEnding::CrLf;
    default:
        return LineEnding::None;
    }
}

testtools::AtCommandLineEnding storedLineEndingFromIndex(int index)
{
    switch (index) {
    case 1:
        return testtools::AtCommandLineEnding::Lf;
    case 2:
        return testtools::AtCommandLineEnding::Cr;
    case 3:
        return testtools::AtCommandLineEnding::CrLf;
    default:
        return testtools::AtCommandLineEnding::None;
    }
}

int storedLineEndingIndex(testtools::AtCommandLineEnding ending)
{
    switch (ending) {
    case testtools::AtCommandLineEnding::Lf:
        return 1;
    case testtools::AtCommandLineEnding::Cr:
        return 2;
    case testtools::AtCommandLineEnding::CrLf:
        return 3;
    default:
        return 0;
    }
}

void setVisualRole(QLabel *label, const QString &objectName)
{
    label->setObjectName(objectName);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

QTextCharFormat logEntryFormat(testtools::SerialLogDirection direction)
{
    QTextCharFormat format;
    switch (direction) {
    case testtools::SerialLogDirection::Rx:
        format.setForeground(QColor(QStringLiteral("#286878")));
        break;
    case testtools::SerialLogDirection::Tx:
        format.setForeground(QColor(QStringLiteral("#965636")));
        break;
    case testtools::SerialLogDirection::Notice:
        format.setForeground(QColor(QStringLiteral("#706963")));
        break;
    }
    return format;
}

QString recordingSize(std::uint64_t bytes)
{
    if (bytes >= 1024 * 1024) return QStringLiteral("%1 MiB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1);
    if (bytes >= 1024) return QStringLiteral("%1 KiB").arg(double(bytes) / 1024, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

} // namespace

SerialWorkspace::SerialWorkspace(QWidget* parent, Purpose purpose,
                                 std::shared_ptr<testtools::SerialPortClaims> claims)
    : QWidget(parent), purpose_(purpose),
      portClaims_(claims ? std::move(claims) : std::make_shared<testtools::SerialPortClaims>())
{
    const auto configuredStorage = qEnvironmentVariable("TESTTOOLS_DATA_DIR");
    const auto storageRoot = configuredStorage.isEmpty()
                                 ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                 : configuredStorage;
    atCommandStorePath_ =
        std::filesystem::path(storageRoot.toStdWString()) / L"at-commands.json";
    std::error_code existsError;
    const bool commandFileExists = std::filesystem::exists(atCommandStorePath_, existsError);
    std::string commandStoreError;
    const bool loaded = !existsError &&
                        testtools::AtCommandStore::load(
                            atCommandStorePath_, atCommandLibrary_, commandStoreError);
    if (loaded && !commandFileExists)
        testtools::AtCommandStore::save(
            atCommandStorePath_, atCommandLibrary_, commandStoreError);

    const auto dataRoot = std::filesystem::path(storageRoot.toStdWString());
    preferencesStorePath_ = dataRoot /
        (purpose_ == Purpose::Terminal ? L"terminal-preferences.json" : L"serial-preferences.json");
    std::error_code prefExistsError;
    const bool prefFileExists = std::filesystem::exists(preferencesStorePath_, prefExistsError);
    std::string prefStoreError;
    preferencesLoaded_ = !prefExistsError && prefFileExists &&
                         testtools::SerialPreferencesStore::load(
                             preferencesStorePath_, preferences_, prefStoreError);
    if (purpose_ == Purpose::Terminal && !prefFileExists) {
        // 首次进入终端模式时继承端点 B 的上次配置，之后将终端偏好独立持久化。
        preferencesLoaded_ = testtools::SerialPreferencesStore::load(
            dataRoot / L"serial-preferences.json", preferences_, prefStoreError);
    }
    if (!preferencesLoaded_) {
        preferences_.channels[1].lineEnding =
            storedLineEndingIndex(atCommandLibrary_.lineEnding());
    }

    buildUi();
    applyPreferencesToUi();
    if (purpose_ == Purpose::Terminal) {
        workspaceState_.applyMode(WorkspaceMode::Terminal, {});
        applyWorkspaceMode();
    }
    refreshAtCommandChoices();
    for (int index = 0; index < static_cast<int>(channels_.size()); ++index) {
        attachSessionCallbacks(index);
    }
    recordingTimer_ = new QTimer(this);
    recordingTimer_->setInterval(200);
    connect(recordingTimer_, &QTimer::timeout, this, &SerialWorkspace::pollRecordings);
    recordingTimer_->start();
    pollRecordings();
    refreshTargets();
    if (purpose_ == Purpose::Terminal && !prefFileExists)
        savePreferences();
    if (!commandStoreError.empty()) {
        const QString message = QStringLiteral("AT \u6307\u4ee4\u5e93\u672a\u80fd\u4ece\u672c\u5730\u6587\u4ef6\u6062\u590d\uff1a%1")
                                    .arg(fromUtf8(commandStoreError));
        QTimer::singleShot(0, this, [this, message] { emit activityChanged(message); });
    }
}

SerialWorkspace::~SerialWorkspace()
{
    prepareForShutdown();
}

QString SerialWorkspace::currentWorkspaceMode() const
{
    return workspaceState_.mode() == WorkspaceMode::Dual ? QStringLiteral("dual") : workspaceState_.mode() == WorkspaceMode::Terminal ? QStringLiteral("terminal") : QStringLiteral("single");
}
bool SerialWorkspace::setWorkspaceModeForDiagnostics(const QString& modeName) { return selectWorkspaceMode(modeName); }

bool SerialWorkspace::selectWorkspaceMode(const QString &modeName)
{
    const QString normalized = modeName.trimmed().toLower();
    if (purpose_ == Purpose::Terminal && normalized != QStringLiteral("terminal"))
        return false;
    WorkspaceMode mode = WorkspaceMode::Single;
    if (normalized == QStringLiteral("dual")) {
        mode = WorkspaceMode::Dual;
    } else if (normalized == QStringLiteral("terminal")) {
        mode = WorkspaceMode::Terminal;
    } else if (normalized != QStringLiteral("single")) {
        return false;
    }
    setWorkspaceMode(mode);
    return workspaceState_.mode() == mode;
}

void SerialWorkspace::prepareForShutdown()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (recordingTimer_) recordingTimer_->stop();
    stopRecordings();
    if (scheduleTimer_) {
        scheduleTimer_->stop();
    }
    if (stressTestRunning_) {
        stopStressTest(QStringLiteral("应用正在退出"));
    }
    controlTasks_.wait();
    disconnectAll();
    scanTasks_.wait();
    QCoreApplication::removePostedEvents(this);
}

void SerialWorkspace::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(8);

    auto *modeBar = new QFrame(this);
    modeBar->setObjectName(QStringLiteral("taskModeBar"));
    auto *modeLayout = new QHBoxLayout(modeBar);
    modeLayout->setContentsMargins(2, 0, 2, 2);
    modeLayout->setSpacing(12);
    pageTitle_ = new QLabel(QStringLiteral("单串口调试"), modeBar);
    pageTitle_->setObjectName(QStringLiteral("pageTitle"));
    modeLayout->addWidget(pageTitle_);
    modeDescriptionLabel_ = new QLabel(modeBar);
    modeDescriptionLabel_->setObjectName(QStringLiteral("modeDescription"));
    modeDescriptionLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    modeLayout->addStretch();
    modeLayout->addWidget(modeDescriptionLabel_);
    rootLayout->addWidget(modeBar);

    linkBar_ = new QFrame(this);
    linkBar_->setObjectName(QStringLiteral("linkBar"));
    auto *linkLayout = new QHBoxLayout(linkBar_);
    linkLayout->setContentsMargins(10, 4, 10, 5);
    linkLayout->setSpacing(6);
    routeLabel_ = new QLabel(QStringLiteral("A  未选择   ⇄   B  未选择"), linkBar_);
    routeLabel_->setObjectName(QStringLiteral("routeSummary"));
    routeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    routeLabel_->setWordWrap(true);
    linkLayout->addWidget(routeLabel_, 1);

    connectBothButton_ = new QPushButton(QStringLiteral("连接两端"), linkBar_);
    connectBothButton_->setObjectName(QStringLiteral("primaryButton"));
    disconnectBothButton_ = new QPushButton(QStringLiteral("断开两端"), linkBar_);
    bidirectionalTestButton_ = new QPushButton(QStringLiteral("双向收发验证"), linkBar_);
    bidirectionalTestButton_->setObjectName(QStringLiteral("verifyButton"));
    bidirectionalTestButton_->setToolTip(
        QStringLiteral("需要先连接端点 A 和 B；会向两端各发送一条临时测试帧"));
    stressTestButton_ = new QPushButton(QStringLiteral("可靠性压测…"), linkBar_);
    stressTestButton_->setObjectName(QStringLiteral("verifyButton"));
    stressTestButton_->setToolTip(
        QStringLiteral("需要先连接端点 A 和 B；打开参数面板后才会开始发送测试帧"));
    linkLayout->addWidget(connectBothButton_);
    linkLayout->addWidget(disconnectBothButton_);
    linkLayout->addWidget(bidirectionalTestButton_);
    linkLayout->addWidget(stressTestButton_);
    rootLayout->addWidget(linkBar_);

    channelSplitter_ = new QSplitter(Qt::Horizontal, this);
    channelSplitter_->setChildrenCollapsible(false);
    channelSplitter_->addWidget(buildChannelPanel(0));
    channelSplitter_->addWidget(buildChannelPanel(1));
    channelSplitter_->setStretchFactor(0, 1);
    channelSplitter_->setStretchFactor(1, 1);
    rootLayout->addWidget(channelSplitter_, 1);

    // 验证与压测结果独占底部区域，避免长摘要挤压上方操作栏。
    resultsPanel_ = new QFrame(this);
    resultsPanel_->setObjectName(QStringLiteral("serialResults"));
    auto *resultsLayout = new QVBoxLayout(resultsPanel_);
    resultsLayout->setContentsMargins(12, 7, 12, 7);
    resultsLayout->setSpacing(4);
    testResultLabel_ = new QLabel(QStringLiteral("双向收发验证 · 未运行"), resultsPanel_);
    stressResultLabel_ = new QLabel(QStringLiteral("可靠性压测 · 未运行"), resultsPanel_);
    for (auto *label : {testResultLabel_, stressResultLabel_}) {
        label->setObjectName(QStringLiteral("testStatusNeutral"));
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        resultsLayout->addWidget(label);
    }
    stressResultLabel_->setToolTip(
        QStringLiteral("BER 统计已成功解析帧的有效载荷；丢帧和格式错误单独显示"));
    rootLayout->addWidget(resultsPanel_);

    connect(connectBothButton_, &QPushButton::clicked, this, [this] {
        QString error;
        for (int index = 0; index < 2; ++index) {
            if (!channelActive(index) && !validateChannel(index, error)) {
                QMessageBox::warning(this, QStringLiteral("无法连接两路"), error);
                return;
            }
        }
        if (!channelActive(0) && !channelActive(1)) {
            auto first = endpointSelection(0);
            auto second = endpointSelection(1);
            first.active = true;
            second.active = true;
            const auto conflict = testtools::SerialWorkspaceState::endpointConflict(first, second);
            if (conflict == testtools::SerialEndpointConflict::AgentSingleSession) {
                QMessageBox::information(this,
                                         QStringLiteral("当前为单板串口会话"),
                                         QStringLiteral("当前设备连接一次只能使用一个设备串口。可同"
                                                        "时使用一路 PC端串口和一路设备串口。"));
                return;
            }
            if (conflict == testtools::SerialEndpointConflict::SameResource) {
                QMessageBox::warning(this,
                                     QStringLiteral("无法连接两路"),
                                     QStringLiteral("端点 A 和 B 不能同时打开同一个串口。"));
                return;
            }
        }
        for (int index = 0; index < 2; ++index) {
            if (!channelActive(index)) {
                connectChannel(index);
            }
        }
    });
    connect(disconnectBothButton_, &QPushButton::clicked, this, &SerialWorkspace::disconnectAll);
    connect(bidirectionalTestButton_,
            &QPushButton::clicked,
            this,
            &SerialWorkspace::runBidirectionalTest);
    connect(stressTestButton_, &QPushButton::clicked, this, [this] {
        if (stressTestRunning_) {
            stopStressTest(QStringLiteral("用户已停止可靠性压测"));
        } else {
            showStressDialog();
        }
    });

    scheduleTimer_ = new QTimer(this);
    scheduleTimer_->setInterval(10);
    connect(scheduleTimer_, &QTimer::timeout, this, &SerialWorkspace::processScheduledSends);
    scheduleTimer_->start();

    const auto searchChannel = [this] {
        auto *focus = QApplication::focusWidget();
        return channels_[0].group->isVisible() && focus &&
                       channels_[0].group->isAncestorOf(focus) ? 0 : 1;
    };
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this, searchChannel] {
        auto &channel = channels_[searchChannel()];
        channel.searchButton->setChecked(true);
        channel.searchEdit->setFocus();
        channel.searchEdit->selectAll();
    });
    auto* terminalFindShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    terminalFindShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(terminalFindShortcut, &QShortcut::activated, this, [this] {
        if (workspaceState_.mode() != WorkspaceMode::Terminal) return;
        channels_[1].searchButton->setChecked(true);
        channels_[1].searchEdit->setFocus();
        channels_[1].searchEdit->selectAll();
    });
    for (int step : {-1, 1}) {
        auto *nextShortcut = new QShortcut(QKeySequence(step > 0 ? "F3" : "Shift+F3"), this);
        nextShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(nextShortcut, &QShortcut::activated, this, [this, searchChannel, step] {
            findLogMatch(searchChannel(), step);
        });
    }

    updateRouteSummary();
    applyWorkspaceMode();
    updateGlobalControls();
}

void SerialWorkspace::setWorkspaceMode(WorkspaceMode mode)
{
    if (mode == workspaceState_.mode()) {
        applyWorkspaceMode();
        return;
    }
    if (!canSwitchWorkspaceMode(mode)) {
        return;
    }
    testtools::SerialWorkspaceActivity activity;
    workspaceState_.applyMode(mode, activity);
    applyWorkspaceMode();
    updateGlobalControls();
}

bool SerialWorkspace::canSwitchWorkspaceMode(WorkspaceMode mode)
{
    testtools::SerialWorkspaceActivity activity;
    for (int index = 0; index < 2; ++index) {
        activity.channelActive[static_cast<std::size_t>(index)] = channelActive(index);
        activity.scheduledSendRunning[static_cast<std::size_t>(index)] =
            channels_[index].session.snapshot().scheduledSend.running;
    }
    activity.bidirectionalTestRunning = bidirectionalTestRunning_;
    activity.reliabilityTestRunning = stressTestRunning_;
    const auto decision = workspaceState_.evaluateModeSwitch(mode, activity);
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::TaskRunning) {
        QMessageBox::information(
            this,
            QStringLiteral("任务正在运行"),
            QStringLiteral("请先停止当前的计划发送、双向收发验证或可靠性压测，再切换调试方式。"));
        return false;
    }
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        QMessageBox::information(
            this, QStringLiteral("端点 A 仍在使用"),
            QStringLiteral("请先断开端点 A，再切换到单串口调试或串口终端。"));
        return false;
    }
    return true;
}

void SerialWorkspace::applyWorkspaceMode()
{
    if (!channels_[0].group || !channels_[1].group || !linkBar_) {
        return;
    }

    const bool terminal = workspaceState_.mode() == WorkspaceMode::Terminal;
    const bool single = workspaceState_.mode() != WorkspaceMode::Dual;
    pageTitle_->setText(terminal ? QStringLiteral("串口终端") : single ? QStringLiteral("单串口调试") : QStringLiteral("双端调试"));
    emit workspaceModeChanged(currentWorkspaceMode());

    channels_[0].group->setVisible(!single);
    channels_[1].group->setVisible(true);
    channels_[0].manualSendArea->setVisible(true);
    channels_[1].manualSendArea->setVisible(true);
    channels_[1].composerFrame->setVisible(!terminal);
    terminalOptionsButton_->setVisible(terminal);
    channels_[0].toggleAtBarButton->hide();
    channels_[1].manualSendArea->setVisible(!terminal);
    channels_[0].sendShortcutHint->setVisible(single);
    channels_[1].sendShortcutHint->setVisible(single);
    for (auto &channel : channels_) {
        channel.group->setProperty("singleChannel", single);
        channel.group->layout()->setContentsMargins(single ? 0 : 14, single ? 0 : 10,
                                                   single ? 0 : 14, single ? 0 : 12);
        channel.group->style()->unpolish(channel.group);
        channel.group->style()->polish(channel.group);
        channel.sendEdit->setFixedHeight(single ? 56 : 44);
        channel.sourceCombo->setItemText(0, QStringLiteral("PC端串口"));
        channel.sourceCombo->setMinimumWidth(single ? 148 : 100);
        channel.sourceCombo->setMaximumWidth(single ? 176 : 116);
        channel.portCombo->setMinimumContentsLength(single ? 12 : 8);
        channel.portCombo->setMinimumWidth(single ? 120 : 80);
        channel.portCombo->setSizePolicy(single ? QSizePolicy::Expanding : QSizePolicy::Ignored,
                                         QSizePolicy::Fixed);
        channel.baudCombo->setMinimumWidth(118);
        channel.baudCombo->setMaximumWidth(132);
        // 双口面板宽度较窄时，将字节计数与截断提示移至独立紧凑工具行。
        auto *counterLayout = single ? channel.receiveHeaderLayout : channel.counterRowLayout;
        auto *previousLayout = single ? channel.counterRowLayout : channel.receiveHeaderLayout;
        previousLayout->removeWidget(channel.counterLabel);
        if (counterLayout->indexOf(channel.counterLabel) < 0) {
            counterLayout->insertWidget(single ? counterLayout->indexOf(channel.searchButton) : 0,
                                         channel.counterLabel);
        }
        previousLayout->removeWidget(channel.truncatedLabel);
        if (counterLayout->indexOf(channel.truncatedLabel) < 0) {
            counterLayout->insertWidget(1, channel.truncatedLabel);
        }
        channel.counterRow->setVisible(!single);
        channel.counterLabel->show();
        updateTruncation(int(&channel - channels_.data()));
        channel.receiveHexCheck->setText(single ? QStringLiteral("HEX 显示")
                                                : QStringLiteral("HEX"));
        channel.autoScrollCheck->setText(single ? QStringLiteral("自动滚动")
                                                : QStringLiteral("滚动"));
        channel.resetCounterButton->setText(single ? QStringLiteral("计数清零")
                                                   : QStringLiteral("清零"));
        channel.saveLogButton->setText(single ? QStringLiteral("保存日志…")
                                              : QStringLiteral("保存…"));
    }
    linkBar_->setVisible(!single);
    resultsPanel_->setVisible(!single);
    testResultLabel_->setVisible(!single);
    bidirectionalTestButton_->setVisible(!single);
    stressResultLabel_->setVisible(!single);
    stressTestButton_->setVisible(!single);
    channels_[1].receiveTitle->setText(terminal ? QStringLiteral("终端") : QStringLiteral("收发记录"));
    channels_[1].clearReceiveButton->setText(terminal ? QStringLiteral("清屏") : QStringLiteral("清空"));
    if (!terminal) {
        const QSignalBlocker blocker(channels_[1].autoScrollCheck);
        channels_[1].autoScrollCheck->setChecked(preferences_.channels[1].autoScroll);
    }
    channels_[1].receiveHexCheck->setVisible(!terminal);
    channels_[1].timestampCheck->setVisible(!terminal);
    channels_[1].autoScrollCheck->setVisible(!terminal);
    channels_[1].counterLabel->setVisible(!terminal);
    channels_[1].resetCounterButton->setVisible(!terminal);
    channels_[1].filterCheck->setVisible(!terminal);
    channels_[1].searchButton->setToolTip(terminal ? QStringLiteral("搜索终端历史（Ctrl+Shift+F）")
                                                : QStringLiteral("搜索与过滤当前日志（Ctrl+F）"));
    channels_[1].receiveEdit->setLineWrapMode(terminal ? QPlainTextEdit::NoWrap : QPlainTextEdit::WidgetWidth);
    channels_[1].receiveEdit->setAttribute(Qt::WA_InputMethodEnabled, terminal);
    channels_[1].receiveEdit->setProperty("terminalInput", terminal);
    channels_[1].receiveEdit->setPlaceholderText(terminal
        ? QStringLiteral("连接后直接输入 · Enter 执行 · ↑↓ 历史 · Tab 补全 · Ctrl+C 中断")
        : QStringLiteral("等待数据；发送成功显示 TX，接收数据显示 RX"));
    channels_[1].clearReceiveButton->setToolTip(terminal
        ? QStringLiteral("清空终端视图，保留字节计数与普通收发记录，不发送数据")
        : QStringLiteral("清空当前收发记录，保留 RX / TX 字节计数"));

    if (single) {
        channels_[1].headerLayout->removeWidget(channels_[1].actionsContainer);
        if (channels_[1].settingsRowLayout->indexOf(channels_[1].actionsContainer) < 0) {
            channels_[1].settingsRowLayout->addWidget(channels_[1].actionsContainer);
        }
        channels_[1].headerWidget->hide();
        channels_[1].refreshPortsButton->setText(QStringLiteral("刷新串口"));
        channels_[1].portCombo->setMaximumWidth(320);
        modeDescriptionLabel_->setText(terminal
            ? QStringLiteral("直接输入 · ↑↓ 历史 · Tab 补全 · Ctrl+C 中断")
            : QStringLiteral("连接一个串口，查看 TX / RX 并发送数据"));
    } else {
        channelSplitter_->setSizes({600, 600});
        for (int index = 0; index < 2; ++index) {
            channels_[index].settingsRowLayout->removeWidget(channels_[index].actionsContainer);
            if (channels_[index].headerLayout->indexOf(channels_[index].actionsContainer) < 0) {
                channels_[index].headerLayout->addWidget(channels_[index].actionsContainer);
            }
            channels_[index].headerWidget->show();
            channels_[index].badgeLabel->show();
            channels_[index].titleLabel->setText(channelName(index));
            channels_[index].refreshPortsButton->setText(QStringLiteral("刷新"));
            channels_[index].portCombo->setMaximumWidth(250);
        }
        modeDescriptionLabel_->setText(
            QStringLiteral("配置端点 A / B；独立收发、双向验证和可靠性压测按需使用"));
    }
    updateCounter(0);
    updateCounter(1);
    renderReceiveLog(1);
    updateShortcutLayout();
    if (terminal) { resumeTerminalInput(); focusTerminal(); }
}

void SerialWorkspace::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width()) forceShortcutOpen_ = false;
    QTimer::singleShot(0, this, [this] { updateShortcutLayout(); });
}

void SerialWorkspace::updateShortcutLayout()
{
    if (updatingShortcutLayout_ || !channels_[1].receiveMoreButton) return;
    updatingShortcutLayout_ = true;
    const bool single = workspaceState_.mode() == WorkspaceMode::Single;
    if (shortcutSidebar_ && shortcutSplitter_) {
        const int available = shortcutSplitter_->width();
        const int savedWidth = std::clamp(preferences_.shortcutSidebarWidth, 220, 480);
        // 自动适配只改变当前可见性，不覆盖用户的展开偏好或保存宽度。
        const bool enoughRoom = available >= 850 + savedWidth + 8;
        const bool visible = single && preferences_.shortcutBarVisible &&
                             (enoughRoom || (forceShortcutOpen_ && available >= 860));
        const bool wasHidden = shortcutSidebar_->isHidden();
        shortcutSidebar_->setVisible(visible);
        if (visible && wasHidden) {
            const int sideWidth = std::min(savedWidth, std::max(220, available - 680));
            shortcutSplitter_->setSizes({std::max(1, available - sideWidth - 8), sideWidth});
        }
        channels_[1].toggleAtBarButton->setVisible(single && !visible);
    } else channels_[1].toggleAtBarButton->hide();
    channels_[0].toggleAtBarButton->hide();
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[index];
        if (!channel.receiveMoreButton) continue;
        const bool terminal = index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal;
        const bool compact = channel.logSendBody->width() < 1060;
        channel.receiveHexCheck->setVisible(!terminal && !compact);
        channel.timestampCheck->setVisible(!terminal && !compact);
        channel.autoScrollCheck->setVisible(!terminal && !compact);
        channel.receiveMoreButton->setVisible(!terminal);
        channel.resetCounterButton->hide();
        channel.saveLogButton->setVisible(terminal);
        for (auto* action : channel.displayActions) if (action) action->setVisible(compact);
        channel.sendShortcutHint->setVisible(single && channel.logSendBody->width() >= 760);
    }
    updatingShortcutLayout_ = false;
}

QWidget *SerialWorkspace::buildChannelPanel(int index)
{
    auto &channel = channels_[index];
    channel.group = new QGroupBox(this);
    channel.group->setObjectName(QStringLiteral("endpointCard"));
    auto *layout = new QVBoxLayout(channel.group);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);

    channel.headerWidget = new QWidget(channel.group);
    channel.headerLayout = new QHBoxLayout(channel.headerWidget);
    channel.headerLayout->setContentsMargins(0, 0, 0, 0);
    channel.headerLayout->setSpacing(6);
    channel.badgeLabel = new QLabel(
        index == 0 ? QStringLiteral("A") : QStringLiteral("B"), channel.headerWidget);
    channel.badgeLabel->setAlignment(Qt::AlignCenter);
    channel.badgeLabel->setFixedSize(22, 22);
    channel.badgeLabel->setObjectName(index == 0 ? QStringLiteral("endpointBadgeA")
                                                 : QStringLiteral("endpointBadgeB"));
    channel.titleLabel = new QLabel(channelName(index), channel.headerWidget);
    channel.titleLabel->setObjectName(QStringLiteral("endpointTitle"));
    channel.headerLayout->addWidget(channel.badgeLabel);
    channel.headerLayout->addWidget(channel.titleLabel);
    channel.headerLayout->addStretch();

    channel.actionsContainer = new QWidget(channel.group);
    auto *actionsLayout = new QHBoxLayout(channel.actionsContainer);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(6);
    channel.refreshPortsButton =
        new QPushButton(QStringLiteral("刷新串口"), channel.actionsContainer);
    channel.refreshPortsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.refreshPortsButton->setToolTip(QStringLiteral("重新扫描当前所选位置的串口"));
    channel.advancedSettingsButton =
        new QPushButton(QStringLiteral("更多参数"), channel.actionsContainer);
    channel.advancedSettingsButton->setObjectName(QStringLiteral("ghostButton"));
    channel.advancedSettingsButton->setCheckable(true);
    channel.statusLabel = new QLabel(QStringLiteral("● 未连接"), channel.actionsContainer);
    channel.statusLabel->setObjectName(QStringLiteral("connectionStatusIdle"));
    channel.statusLabel->setAlignment(Qt::AlignCenter);
    channel.connectButton = new QPushButton(QStringLiteral("连接"), channel.actionsContainer);
    channel.connectButton->setMinimumWidth(78);
    channel.connectButton->setCursor(Qt::PointingHandCursor);
    channel.serviceHint = new QFrame(channel.actionsContainer);
    channel.serviceHint->setObjectName(QStringLiteral("serialServiceHint"));
    auto* hintLayout = new QHBoxLayout(channel.serviceHint);
    hintLayout->setContentsMargins(0, 0, 0, 0);
    hintLayout->setSpacing(6);
    channel.serviceHintLabel = new QLabel(QStringLiteral("需连接板端服务"), channel.serviceHint);
    channel.serviceHintLabel->setObjectName(QStringLiteral("serialServiceMessage"));
    channel.serviceConnectButton = new QPushButton(QStringLiteral("连接服务"), channel.serviceHint);
    channel.serviceConnectButton->setObjectName(QStringLiteral("serialServiceConnect"));
    hintLayout->addWidget(channel.serviceHintLabel);
    hintLayout->addWidget(channel.serviceConnectButton);
    channel.serviceHint->hide();
    connect(channel.serviceConnectButton, &QPushButton::clicked, this, &SerialWorkspace::prepareServiceRequested);
    channel.refreshPortsButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Refresh));
    channel.refreshPortsButton->setIconSize({20, 20});
    actionsLayout->addWidget(channel.refreshPortsButton);
    actionsLayout->addWidget(channel.advancedSettingsButton);
    actionsLayout->addWidget(channel.serviceHint);
    actionsLayout->addWidget(channel.statusLabel);
    actionsLayout->addWidget(channel.connectButton);

    channel.headerLayout->addWidget(channel.actionsContainer);
    layout->addWidget(channel.headerWidget);

    auto* connection = new SerialConnectionBar(channel.group);
    channel.sourceCombo = connection->sourceCombo;
    channel.portCombo = connection->portCombo;
    channel.baudCombo = connection->baudCombo;
    channel.dataBitsCombo = connection->dataBitsCombo;
    channel.parityCombo = connection->parityCombo;
    channel.stopBitsCombo = connection->stopBitsCombo;
    channel.flowControlCombo = connection->flowControlCombo;
    channel.dtrCheck = connection->dtrCheck;
    channel.rtsCheck = connection->rtsCheck;
    channel.settingsRowLayout = connection->settingsRowLayout;
    channel.advancedSettingsArea = connection->advancedSettingsArea;
    layout->addWidget(connection);
    channel.dtrCheck->setProperty("diagnosticObjectName", QStringLiteral("dtr%1").arg(index));
    channel.rtsCheck->setProperty("diagnosticObjectName", QStringLiteral("rts%1").arg(index));
    connect(channel.dtrCheck, &QCheckBox::clicked, this, [this, index](bool checked) {
        setControlLine(index, SerialControlLine::Dtr, checked);
    });
    connect(channel.rtsCheck, &QCheckBox::clicked, this, [this, index](bool checked) {
        setControlLine(index, SerialControlLine::Rts, checked);
    });

    channel.logSendBody = new QWidget(channel.group);
    channel.logSendBody->installEventFilter(this);
    auto* logSendLayout = new QVBoxLayout(channel.logSendBody);
    logSendLayout->setContentsMargins(0, 0, 0, 0);
    logSendLayout->setSpacing(8);
    if (index == 1 && purpose_ == Purpose::Debug) {
        shortcutSplitter_ = new QSplitter(Qt::Horizontal, channel.group);
        shortcutSplitter_->setHandleWidth(8);
        shortcutSplitter_->setChildrenCollapsible(false);
        shortcutSplitter_->addWidget(channel.logSendBody);
        shortcutSidebar_ = new SerialShortcutSidebar(shortcutSplitter_);
        shortcutSplitter_->addWidget(shortcutSidebar_);
        shortcutSplitter_->setStretchFactor(0, 1);
        shortcutSplitter_->setStretchFactor(1, 0);
        shortcutSidebar_->fillRequested = [this](int row) { fillAtCommandToInput(1, row); };
        shortcutSidebar_->sendRequested = [this](int row) { sendAtCommand(1, row); };
        shortcutSidebar_->manageRequested = [this] { showAtCommandManager(); };
        shortcutSidebar_->importRequested = [this] { importAtCommands(); };
        shortcutSidebar_->exportRequested = [this] { exportAtCommands(); };
        shortcutSidebar_->collapseRequested = [this] {
            preferences_.shortcutBarVisible = false;
            forceShortcutOpen_ = false;
            updateShortcutLayout();
            savePreferences();
        };
        connect(shortcutSplitter_, &QSplitter::splitterMoved, this, [this] {
            if (updatingShortcutLayout_ || shortcutSidebar_->isHidden()) return;
            preferences_.shortcutSidebarWidth = std::clamp(shortcutSidebar_->width(), 220, 480);
            forceShortcutOpen_ = true;
            savePreferences();
            updateShortcutLayout();
        });
        layout->addWidget(shortcutSplitter_, 1);
    } else layout->addWidget(channel.logSendBody, 1);

    auto *terminalFrame = new QFrame(channel.group);
    terminalFrame->setObjectName(QStringLiteral("terminalFrame"));
    auto *terminalLayout = new QVBoxLayout(terminalFrame);
    terminalLayout->setContentsMargins(0, 0, 0, 0);
    terminalLayout->setSpacing(0);

    auto *terminalHeader = new QFrame(terminalFrame);
    terminalHeader->setObjectName(QStringLiteral("terminalHeader"));
    auto *headerRows = new QVBoxLayout(terminalHeader);
    headerRows->setContentsMargins(0, 0, 0, 0);
    headerRows->setSpacing(0);
    auto *receiveHeader = new QHBoxLayout;
    headerRows->addLayout(receiveHeader);
    channel.receiveHeaderLayout = receiveHeader;
    receiveHeader->setContentsMargins(12, 5, 10, 5);
    receiveHeader->setSpacing(8);

    auto *receiveTitle = new QLabel(QStringLiteral("收发记录"), terminalHeader);
    channel.receiveTitle = receiveTitle;
    receiveTitle->setObjectName(QStringLiteral("sectionLabel"));

    channel.receiveHexCheck = new QCheckBox(QStringLiteral("HEX 显示"), terminalHeader);
    channel.timestampCheck = new QCheckBox(QStringLiteral("时间戳"), terminalHeader);
    channel.autoScrollCheck = new QCheckBox(QStringLiteral("自动滚动"), terminalHeader);
    channel.autoScrollCheck->setChecked(true);
    channel.autoScrollCheck->setToolTip(
        QStringLiteral("收到新数据时自动滚动到底部；取消勾选后可固定视图查看或复制历史日志"));

    channel.counterLabel = new QLabel(QStringLiteral("RX: 0 B  |  TX: 0 B"), terminalHeader);
    channel.counterLabel->setObjectName(QStringLiteral("counterLabel"));
    channel.counterLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    channel.resetCounterButton = new QPushButton(QStringLiteral("计数清零"), terminalHeader);
    channel.resetCounterButton->setObjectName(QStringLiteral("clearButton"));
    channel.resetCounterButton->setToolTip(QStringLiteral("将当前端点的 RX / TX 字节计数归零，保留收发记录"));

    channel.saveLogButton = new QPushButton(QStringLiteral("保存日志…"), terminalHeader);
    channel.saveLogButton->setObjectName(QStringLiteral("clearButton"));
    channel.saveLogButton->setToolTip(QStringLiteral("保存当前显示的日志；启用过滤时仅保存匹配记录"));

    channel.clearReceiveButton = new QPushButton(QStringLiteral("清空"), terminalHeader);
    channel.clearReceiveButton->setObjectName(QStringLiteral("clearButton"));
    channel.clearReceiveButton->setToolTip(QStringLiteral("清空当前收发记录，保留 RX / TX 字节计数"));

    receiveHeader->addWidget(receiveTitle);
    channel.truncatedLabel = new QLabel(QStringLiteral("已截断"), terminalHeader);
    channel.truncatedLabel->setObjectName(QStringLiteral("mutedLabel"));
    channel.truncatedLabel->setToolTip(QStringLiteral("缓冲已淘汰最早记录；保存日志仅包含当前显示内容"));
    channel.truncatedLabel->hide();
    receiveHeader->addWidget(channel.truncatedLabel);
    receiveHeader->addWidget(channel.receiveHexCheck);
    receiveHeader->addWidget(channel.timestampCheck);
    receiveHeader->addWidget(channel.autoScrollCheck);
    receiveHeader->addStretch();
    receiveHeader->addWidget(channel.counterLabel);
    channel.recordButton = new QPushButton(QStringLiteral("录制…"), terminalHeader);
    channel.recordButton->setObjectName(QStringLiteral("serialRecordButton"));
    channel.recordButton->setToolTip(QStringLiteral("持续录制到独立文件，不受清屏、过滤或显示截断影响"));
    receiveHeader->addWidget(channel.recordButton);
    channel.highlightButton = new QPushButton(QStringLiteral("高亮"), terminalHeader);
    channel.highlightButton->setObjectName(QStringLiteral("serialHighlightButton"));
    channel.highlightButton->setCheckable(true);
    channel.highlightButton->setToolTip(QStringLiteral("只读标记错误与警告词；不改变原始字节、导出内容或 ANSI 样式"));
    receiveHeader->addWidget(channel.highlightButton);
    channel.searchButton = new QPushButton(QStringLiteral("搜索"), terminalHeader);
    channel.searchButton->setObjectName(QStringLiteral("clearButton"));
    channel.saveLogButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Save));
    channel.saveLogButton->setIconSize({20, 20});
    channel.clearReceiveButton->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Clear));
    channel.clearReceiveButton->setIconSize({20, 20});
    channel.searchButton->setCheckable(true);
    channel.searchButton->setToolTip(QStringLiteral("搜索与过滤当前日志（Ctrl+F）"));
    receiveHeader->addWidget(channel.searchButton);
    receiveHeader->addWidget(channel.resetCounterButton);
    receiveHeader->addWidget(channel.saveLogButton);
    receiveHeader->addWidget(channel.clearReceiveButton);
    if (index == 1) buildTerminalOptions(receiveHeader);
    channel.counterRow = new QWidget(terminalHeader);
    channel.counterRowLayout = new QHBoxLayout(channel.counterRow);
    channel.counterRowLayout->setContentsMargins(12, 0, 10, 4);
    channel.counterRowLayout->addStretch();
    channel.counterRow->hide();
    headerRows->addWidget(channel.counterRow);
    terminalLayout->addWidget(terminalHeader);

    channel.searchArea = new QWidget(terminalFrame);
    auto *searchLayout = new QHBoxLayout(channel.searchArea);
    searchLayout->setContentsMargins(12, 4, 10, 4);
    searchLayout->setSpacing(6);
    channel.searchEdit = new QLineEdit(channel.searchArea);
    channel.searchEdit->setPlaceholderText(QStringLiteral("搜索当前显示内容（区分大小写）"));
    channel.searchEdit->setClearButtonEnabled(true);
    channel.searchEdit->installEventFilter(this);
    channel.matchCountLabel = new QLabel(QStringLiteral("0 / 0"), channel.searchArea);
    channel.matchCountLabel->setObjectName(QStringLiteral("mutedLabel"));
    channel.previousMatchButton = new QPushButton(QStringLiteral("↑"), channel.searchArea);
    channel.nextMatchButton = new QPushButton(QStringLiteral("↓"), channel.searchArea);
    for (auto *button : {channel.previousMatchButton, channel.nextMatchButton}) {
        button->setObjectName(QStringLiteral("clearButton"));
        button->setFixedWidth(34);
        button->setEnabled(false);
    }
    channel.previousMatchButton->setToolTip(QStringLiteral("上一个匹配（Shift+F3）"));
    channel.nextMatchButton->setToolTip(QStringLiteral("下一个匹配（Enter / F3）"));
    channel.filterCheck = new QCheckBox(QStringLiteral("仅看匹配"), channel.searchArea);
    auto *closeSearch = new QPushButton(QStringLiteral("关闭"), channel.searchArea);
    closeSearch->setObjectName(QStringLiteral("clearButton"));
    searchLayout->addWidget(channel.searchEdit, 1);
    searchLayout->addWidget(channel.matchCountLabel);
    searchLayout->addWidget(channel.previousMatchButton);
    searchLayout->addWidget(channel.nextMatchButton);
    searchLayout->addWidget(channel.filterCheck);
    searchLayout->addWidget(closeSearch);
    channel.searchArea->hide();
    terminalLayout->addWidget(channel.searchArea);
    channel.searchTimer = new QTimer(this);
    channel.searchTimer->setSingleShot(true);
    channel.searchTimer->setInterval(75);
    connect(channel.searchTimer, &QTimer::timeout, this, [this, index] {
        auto &current = channels_[index];
        if (current.filteredRenderPending) renderReceiveLog(index);
        else updateSearchMatches(index);
    });
    connect(channel.searchButton, &QPushButton::toggled, this, [this, index](bool shown) {
        auto &current = channels_[index];
        current.searchArea->setVisible(shown);
        if (shown) {
            current.searchEdit->setFocus();
        } else {
            const QSignalBlocker queryBlock(current.searchEdit);
            const QSignalBlocker filterBlock(current.filterCheck);
            current.searchEdit->clear();
            current.filterCheck->setChecked(false);
            renderReceiveLog(index);
            if (index == 1) focusTerminal();
        }
    });
    connect(closeSearch, &QPushButton::clicked, this, [this, index] {
        channels_[index].searchButton->setChecked(false);
    });
    connect(channel.searchEdit, &QLineEdit::textChanged, this, [this, index] {
        channels_[index].currentMatch = -1;
        if (channels_[index].filterCheck->isChecked()) renderReceiveLog(index);
        else updateSearchMatches(index);
        findLogMatch(index, 1);
    });
    connect(channel.filterCheck, &QCheckBox::toggled, this, [this, index] {
        channels_[index].currentMatch = -1;
        renderReceiveLog(index);
        findLogMatch(index, 1);
    });
    connect(channel.searchEdit, &QLineEdit::returnPressed, this, [this, index] { findLogMatch(index, 1); });
    connect(channel.previousMatchButton, &QPushButton::clicked, this, [this, index] { findLogMatch(index, -1); });
    connect(channel.nextMatchButton, &QPushButton::clicked, this, [this, index] { findLogMatch(index, 1); });

    channel.receiveEdit = new ReceiveConsole(terminalFrame);
    channel.receiveEdit->setObjectName(QStringLiteral("receiveConsole"));
    channel.receiveEdit->setReadOnly(true);
    // SerialLogBuffer 已限制日志容量，视图层不再二次隐式裁剪行数。
    channel.receiveEdit->setMaximumBlockCount(0);
    channel.receiveEdit->installEventFilter(this);
    channel.receiveEdit->setPlaceholderText(
        QStringLiteral("等待数据；发送成功显示 TX，接收数据显示 RX"));
    if (index == 1) {
        static_cast<ReceiveConsole*>(channel.receiveEdit)->scrollFollowChanged = [this](bool follow) {
            channels_[1].autoScrollCheck->setChecked(follow);
        };
        connect(channel.receiveEdit->verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int position) {
            if (workspaceState_.mode() == WorkspaceMode::Terminal)
                channels_[1].autoScrollCheck->setChecked(position >= channels_[1].receiveEdit->verticalScrollBar()->maximum());
        });
        channel.receiveEdit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(channel.receiveEdit, &QPlainTextEdit::customContextMenuRequested, this, [this](const QPoint& point) {
            auto* edit = channels_[1].receiveEdit;
            if (workspaceState_.mode() != WorkspaceMode::Terminal) {
                auto* menu = edit->createStandardContextMenu();
                menu->exec(edit->mapToGlobal(point)); delete menu; return;
            }
            QMenu menu(this);
            auto* copy = menu.addAction(QStringLiteral("复制\tCtrl+Shift+C"), edit, &QPlainTextEdit::copy);
            copy->setEnabled(edit->textCursor().hasSelection());
            auto* paste = menu.addAction(QStringLiteral("粘贴\tCtrl+Shift+V"), this, [this] {
                const auto bytes = testtools::terminalPasteBytes(toUtf8(QApplication::clipboard()->text()), terminalLineEnding());
                if (!bytes.empty()) { resumeTerminalInput(); sendBytes(1, bytes); }
            });
            paste->setEnabled(channels_[1].session.isOpen());
            menu.addSeparator();
            auto* interrupt = menu.addAction(QStringLiteral("中断当前命令\tCtrl+C"), this, [this] {
                resumeTerminalInput(); sendBytes(1, {0x03});
            });
            interrupt->setEnabled(channels_[1].session.isOpen());
            menu.exec(edit->mapToGlobal(point)); focusTerminal();
        });
    }
    terminalLayout->addWidget(channel.receiveEdit, 1);
    logSendLayout->addWidget(terminalFrame, 1);

    channel.manualSendArea = new QWidget(channel.group);
    auto *sendAreaLayout = new QVBoxLayout(channel.manualSendArea);
    sendAreaLayout->setContentsMargins(0, 0, 0, 0);
    sendAreaLayout->setSpacing(6);

    auto *composerFrame = new QFrame(channel.manualSendArea);
    channel.composerFrame = composerFrame;
    composerFrame->setObjectName(QStringLiteral("composerFrame"));
    auto *composerLayout = new QVBoxLayout(composerFrame);
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerLayout->setSpacing(0);

    channel.sendEdit = new QPlainTextEdit(composerFrame);
    channel.sendEdit->setObjectName(QStringLiteral("sendEditor"));
    channel.sendEdit->setFixedHeight(52);
    channel.sendEdit->setPlaceholderText(QStringLiteral("输入要发送的数据"));
    channel.sendEdit->installEventFilter(this);
    composerLayout->addWidget(channel.sendEdit);

    auto *composerFooter = new QFrame(composerFrame);
    composerFooter->setObjectName(QStringLiteral("composerFooter"));
    auto *sendTools = new QHBoxLayout(composerFooter);
    sendTools->setContentsMargins(10, 5, 10, 5);
    sendTools->setSpacing(8);
    channel.sendHexCheck = new QCheckBox(QStringLiteral("HEX 发送"), composerFooter);
    channel.lineEndingCombo = new QComboBox(composerFooter);
    channel.lineEndingCombo->addItems({QStringLiteral("无"), QStringLiteral("LF"),
                                       QStringLiteral("CR"), QStringLiteral("CRLF")});
    if (index == 1)
        channel.lineEndingCombo->setCurrentIndex(
            storedLineEndingIndex(atCommandLibrary_.lineEnding()));
    channel.lineEndingCombo->setToolTip(
        QStringLiteral("手动发送和文本快捷指令共用此行尾；切换端口不会改变选择。"));
    channel.lineEndingCombo->setMinimumWidth(84);

    auto *endingLabel = new QLabel(QStringLiteral("行尾"), composerFooter);
    endingLabel->setObjectName(QStringLiteral("fieldLabel"));

    channel.sendShortcutHint = new QLabel(QStringLiteral("Enter 发送 · Shift+Enter 换行"),
                                          composerFooter);
    channel.sendShortcutHint->setObjectName(QStringLiteral("fieldLabel"));

    channel.toggleAtBarButton =
        new QPushButton(QStringLiteral("快捷指令"), terminalHeader);
    channel.toggleAtBarButton->setObjectName(QStringLiteral("ghostButton"));
    channel.toggleAtBarButton->setToolTip(QStringLiteral("展开右侧快捷指令列表"));
    receiveHeader->addWidget(channel.toggleAtBarButton);
    channel.receiveMoreButton = new QPushButton(QStringLiteral("更多"), terminalHeader);
    channel.receiveMoreButton->setObjectName(QStringLiteral("ghostButton"));
    auto* toolsMenu = new QMenu(channel.receiveMoreButton);
    toolsMenu->addAction(QStringLiteral("计数清零"), channel.resetCounterButton, &QPushButton::click);
    toolsMenu->addAction(QStringLiteral("保存当前日志…"), channel.saveLogButton, &QPushButton::click);
    toolsMenu->addSeparator();
    const std::array<QCheckBox*, 3> checks{channel.receiveHexCheck, channel.timestampCheck, channel.autoScrollCheck};
    for (int option = 0; option < 3; ++option) {
        auto* check = checks[option];
        auto* action = toolsMenu->addAction(check->text());
        action->setCheckable(true);
        action->setChecked(check->isChecked());
        connect(action, &QAction::toggled, check, &QCheckBox::setChecked);
        connect(check, &QCheckBox::toggled, action, &QAction::setChecked);
        channel.displayActions[option] = action;
    }
    channel.receiveMoreButton->setMenu(toolsMenu);
    receiveHeader->addWidget(channel.receiveMoreButton);

    channel.clearSendButton = new QPushButton(QStringLiteral("清空"), composerFooter);
    channel.clearSendButton->setObjectName(QStringLiteral("clearButton"));
    channel.clearSendButton->setToolTip(QStringLiteral("清空上方发送输入框内容"));

    channel.sendButton = new QPushButton(QStringLiteral("发送"), composerFooter);
    channel.sendButton->setObjectName(QStringLiteral("primaryButton"));
    channel.sendButton->setEnabled(false);
    channel.sendButton->setMinimumWidth(72);

    channel.autoSendButton = new QPushButton(QStringLiteral("定时 / 次数…"), composerFooter);
    channel.autoSendButton->setObjectName(QStringLiteral("clearButton"));
    channel.autoSendButton->setEnabled(false);

    sendTools->addWidget(channel.sendHexCheck);
    sendTools->addWidget(endingLabel);
    sendTools->addWidget(channel.lineEndingCombo);
    sendTools->addWidget(channel.sendShortcutHint);
    sendTools->addStretch();
    sendTools->addWidget(channel.clearSendButton);
    sendTools->addWidget(channel.autoSendButton);
    sendTools->addWidget(channel.sendButton);
    composerLayout->addWidget(composerFooter);

    sendAreaLayout->addWidget(composerFrame);
    logSendLayout->addWidget(channel.manualSendArea);

    connect(channel.sourceCombo, &QComboBox::currentIndexChanged, this, [this, index] {
        applyPorts(index, false);
        updateRouteSummary();
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.portCombo, &QComboBox::currentTextChanged, this, [this, index] {
        updateRouteSummary();
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.baudCombo, &QComboBox::currentTextChanged, this, [this] {
        updateGlobalControls();
        savePreferences();
    });
    connect(channel.dataBitsCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.parityCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.stopBitsCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.flowControlCombo, &QComboBox::currentIndexChanged, this, [this] {
        savePreferences();
    });
    connect(channel.refreshPortsButton, &QPushButton::clicked, this, [this, index] {
        if (shuttingDown_ || channelActive(index))
            return;
        if (channelUsesAgent(index)) {
            if (!scanning_ && !hasActiveAgentConnection() && selectedAgentIsOnline())
                refreshAgentPorts();
        } else {
            refreshLocalPorts(true);
        }
    });
    connect(
        channel.advancedSettingsButton, &QPushButton::toggled, this, [this, index](bool visible) {
            channels_[index].advancedSettingsArea->setVisible(visible);
            channels_[index].advancedSettingsButton->setText(visible ? QStringLiteral("收起参数")
                                                                     : QStringLiteral("更多参数"));
        });
    connect(channel.connectButton, &QPushButton::clicked, this, [this, index] {
        if (channelActive(index)) {
            disconnectChannel(index);
        } else {
            connectChannel(index);
        }
    });
    connect(channel.receiveHexCheck, &QCheckBox::toggled, this, [this, index] {
        renderReceiveLog(index);
        savePreferences();
    });
    connect(channel.timestampCheck, &QCheckBox::toggled, this, [this, index] {
        renderReceiveLog(index);
        savePreferences();
    });
    connect(channel.autoScrollCheck, &QCheckBox::toggled, this, [this, index](bool checked) {
        if (checked && channels_[index].receiveEdit->verticalScrollBar()) {
            channels_[index].receiveEdit->verticalScrollBar()->setValue(
                channels_[index].receiveEdit->verticalScrollBar()->maximum());
        }
        savePreferences();
    });
    connect(channel.resetCounterButton, &QPushButton::clicked, this, [this, index] {
        resetChannelCounters(index);
    });
    connect(channel.saveLogButton, &QPushButton::clicked, this, [this, index] {
        saveChannelLog(index);
    });
    channel.highlighter = new SerialKeywordHighlighter(channel.receiveEdit->document());
    connect(channel.highlightButton, &QPushButton::toggled, this, [this, index](bool enabled) {
        channels_[index].highlighter->setEnabled(enabled);
        savePreferences();
    });
    connect(channel.recordButton, &QPushButton::clicked, this, [this, index] { toggleRecording(index); });
    connect(channel.clearReceiveButton, &QPushButton::clicked, this, [this, index] {
        channels_[index].displayDroppedBytes = 0;
        if (index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal) {
            terminalBuffer_.clear();
            renderTerminal(true);
            return;
        }
        channels_[index].logBuffer.clear();
        channels_[index].receiveEdit->clear();
        channels_[index].truncatedLabel->hide();
        updateSearchMatches(index);
    });
    connect(channel.toggleAtBarButton, &QPushButton::clicked, this, [this] {
        preferences_.shortcutBarVisible = true;
        forceShortcutOpen_ = true;
        updateShortcutLayout();
        savePreferences();
    });
    connect(channel.sendButton, &QPushButton::clicked, this, [this, index] { sendManual(index); });
    connect(channel.sendHexCheck, &QCheckBox::toggled, this, [this] { savePreferences(); });
    connect(channel.lineEndingCombo,
            &QComboBox::currentIndexChanged,
            this,
            [this, index](int selected) {
                if (index == 1) {
                    atCommandLibrary_.setLineEnding(storedLineEndingFromIndex(selected));
                    saveAtCommands();
                }
                savePreferences();
            });
    connect(channel.autoSendButton, &QPushButton::clicked, this, [this, index] {
        toggleAutoSend(index);
    });
    connect(channel.clearSendButton,
            &QPushButton::clicked,
            channel.sendEdit,
            &QPlainTextEdit::clear);

    setChannelStatus(index, QStringLiteral("未连接"));
    return channel.group;
}

void SerialWorkspace::buildTerminalOptions(QHBoxLayout* header)
{
    auto& channel = channels_[1];
    terminalOptionsButton_ = new QPushButton(QStringLiteral("终端选项…"), this);
    terminalOptionsButton_->setObjectName(QStringLiteral("clearButton"));
    auto* menu = new QMenu(terminalOptionsButton_);
    auto* endings = menu->addMenu(QStringLiteral("回车格式"));
    auto* endingGroup = new QActionGroup(endings);
    const std::array<QString, 3> names{QStringLiteral("CR"), QStringLiteral("LF"), QStringLiteral("CRLF")};
    const std::array<int, 3> values{2, 1, 3};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto* action = endings->addAction(names[i]);
        action->setCheckable(true); action->setData(values[i]);
        action->setChecked(values[i] == preferences_.terminalLineEnding);
        endingGroup->addAction(action); terminalEndingActions_[i] = action;
        connect(action, &QAction::triggered, this, [this, value = values[i]] {
            preferences_.terminalLineEnding = value; savePreferences();
        });
    }
    auto* backspace = menu->addMenu(QStringLiteral("退格键"));
    auto* backspaceGroup = new QActionGroup(backspace);
    for (std::size_t i = 0; i < 2; ++i) {
        const int value = i == 0 ? 127 : 8;
        auto* action = backspace->addAction(i == 0 ? QStringLiteral("DEL (7F)") : QStringLiteral("BS (08)"));
        action->setCheckable(true); action->setData(value);
        action->setChecked(value == preferences_.terminalBackspace);
        backspaceGroup->addAction(action); terminalBackspaceActions_[i] = action;
        connect(action, &QAction::triggered, this, [this, value] {
            preferences_.terminalBackspace = value; savePreferences();
        });
    }
    menu->addSeparator();
    for (auto* check : {channel.timestampCheck, channel.autoScrollCheck}) {
        auto* action = menu->addAction(check->text());
        action->setCheckable(true); action->setChecked(check->isChecked());
        connect(action, &QAction::toggled, check, &QCheckBox::setChecked);
        connect(check, &QCheckBox::toggled, action, &QAction::setChecked);
        connect(menu, &QMenu::aboutToShow, this, [check, action] {
            const QSignalBlocker blocker(action); action->setChecked(check->isChecked());
        });
    }
    menu->addAction(QStringLiteral("计数清零"), this, [this] { resetChannelCounters(1); });
    menu->addSeparator();
    menu->addAction(QStringLiteral("键盘说明…"), this, [this] {
        QMessageBox::information(this, QStringLiteral("串口终端键盘"),
            QStringLiteral("直接输入，Enter 执行；↑/↓ 历史、Tab 补全由设备处理。\n"
                           "Ctrl+C：中断设备前台命令，例如停止 ping。\n"
                           "Ctrl+Shift+C / V：复制 / 粘贴。\n"
                           "Ctrl+Shift+F：搜索；Shift+PageUp / PageDown：回看输出。\n"
                           "终端只显示设备回显；仅有内核日志不代表设备已开放交互式 Shell。"));
    });
    connect(menu, &QMenu::aboutToHide, this, [this] { QTimer::singleShot(0, this, &SerialWorkspace::focusTerminal); });
    terminalOptionsButton_->setMenu(menu);
    header->addWidget(terminalOptionsButton_);
    terminalRenderTimer_ = new QTimer(this);
    terminalRenderTimer_->setSingleShot(true);
    terminalRenderTimer_->setInterval(16);
    connect(terminalRenderTimer_, &QTimer::timeout, this, [this] {
        if (workspaceState_.mode() == WorkspaceMode::Terminal) renderTerminal();
    });
    terminalOptionsButton_->hide();
}

LineEnding SerialWorkspace::terminalLineEnding() const
{
    return lineEndingFromIndex(preferences_.terminalLineEnding);
}

void SerialWorkspace::focusTerminal()
{
    if (workspaceState_.mode() == WorkspaceMode::Terminal && channels_[1].receiveEdit)
        channels_[1].receiveEdit->setFocus(Qt::OtherFocusReason);
}

void SerialWorkspace::updateTerminalCursor(bool follow)
{
    auto* edit = channels_[1].receiveEdit;
    const auto row = terminalBuffer_.cursorRow();
    const auto& line = terminalBuffer_.lines()[row];
    const auto block = edit->document()->findBlockByNumber(static_cast<int>(row));
    if (!block.isValid()) return;
    int offset = channels_[1].timestampCheck->isChecked() && !line.timestamp.empty()
        ? static_cast<int>(fromUtf8(line.timestamp).size()) + 3 : 0;
    for (std::size_t i = 0; i < std::min(terminalBuffer_.cursorColumn(), line.cells.size()); ++i)
        offset += line.cells[i].character > 0xffff ? 2 : 1;
    QTextCursor caret(block);
    caret.setPosition(std::min(block.position() + offset, edit->document()->characterCount() - 1));
    static_cast<ReceiveConsole*>(edit)->setTerminalCursor(caret);
    if (follow && !edit->textCursor().hasSelection()) {
        edit->setTextCursor(caret); edit->ensureCursorVisible();
    }
}

void SerialWorkspace::resumeTerminalInput()
{
    auto& channel = channels_[1];
    channel.searchButton->setChecked(false);
    channel.autoScrollCheck->setChecked(true);
    auto cursor = channel.receiveEdit->textCursor(); cursor.clearSelection();
    channel.receiveEdit->setTextCursor(cursor);
    renderTerminal();
    updateTerminalCursor(true);
}

bool SerialWorkspace::handleTerminalKey(QKeyEvent* event)
{
    const auto key = event->key();
    const auto modifiers = event->modifiers();
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    auto* edit = channels_[1].receiveEdit;
    if ((control && shift && key == Qt::Key_F) || key == Qt::Key_F3) return false;
    if (control && shift && key == Qt::Key_C) { edit->copy(); return true; }
    if (shift && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        channels_[1].autoScrollCheck->setChecked(false);
        auto* scroll = edit->verticalScrollBar();
        scroll->setValue(scroll->value() + (key == Qt::Key_PageUp ? -1 : 1) * scroll->pageStep());
        return true;
    }
    if ((control && shift && key == Qt::Key_V) || (shift && key == Qt::Key_Insert)) {
        const auto bytes = testtools::terminalPasteBytes(toUtf8(QApplication::clipboard()->text()), terminalLineEnding());
        if (!bytes.empty()) { resumeTerminalInput(); sendBytes(1, bytes); }
        return true;
    }
    if (control && key >= Qt::Key_A && key <= Qt::Key_Z) {
        resumeTerminalInput();
        sendBytes(1, {static_cast<std::uint8_t>(key - Qt::Key_A + 1)});
        return true;
    }
    using Key = testtools::TerminalKey;
    ByteBuffer bytes;
    switch (key) {
    case Qt::Key_Return: case Qt::Key_Enter: bytes = testtools::terminalKeyBytes(Key::Enter, terminalLineEnding()); break;
    case Qt::Key_Tab: bytes = testtools::terminalKeyBytes(Key::Tab, terminalLineEnding()); break;
    case Qt::Key_Backspace: bytes = {static_cast<std::uint8_t>(preferences_.terminalBackspace)}; break;
    case Qt::Key_Up: bytes = testtools::terminalKeyBytes(Key::Up, terminalLineEnding()); break;
    case Qt::Key_Down: bytes = testtools::terminalKeyBytes(Key::Down, terminalLineEnding()); break;
    case Qt::Key_Right: bytes = testtools::terminalKeyBytes(Key::Right, terminalLineEnding()); break;
    case Qt::Key_Left: bytes = testtools::terminalKeyBytes(Key::Left, terminalLineEnding()); break;
    case Qt::Key_Home: bytes = testtools::terminalKeyBytes(Key::Home, terminalLineEnding()); break;
    case Qt::Key_End: bytes = testtools::terminalKeyBytes(Key::End, terminalLineEnding()); break;
    case Qt::Key_Delete: bytes = testtools::terminalKeyBytes(Key::Delete, terminalLineEnding()); break;
    case Qt::Key_PageUp: bytes = testtools::terminalKeyBytes(Key::PageUp, terminalLineEnding()); break;
    case Qt::Key_PageDown: bytes = testtools::terminalKeyBytes(Key::PageDown, terminalLineEnding()); break;
    case Qt::Key_Escape: bytes = testtools::terminalKeyBytes(Key::Escape, terminalLineEnding()); break;
    default: {
        if (control || modifiers.testFlag(Qt::MetaModifier)) return false;
        auto text = toUtf8(event->text());
        if (modifiers.testFlag(Qt::AltModifier) && !text.empty()) text.insert(text.begin(), '\x1b');
        bytes.assign(text.begin(), text.end());
        break;
    }
    }
    if (bytes.empty()) return false;
    resumeTerminalInput(); sendBytes(1, bytes);
    return true;
}

bool SerialWorkspace::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Resize &&
        (watched == channels_[0].logSendBody || watched == channels_[1].logSendBody)) {
        QTimer::singleShot(0, this, [this] { updateShortcutLayout(); });
    }
    if (workspaceState_.mode() == WorkspaceMode::Terminal &&
        watched == channels_[1].receiveEdit) {
        if (event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!(key->modifiers().testFlag(Qt::ControlModifier) && key->modifiers().testFlag(Qt::ShiftModifier)
                  && key->key() == Qt::Key_F) && key->key() != Qt::Key_F3) {
                key->accept();
                return true;
            }
        }
        if (event->type() == QEvent::InputMethod) {
            const auto text = toUtf8(static_cast<QInputMethodEvent*>(event)->commitString());
            if (!text.empty()) { resumeTerminalInput(); sendBytes(1, {text.begin(), text.end()}); }
            return true;
        }
        if (event->type() == QEvent::InputMethodQuery) {
            auto* query = static_cast<QInputMethodQueryEvent*>(event);
            query->setValue(Qt::ImEnabled, true);
            query->setValue(Qt::ImCursorRectangle, static_cast<ReceiveConsole*>(channels_[1].receiveEdit)->terminalCursorRect());
            query->setValue(Qt::ImFont, channels_[1].receiveEdit->font());
            query->setValue(Qt::ImSurroundingText, QString{});
            query->setValue(Qt::ImCursorPosition, 0);
            query->setValue(Qt::ImAnchorPosition, 0);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (!(key->key() == Qt::Key_Escape && channels_[1].searchButton->isChecked())
                && handleTerminalKey(key)) return true;
        }
    }
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            for (auto &channel : channels_) {
                if ((watched == channel.searchEdit || watched == channel.receiveEdit) &&
                    channel.searchButton->isChecked()) {
                    channel.searchButton->setChecked(false);
                    return true;
                }
            }
        }
        const bool enter = keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter;
        if (enter && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            for (int index = 0; index < 2; ++index) {
                if (watched == channels_[index].sendEdit) {
                    sendManual(index);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool SerialWorkspace::channelUsesAgent(int index) const
{
    return channels_[index].sourceCombo->currentData().toBool();
}

bool SerialWorkspace::selectedAgentIsOnline() const
{
    return !maintenanceBlocked_ && currentAdbDevice_.state == "device" && !currentAdbDevice_.serial.empty() &&
           agentControlPort_ != 0 && agentAdbSerial_ == currentAdbDevice_.serial;
}

void SerialWorkspace::refreshTargets()
{
    if (shuttingDown_ || channelActive(0) || channelActive(1) || scanning_) {
        return;
    }

    refreshLocalPorts();
    refreshAgentPorts();
}

void SerialWorkspace::refreshLocalPorts(bool announce)
{
    if (shuttingDown_)
        return;
    localPorts_ = testtools::SerialTransportService::localPorts();
    for (int index = 0; index < 2; ++index)
        if (!channelUsesAgent(index))
            applyPorts(index);
    if (announce)
        emit activityChanged(QStringLiteral("已刷新 PC端串口，共 %1 个").arg(localPorts_.size()));
    updateGlobalControls();
}

void SerialWorkspace::refreshAgentPorts()
{
    const auto generation = ++agentPortGeneration_;
    agentPorts_.clear();
    for (int index = 0; index < 2; ++index)
        if (channelUsesAgent(index))
            applyPorts(index);

    if (!selectedAgentIsOnline()) {
        scanning_ = false;
        updateGlobalControls();
        return;
    }

    scanning_ = true;
    emit activityChanged(QStringLiteral("正在读取 %1 的设备串口")
                             .arg(fromUtf8(currentAdbDevice_.serial)));
    updateGlobalControls();
    const std::uint16_t controlPort = agentControlPort_;
    QPointer<SerialWorkspace> guard(this);
    scanTasks_.start([guard, controlPort, generation] {
        std::string error;
        auto ports = testtools::SerialTransportService::agentPorts(controlPort, error);
        QMetaObject::invokeMethod(
            guard,
            [guard, generation, ports = std::move(ports), error = std::move(error)] {
                if (!guard || guard->shuttingDown_ || generation != guard->agentPortGeneration_) {
                    return;
                }
                guard->scanning_ = false;
                guard->agentPorts_ = std::move(ports);
                for (int index = 0; index < 2; ++index)
                    if (guard->channelUsesAgent(index))
                        guard->applyPorts(index);
                if (!error.empty()) {
                    emit guard->activityChanged(fromUtf8(error));
                } else {
                    emit guard->activityChanged(QStringLiteral("已发现 %1 个设备串口")
                                                    .arg(guard->agentPorts_.size()));
                }
                guard->updateGlobalControls();
            },
            Qt::QueuedConnection);
    });
}

void SerialWorkspace::setAdbDevice(const AdbDevice& device)
{
    // 本地 COM 连接已开启时仍允许切换当前选中的 ADB 设备上下文；
    // 活跃传输对象已各自持有独立的设备与配置快照。
    currentAdbDevice_ = device;
    refreshTargets();
    updateRouteSummary();
}

void SerialWorkspace::setServiceStatus(testtools::AgentServiceState state,
                                       const QString& detail,
                                       bool cleanupPending)
{
    if (serviceState_ == state && serviceDetail_ == detail &&
        serviceCleanupPending_ == cleanupPending)
        return;
    serviceCleanupPending_ = cleanupPending;
    serviceState_ = state;
    serviceDetail_ = detail;
    updateGlobalControls();
}

void SerialWorkspace::setAgentConnection(const std::string& adbSerial,
                                         std::uint16_t localControlPort)
{
    if (agentAdbSerial_ != adbSerial || agentControlPort_ != localControlPort) {
        for (int i = 0; i < 2; ++i)
            if (channelUsesAgent(i) && channelActive(i))
                disconnectChannel(i);
    }
    agentAdbSerial_ = adbSerial;
    agentControlPort_ = localControlPort;
    if (hasActiveConnections() && !hasActiveAgentConnection())
        refreshAgentPorts();
    else
        refreshTargets();
    updateRouteSummary();
}

bool SerialWorkspace::hasActiveConnections() const
{
    return channelActive(0) || channelActive(1);
}

bool SerialWorkspace::hasActiveAgentConnection() const
{
    return (channelUsesAgent(0) && channelActive(0)) ||
           (channelUsesAgent(1) && channelActive(1));
}

void SerialWorkspace::applyPorts(int index, bool preserveEditedEndpoint)
{
    auto& channel = channels_[index];
    if (!channel.portCombo || channelActive(index)) {
        return;
    }
    const QString previousData = selectedEndpoint(channel.portCombo);
    const QString previousText = channel.portCombo->currentText();
    const bool edited = previousText != channel.portCombo->itemText(channel.portCombo->currentIndex());
    const QString preferredEndpoint =
        fromUtf8(preferences_.channels[static_cast<std::size_t>(index)].endpoint);
    const QSignalBlocker blocker(channel.portCombo);
    channel.portCombo->clear();

    if (channelUsesAgent(index)) {
        for (const auto& port : agentPorts_) {
            QString detail = fromUtf8(port.interfaceName);
            if (!port.occupiedBy.empty()) {
                detail = QStringLiteral("占用中：%1").arg(fromUtf8(port.occupiedBy));
            } else if (!port.readable || !port.writable) {
                detail = QStringLiteral("权限不足");
            }
            const QString endpoint = fromUtf8(port.endpoint);
            channel.portCombo->addItem(detail.isEmpty()
                                           ? endpoint : endpoint + QStringLiteral(" — ") + detail,
                                       endpoint);
        }
        channel.portCombo->setToolTip(QStringLiteral("当前连接设备提供的串口节点"));
    } else {
        for (const auto &port : localPorts_) {
            const QString name = fromUtf8(port.name);
            const QString description = fromUtf8(port.description);
            channel.portCombo->addItem(description.isEmpty() ? name : name + QStringLiteral(" — ") + description,
                                       name);
        }
        channel.portCombo->setToolTip(QStringLiteral("PC端 COM 口，例如 COM3"));
    }

    int restoreIndex = channel.portCombo->findData(previousData);
    if (restoreIndex < 0) {
        restoreIndex = channel.portCombo->findData(previousText);
    }
    if (restoreIndex < 0 && !preferredEndpoint.isEmpty()) {
        restoreIndex = channel.portCombo->findData(preferredEndpoint);
    }
    if (restoreIndex >= 0) {
        channel.portCombo->setCurrentIndex(restoreIndex);
    } else if (preserveEditedEndpoint && edited && !previousText.trimmed().isEmpty()) {
        channel.portCombo->setEditText(previousText);
    } else if (!preferredEndpoint.isEmpty() && channel.portCombo->count() == 0) {
        channel.portCombo->setEditText(preferredEndpoint);
    }
    if (channel.portCombo->lineEdit()) {
        channel.portCombo->lineEdit()->setCursorPosition(0);
    }
    updateRouteSummary();
}

void SerialWorkspace::applyPreferencesToUi()
{
    applyingPreferences_ = true;
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[static_cast<std::size_t>(index)];
        const auto& pref = preferences_.channels[static_cast<std::size_t>(index)];
        const QSignalBlocker b1(channel.sourceCombo);
        const QSignalBlocker b2(channel.portCombo);
        const QSignalBlocker b3(channel.baudCombo);
        const QSignalBlocker b4(channel.dataBitsCombo);
        const QSignalBlocker b5(channel.parityCombo);
        const QSignalBlocker b6(channel.stopBitsCombo);
        const QSignalBlocker b7(channel.flowControlCombo);
        const QSignalBlocker b8(channel.lineEndingCombo);
        const QSignalBlocker b9(channel.receiveHexCheck);
        const QSignalBlocker b10(channel.timestampCheck);
        const QSignalBlocker b11(channel.autoScrollCheck);
        const QSignalBlocker b12(channel.sendHexCheck);

        channel.sourceCombo->setCurrentIndex(pref.useAgent ? 1 : 0);
        channel.baudCombo->setCurrentText(QString::number(pref.baudRate));
        if (channel.baudCombo->lineEdit()) {
            channel.baudCombo->lineEdit()->setCursorPosition(0);
        }
        channel.dataBitsCombo->setCurrentText(QString::number(pref.dataBits));
        channel.parityCombo->setCurrentIndex(std::clamp(pref.parity, 0, 2));
        channel.stopBitsCombo->setCurrentIndex(std::clamp(pref.stopBits, 0, 1));
        channel.flowControlCombo->setCurrentIndex(std::clamp(pref.flowControl, 0, 2));
        channel.lineEndingCombo->setCurrentIndex(std::clamp(pref.lineEnding, 0, 3));
        channel.receiveHexCheck->setChecked(pref.receiveHex);
        channel.timestampCheck->setChecked(pref.timestamp);
        channel.autoScrollCheck->setChecked(pref.autoScroll);
        channel.sendHexCheck->setChecked(pref.sendHex);
        channel.highlightButton->setChecked(pref.highlightKeywords);
        if (!pref.endpoint.empty()) {
            channel.portCombo->setEditText(fromUtf8(pref.endpoint));
            if (channel.portCombo->lineEdit()) {
                channel.portCombo->lineEdit()->setCursorPosition(0);
            }
        }
    }
    applyingPreferences_ = false;
    applyWorkspaceMode();
}

void SerialWorkspace::savePreferences()
{
    if (applyingPreferences_ || shuttingDown_ || preferencesStorePath_.empty()) {
        return;
    }
    for (int index = 0; index < 2; ++index) {
        const auto& channel = channels_[static_cast<std::size_t>(index)];
        auto& pref = preferences_.channels[static_cast<std::size_t>(index)];
        pref.useAgent = channelUsesAgent(index);
        const QString endpointText = selectedEndpoint(channel.portCombo);
        if (!endpointText.isEmpty()) {
            pref.endpoint = toUtf8(endpointText);
        }
        const int baud = channel.baudCombo->currentText().toInt();
        if (baud > 0) {
            pref.baudRate = baud;
        }
        pref.dataBits = channel.dataBitsCombo->currentText().toInt();
        pref.parity = channel.parityCombo->currentIndex();
        pref.stopBits = channel.stopBitsCombo->currentIndex();
        pref.flowControl = channel.flowControlCombo->currentIndex();
        pref.lineEnding = channel.lineEndingCombo->currentIndex();
        pref.receiveHex = channel.receiveHexCheck->isChecked();
        pref.timestamp = channel.timestampCheck->isChecked();
        if (workspaceState_.mode() != WorkspaceMode::Terminal || index != 1)
            pref.autoScroll = channel.autoScrollCheck->isChecked();
        pref.sendHex = channel.sendHexCheck->isChecked();
        pref.highlightKeywords = channel.highlightButton->isChecked();
    }
    std::string ignoredError;
    testtools::SerialPreferencesStore::save(preferencesStorePath_, preferences_, ignoredError);
}

SerialConfig SerialWorkspace::channelConfig(int index) const
{
    const auto &channel = channels_[index];
    SerialConfig config;
    config.endpoint = toUtf8(selectedEndpoint(channel.portCombo));
    config.adbSerial = channelUsesAgent(index) ? currentAdbDevice_.serial : std::string{};
    config.allowAdbRoot = false;
    config.baudRate = channel.baudCombo->currentText().toInt();
    config.dataBits = channel.dataBitsCombo->currentText().toInt();
    config.parity = channel.parityCombo->currentIndex() == 1 ? Parity::Odd
                    : channel.parityCombo->currentIndex() == 2 ? Parity::Even : Parity::None;
    config.stopBits = channel.stopBitsCombo->currentIndex() == 1 ? StopBits::Two : StopBits::One;
    config.flowControl = channel.flowControlCombo->currentIndex() == 1 ? FlowControl::Hardware
                         : channel.flowControlCombo->currentIndex() == 2 ? FlowControl::Software : FlowControl::None;
    return config;
}

bool SerialWorkspace::validateChannel(int index, QString &error) const
{
    const auto config = channelConfig(index);
    if (config.endpoint.empty()) {
        error = channelDisplayName(index) + QStringLiteral("：尚未选择串口");
        return false;
    }
    if (config.baudRate <= 0) {
        error = channelDisplayName(index) + QStringLiteral("：波特率无效");
        return false;
    }
    if (channelUsesAgent(index) && !selectedAgentIsOnline()) {
        error = QStringLiteral("请先在设备信息中连接当前设备，再连接") + channelDisplayName(index);
        return false;
    }

    auto candidate = endpointSelection(index);
    candidate.active = true;
    const auto conflict = testtools::SerialWorkspaceState::endpointConflict(
        candidate, endpointSelection(1 - index));
    if (conflict == testtools::SerialEndpointConflict::AgentSingleSession) {
        error = QStringLiteral("当前设备连接一次只能使用一个设备串口");
        return false;
    }
    if (conflict == testtools::SerialEndpointConflict::SameResource) {
        error = QStringLiteral("端点 A 和 B 不能同时打开同一个串口");
        return false;
    }
    return true;
}

testtools::SerialEndpointSelection SerialWorkspace::endpointSelection(int index) const
{
    testtools::SerialEndpointSelection selection;
    selection.active = channelActive(index);
    selection.kind = channelUsesAgent(index) ? testtools::SerialEndpointKind::Agent
                                             : testtools::SerialEndpointKind::Local;
    const auto config = channelConfig(index);
    selection.endpoint = config.endpoint;
    selection.deviceSerial = config.adbSerial;
    return selection;
}

void SerialWorkspace::connectChannel(int index)
{
    auto& channel = channels_[index];
    if (channelActive(index)) {
        return;
    }

    QString error;
    if (!validateChannel(index, error)) {
        QMessageBox::warning(this, QStringLiteral("无法连接"), error);
        return;
    }

    const bool agent = channelUsesAgent(index);
    std::string claimError;
    const auto key = testtools::SerialPortClaims::resourceKey(
        agent, agentAdbSerial_, channelConfig(index).endpoint);
    channel.portClaim = portClaims_->acquire(
        key, purpose_ == Purpose::Terminal ? "串口终端" : index == 0 ? "双端调试 A" : "单/双串口调试 B",
        claimError);
    if (!channel.portClaim) {
        setChannelStatus(index, fromUtf8(claimError), true);
        emit activityChanged(fromUtf8(claimError));
        return;
    }
    std::unique_ptr<ISerialTransport> transport;
    if (agent) {
        transport =
            testtools::SerialTransportService::createAgent(agentAdbSerial_, agentControlPort_);
    } else {
        transport = testtools::SerialTransportService::createLocal();
    }
    channel.connectButton->setText(QStringLiteral("取消 / 断开"));
    setChannelControlsEnabled(index, false);
    setChannelStatus(index, QStringLiteral("正在连接..."));
    updateGlobalControls();
    if (index == 1) terminalBuffer_.resetStream();
    channel.session.open(std::move(transport), channelConfig(index));
    updateGlobalControls();
}

void SerialWorkspace::attachSessionCallbacks(int index)
{
    QPointer<SerialWorkspace> guard(this);
    const auto queue = channels_[index].receiveQueue;
    channels_[index].session.setDataCallback(
        [guard, index, queue](std::uint64_t generation, const ByteBuffer& data) {
            if (queue->push(generation, data))
                QMetaObject::invokeMethod(guard, [guard, index] {
                    if (guard && !guard->shuttingDown_) guard->drainReceived(index);
                }, Qt::QueuedConnection);
        });
    channels_[index].session.setStatusCallback(
        [guard, index](std::uint64_t generation, TransportState state, const std::string& message) {
            QMetaObject::invokeMethod(
                guard,
                [guard, index, generation, state, message] {
                    if (!guard || guard->shuttingDown_) {
                        return;
                    }
                    if (guard->channels_[index].session.generation() != generation) {
                        return;
                    }
                    auto& channel = guard->channels_[index];
                    const QString status = fromUtf8(message);
                    emit guard->activityChanged(status);
                    const bool connected = state == TransportState::Connected;
                    const bool progress = state == TransportState::Progress;
                    const bool error = state == TransportState::Error;
                    guard->setChannelStatus(index, status, error);
                    if (connected) {
                        channel.connectButton->setText(QStringLiteral("断开"));
                        channel.sendButton->setEnabled(true);
                    } else if (!progress) {
                        if (!channel.session.isOpen()) {
                            guard->disconnectChannel(index, false);
                            guard->setChannelStatus(index, status, error);
                        }
                    }
                    guard->updateGlobalControls();
                    if (connected && index == 1) guard->focusTerminal();
                },
                Qt::QueuedConnection);
        });
    channels_[index].session.setSendCallback([guard, index](std::uint64_t generation,
                                                            const ByteBuffer& data,
                                                            std::uint64_t tag,
                                                            bool succeeded,
                                                            bool scheduledSendStopped) {
        QMetaObject::invokeMethod(
            guard,
            [guard, index, generation, data, tag, succeeded, scheduledSendStopped] {
                if (!guard || guard->shuttingDown_) {
                    return;
                }
                auto& current = guard->channels_[index];
                if (current.session.generation() != generation) {
                    return;
                }
                if (!succeeded) {
                    guard->setChannelStatus(
                        index, QStringLiteral("发送失败，连接可能已经断开"), true);
                    guard->stopAutoSend(index, false);
                    emit guard->activityChanged(
                        scheduledSendStopped
                            ? QStringLiteral("%1 发送失败；计划发送已停止")
                                  .arg(guard->channelDisplayName(index))
                            : QStringLiteral("%1 发送失败").arg(guard->channelDisplayName(index)));
                    if (guard->stressTestRunning_) {
                        guard->stopStressTest(QStringLiteral("可靠性压测发送失败"));
                    }
                    guard->updateGlobalControls();
                    return;
                }
                if ((tag & 1u) != 0u) {
                    guard->appendTransmitted(index, data);
                }
                guard->updateCounter(index);
            },
            Qt::QueuedConnection);
    });
}

void SerialWorkspace::drainReceived(int index)
{
    auto& channel = channels_[index];
    const auto batch = channel.receiveQueue->take();
    if (batch.generation == channel.session.generation()) {
        if (channel.displayGeneration != batch.generation) {
            channel.displayGeneration = batch.generation;
            channel.displayDroppedBytes = 0;
        }
        if (batch.droppedBytes) {
            channel.displayDroppedBytes += batch.droppedBytes;
            if (index == 1) terminalBuffer_.resetStream();
            channel.detectionBuffer.clear();
            if (stressTestRunning_) {
                stopStressTest(QStringLiteral("接收显示积压，压测结果不完整"));
                stressResultLabel_->setText(stressResultLabel_->text() + QStringLiteral("\n接收积压，结果不完整"));
            }
            if (bidirectionalTestRunning_) {
                bidirectionalTestRunning_ = false; ++testGeneration_;
                channels_[0].expectedFrame.clear(); channels_[1].expectedFrame.clear();
                testResultLabel_->setText(QStringLiteral("接收积压，双向验证已停止，请重试"));
                updateGlobalControls();
            }
            emit activityChanged(QStringLiteral("%1 显示积压，已跳过 %2；录制状态请查看录制入口")
                .arg(channelDisplayName(index), recordingSize(batch.droppedBytes)));
        }
        channel.receivingBatch = true;
        channel.receivedAppend = {};
        for (const auto& bytes : batch.chunks) appendReceived(index, bytes);
        channel.receivingBatch = false;
        applyLogAppendResult(index, channel.receivedAppend);
        updateTruncation(index);
        updateCounter(index);
    }
    if (batch.more) QTimer::singleShot(0, this, [this, index] {
        if (!shuttingDown_) drainReceived(index);
    });
}

void SerialWorkspace::updateTruncation(int index)
{
    auto& channel = channels_[index];
    const bool terminal = index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal;
    channel.truncatedLabel->setVisible(channel.displayDroppedBytes ||
        (terminal ? terminalBuffer_.truncated() : channel.logBuffer.truncated()));
    channel.truncatedLabel->setToolTip(channel.displayDroppedBytes
        ? QStringLiteral("接收显示队列已跳过 %1；显示与导出可能不连续。独立录制不经过此队列，请查看其完成状态。")
            .arg(recordingSize(channel.displayDroppedBytes))
        : QStringLiteral("显示缓冲已淘汰最早记录；保存当前日志只包含保留的显示内容"));
}

void SerialWorkspace::toggleRecording(int index)
{
    auto& session = channels_[index].session;
    const auto state = session.recordingSnapshot();
    if (state.busy) {
        session.stopRecording("用户停止", state.id); pollRecordings(); return;
    }
    if (!session.isOpen() || shuttingDown_) return;
    const auto generation = session.generation();
    const QString textFilter = QStringLiteral("带标记文本 (*.log)");
    QString selected = textFilter;
    const auto filename = QFileDialog::getSaveFileName(this, QStringLiteral("持续录制串口数据"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(
            QStringLiteral("serial-%1.log").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))),
        textFilter + QStringLiteral(";;原始 RX 字节 (*.bin)"), &selected, QFileDialog::DontConfirmOverwrite);
    if (filename.isEmpty()) return;
    testtools::SerialRecordOptions options;
    options.path = std::filesystem::path(filename.toStdWString());
    options.format = selected == textFilter ? testtools::SerialRecordFormat::TaggedText : testtools::SerialRecordFormat::RawRx;
    const QString metadata = QString::fromStdWString(testtools::SerialRecorder::metadataPath(options.path).wstring());
    if (QFile::exists(filename) || QFile::exists(metadata)) {
        if (QMessageBox::question(this, QStringLiteral("覆盖录制文件"),
            QStringLiteral("将覆盖以下录制文件和元数据：\n%1\n%2").arg(filename, metadata),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        options.overwrite = true;
    }
    std::string error;
    if (!session.startRecording(options, generation, error))
        emit activityChanged(QStringLiteral("%1：%2").arg(channelDisplayName(index), fromUtf8(error)));
    pollRecordings();
}

void SerialWorkspace::pollRecordings()
{
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[index];
        const auto state = channel.session.recordingSnapshot();
        using State = testtools::SerialRecordState;
        const bool stopping = state.busy && state.state != State::Starting && state.state != State::Recording;
        channel.recordButton->setText(stopping ? QStringLiteral("正在保存…")
            : state.busy ? QStringLiteral("■ 停止录制 · %1").arg(recordingSize(state.writtenFileBytes))
            : state.state == State::Failed ? QStringLiteral("录制失败 · 重试…") : QStringLiteral("录制…"));
        channel.recordButton->setEnabled(!shuttingDown_ && !stopping && (state.busy || channel.session.isOpen()));
        const auto visualState = state.state == State::Failed ? QStringLiteral("failed")
            : state.busy ? QStringLiteral("recording") : QStringLiteral("idle");
        if (channel.recordButton->property("recordState").toString() != visualState) {
            channel.recordButton->setProperty("recordState", visualState);
            channel.recordButton->style()->unpolish(channel.recordButton);
            channel.recordButton->style()->polish(channel.recordButton);
        }
        const auto path = QString::fromStdWString(state.path.wstring());
        channel.recordButton->setToolTip(state.id
            ? QStringLiteral("%1\n%2 · 已写 %3 · 待写 %4%5").arg(path,
                state.state == State::Starting ? QStringLiteral("准备文件，已开始缓存数据")
                : stopping ? QStringLiteral("正在保存已接受数据")
                : state.busy ? QStringLiteral("正在录制")
                : state.state == State::Completed ? QStringLiteral("已完成") : QStringLiteral("录制失败"),
                recordingSize(state.writtenFileBytes), recordingSize(state.pendingBytes),
                state.error.empty() ? QString() : QStringLiteral("\n") + fromUtf8(state.error))
            : QStringLiteral("持续录制到独立文件；文本包含 RX/TX，原始格式只保存 RX"));
        const auto announcement = QStringLiteral("%1/%2/%3/%4").arg(state.id).arg(int(state.state)).arg(state.busy).arg(fromUtf8(state.error));
        if (state.id && announcement != channel.recordAnnouncement) {
            channel.recordAnnouncement = announcement;
            const QString message = !state.error.empty() ? fromUtf8(state.error)
                : stopping ? QStringLiteral("正在保存录制…")
                : state.state == State::Starting ? QStringLiteral("正在准备录制…")
                : state.busy ? QStringLiteral("正在录制") : QStringLiteral("录制已完成");
            emit activityChanged(QStringLiteral("%1 · %2 · %3").arg(channelDisplayName(index), message, path));
        }
    }
}

std::vector<SerialWorkspace::BackgroundTask> SerialWorkspace::activeTasks() const
{
    std::vector<BackgroundTask> result;
    const auto prefix = purpose_ == Purpose::Terminal ? QStringLiteral("串口终端") : QStringLiteral("串口调试");
    for (int index = 0; index < 2; ++index) {
        const auto& session = channels_[index].session;
        const auto record = session.recordingSnapshot();
        const auto state = session.snapshot();
        if (record.busy) {
            const bool saving = record.state != testtools::SerialRecordState::Starting && record.state != testtools::SerialRecordState::Recording;
            result.push_back({BackgroundTask::Kind::Recording, index, record.id,
                QStringLiteral("%1 · %2 · %3 · %4").arg(prefix, channelDisplayName(index),
                    saving ? QStringLiteral("录制保存中") : QStringLiteral("录制"), recordingSize(record.writtenFileBytes)), !saving});
        }
        if (state.scheduledSend.running)
            result.push_back({BackgroundTask::Kind::ScheduledSend, index, state.scheduledSend.id,
                QStringLiteral("%1 · %2 · 计划发送 · 已提交 %3 次").arg(prefix, channelDisplayName(index)).arg(state.scheduledSend.submitted)});
    }
    if (stressTestRunning_) result.push_back({BackgroundTask::Kind::Stress, 0, stressGeneration_, QStringLiteral("串口调试 · 可靠性压测")});
    return result;
}

void SerialWorkspace::stopTask(const BackgroundTask& task)
{
    if (task.channel < 0 || task.channel >= 2 || shuttingDown_) return;
    auto& session = channels_[task.channel].session;
    if (task.kind == BackgroundTask::Kind::Recording) session.stopRecording("用户停止", task.token);
    else if (task.kind == BackgroundTask::Kind::ScheduledSend && session.snapshot().scheduledSend.id == task.token) stopAutoSend(task.channel, true);
    else if (task.kind == BackgroundTask::Kind::Stress && stressTestRunning_ && stressGeneration_ == task.token)
        stopStressTest(QStringLiteral("用户已停止可靠性压测"));
    pollRecordings(); updateGlobalControls();
}

void SerialWorkspace::stopRecordings()
{
    for (auto& channel : channels_) channel.session.stopRecording("应用退出");
}

bool SerialWorkspace::hasPendingRecordings() const
{
    for (const auto& channel : channels_) if (channel.session.recordingSnapshot().busy) return true;
    return false;
}

void SerialWorkspace::disconnectChannel(int index, bool showClosedStatus)
{
    auto& channel = channels_[index];
    const auto snapshot = channel.session.snapshot();
    if (channel.disconnectPending ||
        snapshot.state == testtools::SerialSessionState::Disconnecting) {
        return;
    }
    stopAutoSend(index);
    if (stressTestRunning_) {
        stopStressTest(QStringLiteral("串口连接已断开"));
    }
    channel.expectedFrame.clear();
    channel.detectionBuffer.clear();
    bidirectionalTestRunning_ = false;
    ++testGeneration_;

    if (!snapshot.active) {
        completeChannelDisconnect(index, channel.session.generation(), showClosedStatus);
        return;
    }

    channel.connectButton->setText(QStringLiteral("正在断开…"));
    channel.connectButton->setEnabled(false);
    channel.sendButton->setEnabled(false);
    channel.disconnectPending = true;
    setChannelControlsEnabled(index, false);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("正在断开..."));
    }
    QPointer<SerialWorkspace> guard(this);
    channel.session.closeAsync([guard, index, showClosedStatus](std::uint64_t generation) {
        QMetaObject::invokeMethod(
            guard,
            [guard, index, generation, showClosedStatus] {
                if (guard && !guard->shuttingDown_) {
                    guard->completeChannelDisconnect(index, generation, showClosedStatus);
                }
            },
            Qt::QueuedConnection);
    });
    if (index == 1) {
        terminalBuffer_.resetStream();
        if (workspaceState_.mode() == WorkspaceMode::Terminal) scheduleTerminalRender();
    }
    updateGlobalControls();
}

void SerialWorkspace::completeChannelDisconnect(int index,
                                                std::uint64_t generation,
                                                bool showClosedStatus)
{
    auto& channel = channels_[index];
    if (channel.session.generation() != generation || channel.session.isActive()) {
        return;
    }
    channel.disconnectPending = false;
    channel.portClaim.reset();
    channel.controlPending = false;
    channel.lineStates = {Qt::PartiallyChecked, Qt::PartiallyChecked};
    for (auto *control : {channel.dtrCheck, channel.rtsCheck}) {
        const QSignalBlocker blocker(control);
        control->setCheckState(Qt::PartiallyChecked);
    }
    channel.connectButton->setText(QStringLiteral("连接"));
    channel.sendButton->setEnabled(false);
    setChannelControlsEnabled(index, true);
    if (showClosedStatus) {
        setChannelStatus(index, QStringLiteral("已断开"));
    }
    updateGlobalControls();
}

void SerialWorkspace::disconnectAll()
{
    disconnectChannel(0, channelActive(0));
    disconnectChannel(1, channelActive(1));
}

void SerialWorkspace::setChannelControlsEnabled(int index, bool enabled)
{
    auto& channel = channels_[index];
    channel.sourceCombo->setEnabled(enabled);
    channel.portCombo->setEnabled(enabled);
    channel.baudCombo->setEnabled(enabled);
    channel.dataBitsCombo->setEnabled(enabled);
    channel.parityCombo->setEnabled(enabled);
    channel.stopBitsCombo->setEnabled(enabled);
    channel.flowControlCombo->setEnabled(enabled);
}

void SerialWorkspace::updateGlobalControls()
{
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[index];
        const bool needsService = channelUsesAgent(index) && !selectedAgentIsOnline();
        const bool connecting = serviceState_ == testtools::AgentServiceState::Connecting;
        channel.serviceHint->setVisible(needsService);
        channel.statusLabel->setVisible(!needsService);
        channel.connectButton->setVisible(!needsService);
        channel.serviceHintLabel->setText(maintenanceBlocked_ ? QStringLiteral("设备维护中…") : connecting ? QStringLiteral("正在启动服务…")
            : serviceState_ == testtools::AgentServiceState::Failed ? QStringLiteral("服务连接失败")
            : QStringLiteral("需连接板端服务"));
        channel.serviceHintLabel->setToolTip(currentAdbDevice_.state != "device"
            ? QStringLiteral("请先选择在线且已授权的 ADB 设备") : serviceDetail_);
        channel.serviceConnectButton->setText(serviceState_ == testtools::AgentServiceState::Failed
            ? QStringLiteral("重试连接") : QStringLiteral("连接服务"));
        channel.serviceConnectButton->setEnabled(currentAdbDevice_.state == "device" && !connecting &&
            !maintenanceBlocked_ && !serviceCleanupPending_ && !hasActiveAgentConnection());
    }

    const auto firstSnapshot = channels_[0].session.snapshot();
    const auto secondSnapshot = channels_[1].session.snapshot();
    const bool firstActive = channelActive(0);
    const bool secondActive = channelActive(1);
    const bool anyActive = firstActive || secondActive;
    const bool bothOpen = firstSnapshot.open && secondSnapshot.open;
    const bool anyControlPending = channels_[0].controlPending || channels_[1].controlPending;
    const bool anyAutoSend =
        firstSnapshot.scheduledSend.running || secondSnapshot.scheduledSend.running;
    const auto resourceConflict = [this] {
        auto first = endpointSelection(0);
        auto second = endpointSelection(1);
        first.active = true;
        second.active = true;
        return !first.endpoint.empty() && !second.endpoint.empty() &&
               testtools::SerialWorkspaceState::endpointConflict(first, second) !=
                   testtools::SerialEndpointConflict::None;
    };
    const auto channelReady = [this, &resourceConflict](int index) {
        const auto& channel = channels_[index];
        const bool basicReady = !channel.portCombo->currentText().trimmed().isEmpty() &&
                                channel.baudCombo->currentText().toInt() > 0 &&
                                (!channelUsesAgent(index) || selectedAgentIsOnline());
        const int otherIndex = 1 - index;
        const bool conflictsWithOpenChannel = channelActive(otherIndex) && resourceConflict();
        return basicReady && !conflictsWithOpenChannel;
    };
    for (int index = 0; index < 2; ++index) {
        const auto session = channels_[index].session.snapshot();
        const bool open = session.open;
        if (channels_[index].refreshPortsButton) {
            const bool agent = channelUsesAgent(index);
            channels_[index].refreshPortsButton->setEnabled(
                !shuttingDown_ && !channelActive(index) &&
                (!agent || (!scanning_ && !hasActiveAgentConnection() && selectedAgentIsOnline())));
            channels_[index].refreshPortsButton->setToolTip(
                agent ? selectedAgentIsOnline() ? QStringLiteral("重新扫描设备串口列表")
                                                : QStringLiteral("请先连接当前设备的板端服务")
                      : QStringLiteral("重新扫描 PC端串口，无需连接板端服务"));
        }
        if (channels_[index].disconnectPending ||
            session.state == testtools::SerialSessionState::Disconnecting) {
            channels_[index].connectButton->setEnabled(false);
        } else if (channelActive(index)) {
            channels_[index].connectButton->setEnabled(true);
        } else {
            channels_[index].connectButton->setEnabled(
                (!scanning_ || !channelUsesAgent(index)) && channelReady(index));
        }
        channels_[index].sendButton->setEnabled(open && !stressTestRunning_ &&
                                                !session.scheduledSend.running);
        const auto role = channelActive(index) ? QStringLiteral("secondary") : QStringLiteral("primary");
        auto* connectButton = channels_[index].connectButton;
        if (connectButton->property("buttonRole").toString() != role) {
            connectButton->setProperty("buttonRole", role);
            connectButton->style()->unpolish(connectButton);
            connectButton->style()->polish(connectButton);
        }
        if (index == 1 && shortcutSidebar_) {
            const bool editable = !stressTestRunning_ && !bidirectionalTestRunning_ && !session.scheduledSend.running;
            shortcutSidebar_->setActionsEnabled(editable, open && editable);
        }
        auto &current = channels_[index];
        const bool controlsReady = open && !current.controlPending && !stressTestRunning_ &&
                                   !bidirectionalTestRunning_ && !session.scheduledSend.running;
        current.dtrCheck->setEnabled(controlsReady && current.session.supportsControlLine(SerialControlLine::Dtr));
        current.rtsCheck->setEnabled(controlsReady && current.session.supportsControlLine(SerialControlLine::Rts));
        current.rtsCheck->setToolTip(
            channelUsesAgent(index) ? QStringLiteral("设备串口暂不支持手动控制 RTS")
            : current.flowControlCombo->currentIndex() == 1
                ? QStringLiteral("RTS 正由硬件流控管理")
                : QStringLiteral("连接 PC端串口后可设置；半选表示本次连接尚未手动设置"));
        current.dtrCheck->setToolTip(
            channelUsesAgent(index)
                ? QStringLiteral("设备串口暂不支持手动控制 DTR")
                : QStringLiteral("连接 PC端串口后可设置；半选表示本次连接尚未手动设置"));
        channels_[index].autoSendButton->setEnabled(open && !stressTestRunning_ && !current.controlPending);
    }
    const bool missingChannelsReady =
        (firstActive || channelReady(0)) && (secondActive || channelReady(1));
    const bool pairCompatible = firstActive || secondActive || !resourceConflict();
    const bool transitioning =
        firstSnapshot.state == testtools::SerialSessionState::Connecting ||
        secondSnapshot.state == testtools::SerialSessionState::Connecting ||
        firstSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        secondSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        channels_[0].disconnectPending || channels_[1].disconnectPending;
    const bool waitingForAgent = scanning_ &&
        ((channelUsesAgent(0) && !firstActive) || (channelUsesAgent(1) && !secondActive));
    connectBothButton_->setEnabled(!waitingForAgent && !bothOpen && !transitioning &&
                                   missingChannelsReady && pairCompatible);
    connectBothButton_->setText(bothOpen    ? QStringLiteral("两端已连接")
                                : anyActive ? QStringLiteral("连接另一端")
                                            : QStringLiteral("连接两端"));
    disconnectBothButton_->setEnabled(anyActive && !transitioning);
    bidirectionalTestButton_->setEnabled(bothOpen && !anyControlPending && !bidirectionalTestRunning_ &&
                                         !stressTestRunning_ && !anyAutoSend);
    bidirectionalTestButton_->setText(bidirectionalTestRunning_ ? QStringLiteral("正在验证…")
                                                                : QStringLiteral("双向收发验证"));
    stressTestButton_->setEnabled(stressTestRunning_ ||
                                  (bothOpen && !anyControlPending && !bidirectionalTestRunning_ && !anyAutoSend));
    stressTestButton_->setText(stressTestRunning_ ? QStringLiteral("停止可靠性压测")
                                                  : QStringLiteral("可靠性压测…"));
    updateShortcutLayout();
    pollRecordings();
    emit busyChanged(anyActive);
}

void SerialWorkspace::setChannelStatus(int index, const QString& message, bool error)
{
    auto* label = channels_[index].statusLabel;
    QString display = message;
    QString role = QStringLiteral("connectionStatusIdle");
    if (message.startsWith(QStringLiteral("正在断开"))) {
        display = QStringLiteral("断开中");
        role = QStringLiteral("connectionStatusWorking");
    } else if (message.startsWith(QStringLiteral("正在"))) {
        display = QStringLiteral("连接中");
        role = QStringLiteral("connectionStatusWorking");
    } else if (!error && message.contains(QStringLiteral("已连接"))) {
        display = QStringLiteral("已连接");
        role = QStringLiteral("connectionStatusGood");
    } else if (error) {
        display = QStringLiteral("异常");
        role = QStringLiteral("connectionStatusError");
    }
    label->setText(QStringLiteral("● ") + display);
    label->setToolTip(message);
    setVisualRole(label, role);
}

void SerialWorkspace::updateRouteSummary()
{
    if (!routeLabel_ || !channels_[0].sourceCombo || !channels_[1].sourceCombo) {
        return;
    }
    const auto endpoint = [this](int index) {
        const auto& channel = channels_[index];
        const QString type =
            channelUsesAgent(index) ? QStringLiteral("设备") : QStringLiteral("PC端");
        const QString port = selectedEndpoint(channel.portCombo);
        return QStringLiteral("%1 · %2").arg(type,
                                             port.isEmpty() ? QStringLiteral("未选择") : port);
    };
    routeLabel_->setText(QStringLiteral("A  %1     ⇄     B  %2").arg(endpoint(0), endpoint(1)));
}

QString SerialWorkspace::channelDisplayName(int index) const
{
    if (workspaceState_.mode() != WorkspaceMode::Dual && index == 1) {
        return QStringLiteral("当前串口");
    }
    return channelName(index);
}

bool SerialWorkspace::channelActive(int index) const
{
    return channels_[index].disconnectPending || channels_[index].session.isActive();
}

ByteBuffer SerialWorkspace::manualPayload(int index, bool& ok)
{
    auto& channel = channels_[index];
    ByteBuffer data;
    ok = true;
    const QString input = channel.sendEdit->toPlainText();
    if ((!channel.sendHexCheck->isChecked() && input.isEmpty())
        || (channel.sendHexCheck->isChecked() && input.trimmed().isEmpty())) {
        return {};
    }
    if (channel.sendHexCheck->isChecked()) {
        std::string error;
        if (!parseHex(toUtf8(input), data, error)) {
            QMessageBox::warning(this, QStringLiteral("HEX 输入错误"), fromUtf8(error));
            ok = false;
            return {};
        }
    } else {
        const QByteArray text = input.toUtf8();
        data.assign(reinterpret_cast<const std::uint8_t*>(text.constData()),
                    reinterpret_cast<const std::uint8_t*>(text.constData()) + text.size());
    }
    appendLineEnding(data, lineEndingFromIndex(channel.lineEndingCombo->currentIndex()));
    return data;
}

bool SerialWorkspace::sendBytes(int index, const ByteBuffer& data, bool echo)
{
    auto& channel = channels_[index];
    if (!channel.session.isOpen()) {
        setChannelStatus(index, QStringLiteral("尚未连接，无法发送"), true);
        return false;
    }
    if (!channel.session.send(data, echo ? 1u : 0u)) {
        setChannelStatus(index, QStringLiteral("发送队列已满或不可用"), true);
        return false;
    }
    return true;
}

void SerialWorkspace::sendManual(int index)
{
    bool ok = false;
    const auto data = manualPayload(index, ok);
    if (!ok) {
        return;
    }
    if (data.empty()) {
        QMessageBox::information(
            this, QStringLiteral("没有发送内容"), QStringLiteral("请输入要发送的数据。"));
        return;
    }
    if (!channels_[index].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("请先连接当前串口。"));
        return;
    }
    sendBytes(index, data);
}

void SerialWorkspace::sendAtCommand(int index, int commandIndex)
{
    if (stressTestRunning_ || bidirectionalTestRunning_ || channels_[index].session.snapshot().scheduledSend.running)
        return;
    const auto& commands = atCommandLibrary_.commands();
    if (commandIndex < 0 || commandIndex >= static_cast<int>(commands.size())) {
        QMessageBox::information(
            this, QStringLiteral("未选择指令"), QStringLiteral("请先选择要发送的快捷指令。"));
        return;
    }
    if (!channels_[index].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("请先连接当前串口。"));
        return;
    }
    const auto& command = commands[static_cast<std::size_t>(commandIndex)];
    ByteBuffer data;
    if (command.hex) {
        std::string error;
        if (!parseHex(command.command, data, error)) {
            QMessageBox::warning(this, QStringLiteral("HEX 指令格式错误"), fromUtf8(error));
            return;
        }
    } else {
        data.assign(command.command.begin(), command.command.end());
        appendLineEnding(data, lineEndingFromIndex(channels_[index].lineEndingCombo->currentIndex()));
    }
    if (sendBytes(index, data)) {
        emit activityChanged(QStringLiteral("%1 已发送快捷指令：%2")
                                 .arg(channelDisplayName(index), fromUtf8(command.name)));
    }
}

void SerialWorkspace::fillAtCommandToInput(int index, int commandIndex)
{
    if (stressTestRunning_ || bidirectionalTestRunning_ || channels_[index].session.snapshot().scheduledSend.running)
        return;
    const auto& commands = atCommandLibrary_.commands();
    if (commandIndex < 0 || commandIndex >= static_cast<int>(commands.size())) {
        QMessageBox::information(
            this, QStringLiteral("未选择指令"), QStringLiteral("请先选择要填入的快捷指令。"));
        return;
    }
    const auto& command = commands[static_cast<std::size_t>(commandIndex)];
    auto& channel = channels_[index];
    channel.sendHexCheck->setChecked(command.hex);
    channel.sendEdit->setPlainText(fromUtf8(command.command));
    channel.sendEdit->setFocus();
    channel.sendEdit->moveCursor(QTextCursor::End);
    emit activityChanged(
        QStringLiteral("已将快捷指令“%1”填入发送输入框").arg(fromUtf8(command.name)));
}

void SerialWorkspace::refreshAtCommandChoices()
{
    if (shortcutSidebar_) shortcutSidebar_->setCommands(atCommandLibrary_.commands());
    updateGlobalControls();
}

bool SerialWorkspace::saveAtCommands()
{
    std::string error;
    if (!testtools::AtCommandStore::save(atCommandStorePath_, atCommandLibrary_, error)) {
        QMessageBox::warning(
            this,
            QStringLiteral("\u6307\u4ee4\u672a\u4fdd\u5b58"),
            QStringLiteral("%1\n\u672c\u6b21\u4fee\u6539\u4ecd\u53ef\u5728\u5f53\u524d\u4f1a\u8bdd\u4f7f\u7528\u3002").arg(fromUtf8(error)));
        return false;
    }
    return true;
}

void SerialWorkspace::importAtCommands()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入并替换快捷指令库"),
                                                     {}, QStringLiteral("指令库 (*.json)"));
    if (path.isEmpty()) return;
    auto imported = atCommandLibrary_;
    std::string error;
    if (!testtools::AtCommandStore::importCommands(std::filesystem::path(path.toStdWString()), imported, error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), fromUtf8(error));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("替换快捷指令库"),
            QStringLiteral("将当前 %1 条指令替换为文件中的 %2 条。\n当前串口行尾保持不变，不会发送指令。")
                .arg(atCommandLibrary_.commands().size()).arg(imported.commands().size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    // 先落盘再替换内存中的指令库，确保写盘失败时原内存状态与磁盘均保持不变。
    if (!testtools::AtCommandStore::save(atCommandStorePath_, imported, error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), fromUtf8(error));
        return;
    }
    atCommandLibrary_ = std::move(imported);
    refreshAtCommandChoices();
    emit activityChanged(QStringLiteral("已导入 %1 条快捷指令，当前行尾保持不变")
                             .arg(atCommandLibrary_.commands().size()));
}

void SerialWorkspace::exportAtCommands()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出快捷指令库"),
                        QStringLiteral("serial-commands.json"), QStringLiteral("指令库 (*.json)"));
    if (path.isEmpty()) return;
    std::string error;
    if (!testtools::AtCommandStore::save(std::filesystem::path(path.toStdWString()), atCommandLibrary_, error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), fromUtf8(error));
        return;
    }
    emit activityChanged(QStringLiteral("已导出快捷指令库：%1").arg(path));
}

void SerialWorkspace::setControlLine(int index, SerialControlLine line, bool enabled)
{
    auto &channel = channels_[index];
    auto *check = line == SerialControlLine::Dtr ? channel.dtrCheck : channel.rtsCheck;
    check->setTristate(false);
    const auto generation = channel.session.generation();
    channel.controlPending = true;
    updateGlobalControls();
    QPointer<SerialWorkspace> guard(this);
    auto *session = &channel.session;
    controlTasks_.start([guard, session, index, generation, line, enabled] {
        std::string error;
        const bool succeeded = session->setControlLine(generation, line, enabled, error);
        QMetaObject::invokeMethod(guard, [guard, index, generation, line, enabled, succeeded, error] {
            if (!guard || guard->shuttingDown_ || guard->channels_[index].session.generation() != generation) return;
            auto &current = guard->channels_[index];
            const int lineIndex = line == SerialControlLine::Dtr ? 0 : 1;
            auto *control = lineIndex == 0 ? current.dtrCheck : current.rtsCheck;
            if (succeeded) current.lineStates[lineIndex] = enabled ? Qt::Checked : Qt::Unchecked;
            const QSignalBlocker blocker(control);
            control->setCheckState(current.lineStates[lineIndex]);
            current.controlPending = false;
            guard->updateGlobalControls();
            emit guard->activityChanged(succeeded
                ? QStringLiteral("%1：%2 已%3（驱动已接受）")
                    .arg(guard->channelDisplayName(index), lineIndex == 0 ? QStringLiteral("DTR") : QStringLiteral("RTS"),
                         enabled ? QStringLiteral("置位") : QStringLiteral("清除"))
                : QStringLiteral("控制线未修改：%1").arg(fromUtf8(error)));
        }, Qt::QueuedConnection);
    });
}

void SerialWorkspace::showAtCommandManager()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("atCommandDialog"));
    dialog.setWindowTitle(QStringLiteral("管理快捷指令"));
    dialog.resize(660, 420);
    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 16, 16, 14);
    root->setSpacing(12);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);
    auto *commandList = new QListWidget(&dialog);
    commandList->setMinimumWidth(190);
    body->addWidget(commandList, 1);

    auto *editor = new QWidget(&dialog);
    auto *editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(10);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(12);
    auto *nameEdit = new QLineEdit(editor);
    nameEdit->setMaxLength(40);
    nameEdit->setPlaceholderText(QStringLiteral("例如：信号质量 / 握手帧"));
    auto *hexCheck = new QCheckBox(QStringLiteral("HEX 十六进制格式（按原始字节发送，不附加行尾）"), editor);
    auto *commandEdit = new QPlainTextEdit(editor);
    commandEdit->setFixedHeight(96);
    commandEdit->setPlaceholderText(QStringLiteral("例如：AT+CSQ 或 55 AA 01 0D 0A"));
    form->addRow(QStringLiteral("名称"), nameEdit);
    form->addRow(QStringLiteral("格式"), hexCheck);
    form->addRow(QStringLiteral("指令内容"), commandEdit);
    editorLayout->addLayout(form);
    auto *hint = new QLabel(
        QStringLiteral("文本指令发送时使用主页发送数据区的行尾设置；HEX 指令直接发送原始字节。"), editor);
    hint->setObjectName(QStringLiteral("fieldLabel"));
    hint->setWordWrap(true);
    editorLayout->addWidget(hint);
    editorLayout->addStretch();

    auto *actions = new QHBoxLayout;
    auto *newButton = new QPushButton(QStringLiteral("新建"), editor);
    auto *deleteButton = new QPushButton(QStringLiteral("删除"), editor);
    auto *saveButton = new QPushButton(QStringLiteral("保存指令"), editor);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    actions->addWidget(newButton);
    actions->addWidget(deleteButton);
    actions->addStretch();
    actions->addWidget(saveButton);
    editorLayout->addLayout(actions);
    body->addWidget(editor, 2);
    root->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("完成"));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::function<void(int)> populateForm = [&](int row) {
        const auto& commands = atCommandLibrary_.commands();
        if (row < 0 || row >= static_cast<int>(commands.size())) {
            nameEdit->clear();
            hexCheck->setChecked(false);
            commandEdit->clear();
            deleteButton->setEnabled(false);
            return;
        }
        const auto& command = commands[static_cast<std::size_t>(row)];
        nameEdit->setText(fromUtf8(command.name));
        hexCheck->setChecked(command.hex);
        commandEdit->setPlainText(fromUtf8(command.command));
        deleteButton->setEnabled(true);
    };
    std::function<void(int)> refreshList = [&](int selectedRow) {
        const QSignalBlocker blocker(commandList);
        commandList->clear();
        for (const auto& command : atCommandLibrary_.commands()) {
            const QString itemLabel =
                command.hex
                    ? QStringLiteral("[HEX] %1").arg(fromUtf8(command.name))
                    : fromUtf8(command.name);
            commandList->addItem(itemLabel);
        }
        if (selectedRow >= 0 && selectedRow < commandList->count())
            commandList->setCurrentRow(selectedRow);
        else
            commandList->setCurrentRow(commandList->count() > 0 ? 0 : -1);
        populateForm(commandList->currentRow());
    };

    connect(commandList,
            &QListWidget::currentRowChanged,
            &dialog,
            [&](int row) { populateForm(row); });
    connect(newButton, &QPushButton::clicked, &dialog, [&] {
        commandList->setCurrentRow(-1);
        populateForm(-1);
        nameEdit->setFocus();
    });
    connect(saveButton, &QPushButton::clicked, &dialog, [&] {
        testtools::AtCommand command;
        command.name = toUtf8(nameEdit->text().trimmed());
        command.command = toUtf8(commandEdit->toPlainText().trimmed());
        command.hex = hexCheck->isChecked();
        std::string error;
        const int row = commandList->currentRow();
        const bool saved = row >= 0
                               ? atCommandLibrary_.update(
                                     static_cast<std::size_t>(row), std::move(command), error)
                               : atCommandLibrary_.add(std::move(command), error);
        if (!saved) {
            QMessageBox::information(&dialog,
                                     QStringLiteral("无法保存指令"),
                                     fromUtf8(error));
            return;
        }
        const int selected = row >= 0 ? row
                                      : static_cast<int>(atCommandLibrary_.commands().size()) - 1;
        saveAtCommands();
        refreshAtCommandChoices();
        refreshList(selected);
    });
    connect(deleteButton, &QPushButton::clicked, &dialog, [&] {
        const int row = commandList->currentRow();
        if (row < 0)
            return;
        if (QMessageBox::question(
                &dialog,
                QStringLiteral("删除快捷指令"),
                QStringLiteral("确定删除“%1”吗？").arg(commandList->currentItem()->text())) !=
            QMessageBox::Yes) {
            return;
        }
        atCommandLibrary_.remove(static_cast<std::size_t>(row));
        saveAtCommands();
        refreshAtCommandChoices();
        refreshList(std::min(row, commandList->count() - 1));
    });

    const int selected = shortcutSidebar_ ? shortcutSidebar_->currentIndex() : 0;
    refreshList(selected);
    dialog.exec();
}

void SerialWorkspace::toggleAutoSend(int index)
{
    auto& channel = channels_[index];
    if (channel.session.snapshot().scheduledSend.running) {
        stopAutoSend(index, true);
        return;
    }
    bool ok = false;
    auto payload = manualPayload(index, ok);
    if (!ok) {
        return;
    }
    if (payload.empty()) {
        QMessageBox::information(
            this, QStringLiteral("没有发送内容"), QStringLiteral("请输入要发送的数据。"));
        return;
    }
    if (!channel.session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接该端点。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(channelDisplayName(index) + QStringLiteral(" · 定时 / 次数发送"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(channel.autoIntervalMs);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    auto* countSpin = new QSpinBox(&dialog);
    countSpin->setRange(0, 1000000);
    countSpin->setValue(static_cast<int>(channel.autoTargetCount));
    countSpin->setSpecialValueText(QStringLiteral("持续"));
    auto* echoCheck = new QCheckBox(QStringLiteral("在收发监视中记录每次 TX"), &dialog);
    echoCheck->setChecked(channel.autoEcho);
    echoCheck->setToolTip(QStringLiteral("高频或长时间发送时可关闭，减少界面刷新开销"));
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    form->addRow(QStringLiteral("发送次数"), countSpin);
    form->addRow(QString(), echoCheck);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    channel.autoIntervalMs = intervalSpin->value();
    channel.autoTargetCount = static_cast<std::uint64_t>(countSpin->value());
    channel.autoEcho = echoCheck->isChecked();

    PeriodicSendConfig config;
    config.interval = std::chrono::milliseconds(channel.autoIntervalMs);
    config.targetCount = channel.autoTargetCount;
    if (!channel.session.startScheduledSend(
            std::move(payload), config, channel.autoEcho ? 1u : 0u)) {
        QMessageBox::warning(
            this, QStringLiteral("参数无效"), QStringLiteral("发送间隔必须大于 0。"));
        return;
    }
    channel.autoSendButton->setText(QStringLiteral("停止计划发送"));
    channel.sendEdit->setEnabled(false);
    channel.sendHexCheck->setEnabled(false);
    channel.lineEndingCombo->setEnabled(false);
    emit activityChanged(QStringLiteral("%1 已开始定时 / 次数发送").arg(channelDisplayName(index)));
    updateGlobalControls();
}

void SerialWorkspace::stopAutoSend(int index, bool announce)
{
    auto& channel = channels_[index];
    const auto summary = channel.session.stopScheduledSend();
    if (channel.autoSendButton) {
        channel.autoSendButton->setText(QStringLiteral("定时 / 次数…"));
        channel.sendEdit->setEnabled(true);
        channel.sendHexCheck->setEnabled(true);
        channel.lineEndingCombo->setEnabled(true);
    }
    if (announce && (summary.running || summary.attempts != 0)) {
        emit activityChanged(QStringLiteral("%1 计划发送结束：已提交 %2 / 尝试 %3")
                                 .arg(channelDisplayName(index))
                                 .arg(summary.submitted)
                                 .arg(summary.attempts));
    }
    updateGlobalControls();
}

void SerialWorkspace::processScheduledSends()
{
    if (shuttingDown_) {
        return;
    }
    const auto now = PeriodicSendPlan::Clock::now();
    for (int index = 0; index < 2; ++index) {
        const auto result = channels_[index].session.processScheduledSend(now);
        if (result == testtools::ScheduledSendTick::Failed) {
            setChannelStatus(index, QStringLiteral("计划发送失败，连接或发送队列不可用"), true);
            stopAutoSend(index, true);
        } else if (result == testtools::ScheduledSendTick::Completed) {
            stopAutoSend(index, true);
        }
    }

    if (!stressTestRunning_ || stressAwaitingReceive_) {
        return;
    }
    bool allSent = true;
    bool queuesDrained = true;
    for (auto& direction : stressDirections_) {
        if (!direction.active) {
            continue;
        }
        if (direction.sendPlan.isDue(now)) {
            const auto frame = encodeIntegrityFrame(direction.nextSequence, direction.payloadSize);
            const bool sent = sendBytes(direction.transmitter, frame, false);
            direction.sendPlan.recordAttempt(sent, now);
            if (!sent) {
                stopStressTest(QStringLiteral("可靠性压测发送失败"));
                return;
            }
            ++direction.nextSequence;
        }
        allSent = allSent && !direction.sendPlan.isRunning();
        queuesDrained =
            queuesDrained && channels_[direction.transmitter].session.snapshot().pendingWrites == 0;
    }

    if (allSent && queuesDrained) {
        stressAwaitingReceive_ = true;
        const std::uint64_t generation = stressGeneration_;
        emit activityChanged(QStringLiteral("压测帧已发送完成，正在等待链路尾部数据"));
        QTimer::singleShot(1500, this, [this, generation] { finishStressTest(generation); });
    }
}

void SerialWorkspace::appendTransmitted(int index, const ByteBuffer &data)
{
    if (data.empty()) {
        return;
    }
    auto &channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendTx(
        data,
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialWorkspace::appendReceived(int index, const ByteBuffer& data)
{
    auto& channel = channels_[index];

    if (stressTestRunning_) {
        for (auto& direction : stressDirections_) {
            if (direction.active && direction.receiver == index) {
                direction.verifier.push(data);
            }
        }
        updateCounter(index);
        return;
    }

    if (bidirectionalTestRunning_ && !channel.expectedFrame.empty()) {
        // 委托核心层执行滑动缓冲拼接与令牌匹配，命中后自动清空 expectedFrame。
        if (appendAndMatchFrame(channel.detectionBuffer, data, channel.expectedFrame)) {
            if (index == 0) {
                bToAPassed_ = true;
            } else {
                aToBPassed_ = true;
            }
            updateBidirectionalResult();
        }
    }

    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendRx(
        data,
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    if (index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal) {
        terminalBuffer_.push(data, toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    }
    applyLogAppendResult(index, result);
    updateCounter(index);
}

void SerialWorkspace::appendNotice(int index, const QString& message)
{
    auto& channel = channels_[index];
    const auto now = QDateTime::currentDateTime();
    const auto result = channel.logBuffer.appendNotice(
        toUtf8(message),
        now.toMSecsSinceEpoch(),
        toUtf8(now.toString(QStringLiteral("HH:mm:ss.zzz"))));
    applyLogAppendResult(index, result);
}

void SerialWorkspace::scheduleSearchRefresh(int index)
{
    auto &channel = channels_[index];
    if (channel.searchEdit->text().isEmpty()) return;
    channel.filteredRenderPending = channel.filterCheck->isChecked();
    if (!channel.searchTimer->isActive()) channel.searchTimer->start();
}

void SerialWorkspace::updateSearchMatches(int index)
{
    auto &channel = channels_[index];
    channel.searchMatches.clear();
    const QString query = channel.searchEdit->text();
    if (!query.isEmpty()) {
        const QString text = channel.receiveEdit->toPlainText();
        qsizetype position = 0;
        while ((position = text.indexOf(query, position, Qt::CaseSensitive)) >= 0) {
            channel.searchMatches.push_back(static_cast<int>(position));
            position += query.size();
        }
    }
    const int count = static_cast<int>(channel.searchMatches.size());
    channel.currentMatch = std::min(channel.currentMatch, count - 1);
    channel.matchCountLabel->setText(QStringLiteral("%1 / %2").arg(channel.currentMatch + 1).arg(count));
    channel.previousMatchButton->setEnabled(count > 0);
    channel.nextMatchButton->setEnabled(count > 0);
    QList<QTextEdit::ExtraSelection> selections;
    if (channel.currentMatch >= 0) {
        QTextEdit::ExtraSelection match;
        match.cursor = QTextCursor(channel.receiveEdit->document());
        match.cursor.setPosition(channel.searchMatches[static_cast<std::size_t>(channel.currentMatch)]);
        match.cursor.setPosition(match.cursor.position() + static_cast<int>(query.size()), QTextCursor::KeepAnchor);
        match.format.setBackground(QColor(QStringLiteral("#f0d8c8")));
        match.format.setForeground(QColor(QStringLiteral("#302f2d")));
        selections.push_back(match);
    }
    channel.receiveEdit->setExtraSelections(selections);
}

void SerialWorkspace::findLogMatch(int index, int step)
{
    auto &channel = channels_[index];
    if (channel.filteredRenderPending) renderReceiveLog(index);
    else updateSearchMatches(index);
    const int count = static_cast<int>(channel.searchMatches.size());
    if (count == 0) return;
    channel.currentMatch = channel.currentMatch < 0 ? (step > 0 ? 0 : count - 1)
                           : (channel.currentMatch + step + count) % count;
    channel.autoScrollCheck->setChecked(false);
    QTextCursor cursor(channel.receiveEdit->document());
    cursor.setPosition(channel.searchMatches[static_cast<std::size_t>(channel.currentMatch)]);
    cursor.setPosition(cursor.position() + static_cast<int>(channel.searchEdit->text().size()), QTextCursor::KeepAnchor);
    channel.receiveEdit->setTextCursor(cursor);
    channel.receiveEdit->ensureCursorVisible();
    updateSearchMatches(index);
}

void SerialWorkspace::scheduleTerminalRender()
{
    if (!terminalRenderTimer_->isActive()) terminalRenderTimer_->start();
}

void SerialWorkspace::renderTerminal(bool fullRedraw)
{
    auto& channel = channels_[1];
    auto* edit = channel.receiveEdit;
    auto* scroll = edit->verticalScrollBar();
    const int previousScroll = scroll->value();
    const bool follow = channel.autoScrollCheck->isChecked() && !edit->textCursor().hasSelection();
    const bool timestamps = channel.timestampCheck->isChecked();
    const auto& lines = terminalBuffer_.lines();
    channel.filteredRenderPending = false;
    updateTruncation(1);
    if (!fullRedraw && !terminalBuffer_.dirty()) return;

    // 搜索仅高亮终端历史匹配项，不从终端屏幕裁剪未匹配行。
    std::size_t first = terminalBuffer_.firstChangedLine();
    const auto removed = terminalBuffer_.removedLines();
    const bool rebuild = fullRedraw || terminalBuffer_.needsFullRedraw() ||
                         removed >= std::size_t(edit->document()->blockCount());
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    if (!rebuild && removed) {
        cursor.setPosition(edit->document()->findBlockByNumber(int(removed)).position());
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    const auto block = edit->document()->findBlockByNumber(static_cast<int>(first));
    if (rebuild || !block.isValid()) {
        cursor.select(QTextCursor::Document);
        cursor.removeSelectedText();
        first = 0;
    } else {
        cursor.setPosition(block.position());
        cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
    }
    bool insertedLine = false;
    for (std::size_t i = first; i < lines.size(); ++i) {
        const auto& line = lines[i];
        const auto prefix = timestamps && !line.timestamp.empty() ? "[" + line.timestamp + "] " : std::string{};
        if (insertedLine) cursor.insertText(QStringLiteral("\n"), QTextCharFormat{});
        insertedLine = true;
        if (!prefix.empty()) cursor.insertText(fromUtf8(prefix), logEntryFormat(testtools::SerialLogDirection::Notice));
        for (std::size_t cell = 0; cell < line.cells.size();) {
            const auto& style = line.cells[cell].style;
            std::u32string text;
            do { text += line.cells[cell++].character; }
            while (cell < line.cells.size() && line.cells[cell].style == style);
            QTextCharFormat format;
            auto foreground = style.foreground == testtools::TerminalStyle::DefaultColor ? 0x302f2du : style.foreground;
            auto background = style.background == testtools::TerminalStyle::DefaultColor ? 0xfaf9f7u : style.background;
            if (style.inverse) std::swap(foreground, background);
            format.setForeground(QColor::fromRgb(foreground));
            format.setBackground(QColor::fromRgb(background));
            format.setFontWeight(style.bold ? QFont::Bold : QFont::Normal);
            format.setFontUnderline(style.underline);
            cursor.insertText(QString::fromUcs4(text.data(), static_cast<qsizetype>(text.size())), format);
        }
    }
    cursor.endEditBlock();
    terminalBuffer_.acknowledgeChanges();
    updateTerminalCursor(follow);
    updateSearchMatches(1);
    const int retainedScroll = rebuild ? previousScroll
        : std::max(0, previousScroll - int(removed));
    scroll->setValue(follow ? scroll->maximum() : std::min(retainedScroll, scroll->maximum()));
}

void SerialWorkspace::renderReceiveLog(int index)
{
    if (index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal) {
        renderTerminal(true);
        return;
    }
    auto& channel = channels_[index];
    auto* edit = channel.receiveEdit;
    if (!edit) {
        return;
    }
    auto* vbar = edit->verticalScrollBar();
    const int previousScroll = vbar ? vbar->value() : 0;
    const bool atBottom = vbar ? (previousScroll >= vbar->maximum() - 4) : true;
    const bool autoScroll = !channel.autoScrollCheck || channel.autoScrollCheck->isChecked();
    const bool hex = channel.receiveHexCheck && channel.receiveHexCheck->isChecked();
    const bool showTimestamp = channel.timestampCheck && channel.timestampCheck->isChecked();

    channel.filteredRenderPending = false;
    const auto filter = channel.filterCheck->isChecked() ? toUtf8(channel.searchEdit->text()) : std::string{};
    edit->clear();
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    for (const auto &entry : channel.logBuffer.entries()) {
        const auto text = testtools::SerialLogBuffer::formatEntry(entry, hex, showTimestamp);
        if (filter.empty() || text.find(filter) != std::string::npos) {
            cursor.insertText(fromUtf8(text), logEntryFormat(entry.direction));
        }
    }
    cursor.endEditBlock();
    updateTruncation(index);
    updateSearchMatches(index);
    if (vbar) {
        if (autoScroll || atBottom) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(std::min(previousScroll, vbar->maximum()));
        }
    }
}

void SerialWorkspace::applyLogAppendResult(int index,
                                           const testtools::SerialLogAppendResult& result)
{
    auto& batch = channels_[index];
    if (batch.receivingBatch) {
        batch.receivedAppend.addedEntries += result.addedEntries;
        batch.receivedAppend.trimmedOldest = batch.receivedAppend.trimmedOldest || result.trimmedOldest;
        return;
    }
    if (index == 1 && workspaceState_.mode() == WorkspaceMode::Terminal) {
        scheduleTerminalRender();
        return;
    }
    auto &current = channels_[index];
    updateTruncation(index);
    scheduleSearchRefresh(index);
    if (current.filterCheck->isChecked() && !current.searchEdit->text().isEmpty()) {
        return; // 过滤模式下由单次定时器合并刷新突发日志，底层原始缓冲已先行更新。
    }
    if (result.addedEntries == 0) {
        return;
    }
    if (result.trimmedOldest) {
        renderReceiveLog(index);
        return;
    }
    auto& channel = channels_[index];
    auto* edit = channel.receiveEdit;
    auto* vbar = edit->verticalScrollBar();
    const int previousScroll = vbar ? vbar->value() : 0;
    const bool autoScroll = !channel.autoScrollCheck || channel.autoScrollCheck->isChecked();
    const bool hex = channel.receiveHexCheck && channel.receiveHexCheck->isChecked();
    const bool showTimestamp = channel.timestampCheck && channel.timestampCheck->isChecked();

    const auto& entries = channel.logBuffer.entries();
    QTextCursor cursor(edit->document());
    const std::size_t startAdded =
        entries.size() >= result.addedEntries ? (entries.size() - result.addedEntries) : 0;
    for (std::size_t i = startAdded; i < entries.size(); ++i) {
        const QString line =
            fromUtf8(testtools::SerialLogBuffer::formatEntry(entries[i], hex, showTimestamp));
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(line, logEntryFormat(entries[i].direction));
    }
    if (vbar) {
        if (autoScroll) {
            vbar->setValue(vbar->maximum());
        } else {
            vbar->setValue(previousScroll);
        }
    }
}

void SerialWorkspace::updateCounter(int index)
{
    auto& channel = channels_[index];
    if (channel.receivingBatch) return;
    if (!channel.counterLabel) {
        return;
    }
    const auto snapshot = channel.session.snapshot();
    if (workspaceState_.mode() != WorkspaceMode::Dual) {
        channel.counterLabel->setText(
            QStringLiteral("RX: %1 B  |  TX: %2 B").arg(snapshot.receivedBytes).arg(snapshot.sentBytes));
    } else {
        channel.counterLabel->setText(
            QStringLiteral("RX %1 · TX %2 B").arg(snapshot.receivedBytes).arg(snapshot.sentBytes));
    }
}

void SerialWorkspace::resetChannelCounters(int index)
{
    channels_[index].session.resetCounters();
    updateCounter(index);
    emit activityChanged(
        QStringLiteral("%1 的 RX / TX 计数已清零").arg(channelDisplayName(index)));
}

void SerialWorkspace::saveChannelLog(int index)
{
    auto& channel = channels_[index];
    const QString text = channel.receiveEdit->toPlainText();
    if (text.trimmed().isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("暂无日志"), QStringLiteral("当前收发记录为空，无需导出。"));
        return;
    }
    const QString defaultName =
        QStringLiteral("serial-%1-%2.log")
            .arg(index == 0 ? QStringLiteral("A") : QStringLiteral("B"),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const QString filePath = QFileDialog::getSaveFileName(
        this,
        channelDisplayName(index) + QStringLiteral(" · 保存收发日志"),
        defaultName,
        QStringLiteral("日志文件 (*.log *.txt);;所有文件 (*.*)"));
    if (filePath.isEmpty()) {
        return;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(
            this, QStringLiteral("保存失败"), QStringLiteral("无法写入文件：%1").arg(filePath));
        return;
    }
    const QByteArray utf8 = text.toUtf8();
    if (file.write(utf8) != utf8.size()) {
        QMessageBox::warning(
            this, QStringLiteral("保存失败"), QStringLiteral("写入日志文件不完整：%1").arg(filePath));
        return;
    }
    file.close();
    emit activityChanged(QStringLiteral("已保存收发日志：%1").arg(filePath));
}

void SerialWorkspace::runBidirectionalTest()
{
    if (!channels_[0].session.isOpen() || !channels_[1].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接端点 A 和端点 B。"));
        return;
    }

    const std::string token = std::to_string(QDateTime::currentMSecsSinceEpoch());
    const std::string frameA = "SERIAL_POC_A_TO_B_" + token;
    const std::string frameB = "SERIAL_POC_B_TO_A_" + token;
    channels_[1].expectedFrame = frameA;
    channels_[0].expectedFrame = frameB;
    channels_[0].detectionBuffer.clear();
    channels_[1].detectionBuffer.clear();
    aToBPassed_ = false;
    bToAPassed_ = false;
    bidirectionalTestRunning_ = true;
    const std::uint64_t generation = ++testGeneration_;
    updateBidirectionalResult();
    updateGlobalControls();

    ByteBuffer bytesA(frameA.begin(), frameA.end());
    ByteBuffer bytesB(frameB.begin(), frameB.end());
    appendLineEnding(bytesA, lineEndingFromIndex(channels_[0].lineEndingCombo->currentIndex()));
    appendLineEnding(bytesB, lineEndingFromIndex(channels_[1].lineEndingCombo->currentIndex()));
    if (!sendBytes(0, bytesA) || !sendBytes(1, bytesB)) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(QStringLiteral("双向收发验证 · 发送失败"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
        updateGlobalControls();
        return;
    }

    QPointer<SerialWorkspace> guard(this);
    QTimer::singleShot(3000, this, [guard, generation] {
        if (guard && guard->bidirectionalTestRunning_ && guard->testGeneration_ == generation) {
            guard->updateBidirectionalResult(true);
        }
    });
}

void SerialWorkspace::updateBidirectionalResult(bool timedOut)
{
    if (aToBPassed_ && bToAPassed_) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(QStringLiteral("双向收发验证 · A→B PASS  /  B→A PASS"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusGood"));
    } else if (timedOut) {
        bidirectionalTestRunning_ = false;
        testResultLabel_->setText(
            QStringLiteral("双向收发验证超时 · A→B %1  /  B→A %2")
                .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
    } else {
        testResultLabel_->setText(
            QStringLiteral("正在验证 · A→B %1  /  B→A %2")
                .arg(aToBPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待"))
                .arg(bToAPassed_ ? QStringLiteral("PASS") : QStringLiteral("等待")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusWorking"));
    }
    updateGlobalControls();
}

void SerialWorkspace::showStressDialog()
{
    if (!channels_[0].session.isOpen() || !channels_[1].session.isOpen()) {
        QMessageBox::information(this,
                                 QStringLiteral("尚未连接"),
                                 QStringLiteral("可靠性压测需要先连接端点 A 和端点 B。"));
        return;
    }
    if (channelConfig(0).dataBits != 8 || channelConfig(1).dataBits != 8) {
        QMessageBox::information(
            this,
            QStringLiteral("串口格式不适合压测"),
            QStringLiteral(
                "可靠性压测帧包含完整 8 bit 二进制数据，请先将两个端点的数据位都设为 8。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("串口可靠性压测"));
    dialog.setMinimumWidth(420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral(
            "发送带序号、确定性载荷和 CRC32 的测试帧。BER 只统计已解析载荷，丢帧单独报告。"),
        &dialog);
    description->setWordWrap(true);
    description->setObjectName(QStringLiteral("cardBody"));
    layout->addWidget(description);

    auto *form = new QFormLayout;
    auto *directionCombo = new QComboBox(&dialog);
    directionCombo->addItems({QStringLiteral("A → B"), QStringLiteral("B → A"),
                              QStringLiteral("双向同时")});
    auto *frameCountSpin = new QSpinBox(&dialog);
    frameCountSpin->setRange(1, 1000000);
    frameCountSpin->setValue(1000);
    frameCountSpin->setSuffix(QStringLiteral(" 帧"));
    auto *payloadSizeSpin = new QSpinBox(&dialog);
    payloadSizeSpin->setRange(1, static_cast<int>(IntegrityStreamVerifier::MaximumPayloadSize));
    payloadSizeSpin->setValue(256);
    payloadSizeSpin->setSuffix(QStringLiteral(" B"));
    auto *intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(10);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    form->addRow(QStringLiteral("方向"), directionCombo);
    form->addRow(QStringLiteral("每方向帧数"), frameCountSpin);
    form->addRow(QStringLiteral("有效载荷"), payloadSizeSpin);
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始压测"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        startStressTest(directionCombo->currentIndex(),
                        static_cast<std::uint64_t>(frameCountSpin->value()),
                        static_cast<std::size_t>(payloadSizeSpin->value()),
                        intervalSpin->value());
    }
}

void SerialWorkspace::startStressTest(int direction, std::uint64_t frameCount,
                                      std::size_t payloadSize, int intervalMs)
{
    stopAutoSend(0);
    stopAutoSend(1);
    stressDirections_ = {};

    // 委托核心层根据串口线速参数推算单帧最小发送间隔，防止后台写队列无限堆积。
    int effectiveIntervalMs = intervalMs;
    if (direction == 0 || direction == 2) {
        effectiveIntervalMs =
            minimumStressIntervalMs(channelConfig(0), payloadSize, effectiveIntervalMs);
    }
    if (direction == 1 || direction == 2) {
        effectiveIntervalMs =
            minimumStressIntervalMs(channelConfig(1), payloadSize, effectiveIntervalMs);
    }
    if (effectiveIntervalMs > intervalMs) {
        QMessageBox::information(
            this, QStringLiteral("已按串口线速调整"),
            QStringLiteral("当前帧在所选波特率下至少需要约 %1 ms，实际发送间隔已由 %2 ms 调整为 %1 ms，避免后台队列堆积。")
                .arg(effectiveIntervalMs).arg(intervalMs));
    }

    const auto activate = [this, frameCount, payloadSize, effectiveIntervalMs](int slot, int tx, int rx) {
        auto &testDirection = stressDirections_[slot];
        testDirection.active = true;
        testDirection.transmitter = tx;
        testDirection.receiver = rx;
        testDirection.payloadSize = payloadSize;
        testDirection.nextSequence = 0;
        testDirection.verifier.reset();
        testDirection.sendPlan.start({std::chrono::milliseconds(effectiveIntervalMs), frameCount});
    };
    if (direction == 0 || direction == 2) {
        activate(0, 0, 1);
    }
    if (direction == 1) {
        activate(0, 1, 0);
    } else if (direction == 2) {
        activate(1, 1, 0);
    }

    stressTestRunning_ = true;
    stressAwaitingReceive_ = false;
    ++stressGeneration_;
    stressResultLabel_->setText(QStringLiteral("可靠性压测 · 正在发送"));
    setVisualRole(stressResultLabel_, QStringLiteral("testStatusWorking"));
    for (int index = 0; index < 2; ++index) {
        appendNotice(index,
                     QStringLiteral("— 可靠性压测数据流已折叠，仅更新字节计数与统计结果 —"));
    }
    emit activityChanged(QStringLiteral("串口可靠性压测已开始"));
    updateGlobalControls();
}

void SerialWorkspace::stopStressTest(const QString &reason, bool completed)
{
    if (!stressTestRunning_) {
        return;
    }

    bool passed = completed;
    QStringList summaries;
    for (auto &direction : stressDirections_) {
        if (!direction.active) {
            continue;
        }
        const auto report = evaluateStressDirection(direction.sendPlan, direction.verifier);
        passed = passed && report.passed;
        summaries.push_back(stressDirectionSummary(direction));
        direction.sendPlan.stop();
    }

    stressTestRunning_ = false;
    stressAwaitingReceive_ = false;
    if (completed) {
        stressResultLabel_->setText(QStringLiteral("可靠性压测 · %1\n%2")
                                        .arg(passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                                             summaries.join(QLatin1Char('\n'))));
        setVisualRole(stressResultLabel_, passed ? QStringLiteral("testStatusGood")
                                                 : QStringLiteral("testStatusError"));
    } else {
        stressResultLabel_->setText(QStringLiteral("可靠性压测 · 已停止\n%1")
                                        .arg(summaries.join(QLatin1Char('\n'))));
        setVisualRole(stressResultLabel_, QStringLiteral("testStatusNeutral"));
    }
    emit activityChanged(reason);
    updateGlobalControls();
}

void SerialWorkspace::finishStressTest(std::uint64_t generation)
{
    if (!stressTestRunning_ || generation != stressGeneration_) {
        return;
    }
    stopStressTest(QStringLiteral("串口可靠性压测已完成"), true);
}

QString SerialWorkspace::stressDirectionSummary(const StressDirection &direction) const
{
    const auto report = evaluateStressDirection(direction.sendPlan, direction.verifier);
    return QStringLiteral("%1 → %2   接收 %3 / %4 帧   丢帧 %5   坏帧 %6   格式错误 %7   BER %8")
        .arg(direction.transmitter == 0 ? QStringLiteral("A") : QStringLiteral("B"))
        .arg(direction.receiver == 0 ? QStringLiteral("A") : QStringLiteral("B"))
        .arg(report.uniqueFrames)
        .arg(report.sentFrames)
        .arg(report.lostFrames)
        .arg(report.corruptedFrames)
        .arg(report.framingErrors)
        .arg(QString::number(report.bitErrorRate, 'g', 4));
}

bool SerialWorkspace::hasActiveAgentConnection(const std::string& serial) const
{
    return !serial.empty() && agentAdbSerial_==serial && hasActiveAgentConnection();
}
void SerialWorkspace::setDeviceMaintenance(bool blocked)
{
    if(maintenanceBlocked_==blocked) return;
    maintenanceBlocked_=blocked;
    if(blocked) { ++agentPortGeneration_; scanning_=false; }
    updateGlobalControls();
}
