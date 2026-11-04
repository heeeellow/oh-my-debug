#include "serial_workspace.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include "ui/widgets/at_command_dialog.h"
#include "ui/widgets/serial_connection_bar.h"
#include "ui/widgets/serial_keyword_highlighter.h"
#include "ui/widgets/serial_receive_view.h"
#include "ui/widgets/serial_shortcut_sidebar.h"
#include "ui/widgets/serial_workspace_helpers.h"

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
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>
#include <QtGui/QShortcut>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCharFormat>
#include <QtGui/QTextCursor>
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
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

using namespace testtools;

using namespace testtools::ui::serial;

SerialWorkspace::SerialWorkspace(QWidget* parent,
                                 std::shared_ptr<testtools::SerialPortClaims> claims)
    : SerialEndpointWorkspace(parent, 2, std::move(claims))
{
    atCommandStorePath_ = storageDirectory() / L"at-commands.json";
    std::error_code existsError;
    const bool exists = std::filesystem::exists(atCommandStorePath_, existsError);
    std::string error;
    const bool loaded = !existsError && testtools::AtCommandStore::load(
                                            atCommandStorePath_, atCommandLibrary_, error);
    if (loaded && !exists)
        testtools::AtCommandStore::save(atCommandStorePath_, atCommandLibrary_, error);
    initializePreferences(
        L"serial-preferences.json", false, storedLineEndingIndex(atCommandLibrary_.lineEnding()));
    buildUi();
    applyPreferencesToUi();
    applyWorkspaceMode();
    refreshAtCommandChoices();
    finishSetup();
    if (!error.empty())
        QTimer::singleShot(0, this, [this, error] {
            emit activityChanged(
                QStringLiteral("AT 指令库未能从本地文件恢复：%1").arg(fromUtf8(error)));
        });
}
SerialWorkspace::~SerialWorkspace()
{
    prepareForShutdown();
}
QString SerialWorkspace::currentWorkspaceMode() const
{
    return workspaceState_.mode() == WorkspaceMode::Dual ? QStringLiteral("dual")
                                                         : QStringLiteral("single");
}
bool SerialWorkspace::selectWorkspaceMode(const QString& modeName)
{
    const auto name = modeName.trimmed().toLower();
    if (name != QStringLiteral("single") && name != QStringLiteral("dual"))
        return false;
    const auto mode = name == QStringLiteral("dual") ? WorkspaceMode::Dual : WorkspaceMode::Single;
    setWorkspaceMode(mode);
    return workspaceState_.mode() == mode;
}
void SerialWorkspace::addChannelBody(QVBoxLayout* panelLayout, int index)
{
    auto& channel = channels_[index];
    if (index == 1) {
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
            updateLayout();
            savePreferences();
        };
        connect(shortcutSplitter_, &QSplitter::splitterMoved, this, [this] {
            if (updatingShortcutLayout_ || shortcutSidebar_->isHidden())
                return;
            preferences_.shortcutSidebarWidth = std::clamp(shortcutSidebar_->width(), 220, 480);
            forceShortcutOpen_ = true;
            savePreferences();
            updateLayout();
        });
        panelLayout->addWidget(shortcutSplitter_, 1);
    } else
        panelLayout->addWidget(channel.logSendBody, 1);
}
void SerialWorkspace::buildSendArea(int index,
                                    QWidget* terminalHeader,
                                    QHBoxLayout* receiveHeader,
                                    QVBoxLayout* logSendLayout)
{
    auto& channel = channels_[index];
    channel.manualSendArea = new QWidget(channel.group);
    auto* sendAreaLayout = new QVBoxLayout(channel.manualSendArea);
    sendAreaLayout->setContentsMargins(0, 0, 0, 0);
    sendAreaLayout->setSpacing(6);

    auto* composerFrame = new QFrame(channel.manualSendArea);
    channel.composerFrame = composerFrame;
    composerFrame->setObjectName(QStringLiteral("composerFrame"));
    auto* composerLayout = new QVBoxLayout(composerFrame);
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerLayout->setSpacing(0);

    channel.sendEdit = new QPlainTextEdit(composerFrame);
    channel.sendEdit->setObjectName(QStringLiteral("sendEditor"));
    channel.sendEdit->setFixedHeight(52);
    channel.sendEdit->setPlaceholderText(QStringLiteral("输入要发送的数据"));
    channel.sendEdit->installEventFilter(this);
    composerLayout->addWidget(channel.sendEdit);

    auto* composerFooter = new QFrame(composerFrame);
    composerFooter->setObjectName(QStringLiteral("composerFooter"));
    auto* sendTools = new QHBoxLayout(composerFooter);
    sendTools->setContentsMargins(10, 5, 10, 5);
    sendTools->setSpacing(8);
    channel.sendHexCheck = new QCheckBox(QStringLiteral("HEX 发送"), composerFooter);
    channel.lineEndingCombo = new QComboBox(composerFooter);
    channel.lineEndingCombo->addItems(
        {QStringLiteral("无"), QStringLiteral("LF"), QStringLiteral("CR"), QStringLiteral("CRLF")});
    if (index == 1)
        channel.lineEndingCombo->setCurrentIndex(
            storedLineEndingIndex(atCommandLibrary_.lineEnding()));
    channel.lineEndingCombo->setToolTip(
        QStringLiteral("手动发送和文本快捷指令共用此行尾；切换端口不会改变选择。"));
    channel.lineEndingCombo->setMinimumWidth(84);

    auto* endingLabel = new QLabel(QStringLiteral("行尾"), composerFooter);
    endingLabel->setObjectName(QStringLiteral("fieldLabel"));

    channel.sendShortcutHint =
        new QLabel(QStringLiteral("Enter 发送 · Shift+Enter 换行"), composerFooter);
    channel.sendShortcutHint->setObjectName(QStringLiteral("fieldLabel"));

    channel.toggleAtBarButton = new QPushButton(QStringLiteral("快捷指令"), terminalHeader);
    channel.toggleAtBarButton->setObjectName(QStringLiteral("ghostButton"));
    channel.toggleAtBarButton->setToolTip(QStringLiteral("展开右侧快捷指令列表"));
    receiveHeader->addWidget(channel.toggleAtBarButton);
    channel.receiveMoreButton = new QPushButton(QStringLiteral("更多"), terminalHeader);
    channel.receiveMoreButton->setObjectName(QStringLiteral("ghostButton"));
    auto* toolsMenu = new QMenu(channel.receiveMoreButton);
    toolsMenu->addAction(
        QStringLiteral("计数清零"), channel.resetCounterButton, &QPushButton::click);
    toolsMenu->addAction(
        QStringLiteral("保存当前日志…"), channel.saveLogButton, &QPushButton::click);
    toolsMenu->addSeparator();
    const std::array<QCheckBox*, 3> checks{
        channel.receiveHexCheck, channel.timestampCheck, channel.autoScrollCheck};
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

    connect(channel.toggleAtBarButton, &QPushButton::clicked, this, [this] {
        preferences_.shortcutBarVisible = true;
        forceShortcutOpen_ = true;
        updateLayout();
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
    connect(
        channel.clearSendButton, &QPushButton::clicked, channel.sendEdit, &QPlainTextEdit::clear);
}
void SerialWorkspace::applyWorkspaceMode()
{
    const bool single = singleChannelView();
    pageTitle_->setText(single ? QStringLiteral("单串口调试") : QStringLiteral("双端调试"));
    emit workspaceModeChanged(currentWorkspaceMode());
    channels_[0].group->setVisible(!single);
    channels_[1].group->show();
    applyChannelLayout(single);
    linkBar_->setVisible(!single);
    resultsPanel_->setVisible(!single);
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[index];
        auto* target = single && index == 1 ? channel.settingsRowLayout : channel.headerLayout;
        auto* previous = single && index == 1 ? channel.headerLayout : channel.settingsRowLayout;
        previous->removeWidget(channel.actionsContainer);
        if (target->indexOf(channel.actionsContainer) < 0)
            target->addWidget(channel.actionsContainer);
        channel.headerWidget->setVisible(!single);
        channel.refreshPortsButton->setText(single ? QStringLiteral("刷新串口")
                                                   : QStringLiteral("刷新"));
        channel.portCombo->setMaximumWidth(single ? 320 : 250);
        channel.sendShortcutHint->setVisible(single);
    }
    channels_[0].toggleAtBarButton->hide();
    if (!single)
        channelSplitter_->setSizes({600, 600});
    modeDescriptionLabel_->setText(
        single ? QStringLiteral("连接一个串口，查看 TX / RX 并发送数据")
               : QStringLiteral("配置端点 A / B；独立收发、双向验证和可靠性压测按需使用"));
    updateCounter(0);
    updateCounter(1);
    renderReceiveLog(1);
    updateLayout();
}
bool SerialWorkspace::handleReceiveEvent(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier))
            for (int index = 0; index < 2; ++index)
                if (watched == channels_[index].sendEdit) {
                    sendManual(index);
                    return true;
                }
    }
    return false;
}
void SerialWorkspace::showAtCommandManager()
{
    AtCommandDialog::open(
        this, atCommandLibrary_, shortcutSidebar_ ? shortcutSidebar_->currentIndex() : 0, [this] {
            saveAtCommands();
            refreshAtCommandChoices();
        });
}

void SerialWorkspace::buildUi()
{
    buildPageHeader(QStringLiteral("单串口调试"),
                    QStringLiteral("连接一个串口，查看 TX / RX 并发送数据"));
    auto* rootLayout = static_cast<QVBoxLayout*>(layout());
    linkBar_ = new QFrame(this);
    linkBar_->setObjectName(QStringLiteral("linkBar"));
    auto* linkLayout = new QHBoxLayout(linkBar_);
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
    auto* resultsLayout = new QVBoxLayout(resultsPanel_);
    resultsLayout->setContentsMargins(12, 7, 12, 7);
    resultsLayout->setSpacing(4);
    testResultLabel_ = new QLabel(QStringLiteral("双向收发验证 · 未运行"), resultsPanel_);
    stressResultLabel_ = new QLabel(QStringLiteral("可靠性压测 · 未运行"), resultsPanel_);
    for (auto* label : {testResultLabel_, stressResultLabel_}) {
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
        if (linkVerifier_.stressRunning()) {
            stopStressTest(QStringLiteral("用户已停止可靠性压测"));
        } else {
            showStressDialog();
        }
    });

    scheduleTimer_ = new QTimer(this);
    scheduleTimer_->setTimerType(Qt::PreciseTimer);
    scheduleTimer_->setInterval(10);
    connect(scheduleTimer_, &QTimer::timeout, this, &SerialWorkspace::processScheduledSends);

    installSearchShortcuts();

    updateRouteSummary();
    applyWorkspaceMode();
    updateGlobalControls();
}

void SerialWorkspace::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width())
        forceShortcutOpen_ = false;
    QTimer::singleShot(0, this, [this] { updateLayout(); });
}

void SerialWorkspace::updateLayout()
{
    if (updatingShortcutLayout_ || !channels_[1].receiveMoreButton)
        return;
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
    } else
        channels_[1].toggleAtBarButton->hide();
    channels_[0].toggleAtBarButton->hide();
    for (int index = 0; index < 2; ++index) {
        auto& channel = channels_[index];
        if (!channel.receiveMoreButton)
            continue;
        const bool compact = channel.logSendBody->width() < 1060;
        channel.receiveHexCheck->setVisible(!compact);
        channel.timestampCheck->setVisible(!compact);
        channel.autoScrollCheck->setVisible(!compact);
        channel.receiveMoreButton->setVisible(true);
        channel.resetCounterButton->hide();
        channel.saveLogButton->setVisible(false);
        for (auto* action : channel.displayActions)
            if (action)
                action->setVisible(compact);
        channel.sendShortcutHint->setVisible(single && channel.logSendBody->width() >= 760);
    }
    updatingShortcutLayout_ = false;
}

ByteBuffer SerialWorkspace::manualPayload(int index, bool& ok)
{
    auto& channel = channels_[index];
    ByteBuffer data;
    ok = true;
    const QString input = channel.sendEdit->toPlainText();
    if ((!channel.sendHexCheck->isChecked() && input.isEmpty()) ||
        (channel.sendHexCheck->isChecked() && input.trimmed().isEmpty())) {
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
    if (linkVerifier_.stressRunning() || linkVerifier_.probe().running ||
        channels_[index].session.snapshot().scheduledSend.running)
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
        appendLineEnding(data,
                         lineEndingFromIndex(channels_[index].lineEndingCombo->currentIndex()));
    }
    if (sendBytes(index, data)) {
        emit activityChanged(QStringLiteral("%1 已发送快捷指令：%2")
                                 .arg(channelDisplayName(index), fromUtf8(command.name)));
    }
}

void SerialWorkspace::fillAtCommandToInput(int index, int commandIndex)
{
    if (linkVerifier_.stressRunning() || linkVerifier_.probe().running ||
        channels_[index].session.snapshot().scheduledSend.running)
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
    if (shortcutSidebar_)
        shortcutSidebar_->setCommands(atCommandLibrary_.commands());
    updateGlobalControls();
}

bool SerialWorkspace::saveAtCommands()
{
    std::string error;
    if (!testtools::AtCommandStore::save(atCommandStorePath_, atCommandLibrary_, error)) {
        QMessageBox::warning(this,
                             QStringLiteral("\u6307\u4ee4\u672a\u4fdd\u5b58"),
                             QStringLiteral("%1\n\u672c\u6b21\u4fee\u6539\u4ecd\u53ef\u5728\u5f53"
                                            "\u524d\u4f1a\u8bdd\u4f7f\u7528\u3002")
                                 .arg(fromUtf8(error)));
        return false;
    }
    return true;
}

void SerialWorkspace::importAtCommands()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入并替换快捷指令库"), {}, QStringLiteral("指令库 (*.json)"));
    if (path.isEmpty())
        return;
    auto imported = atCommandLibrary_;
    std::string error;
    if (!testtools::AtCommandStore::importCommands(
            std::filesystem::path(path.toStdWString()), imported, error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), fromUtf8(error));
        return;
    }
    if (QMessageBox::question(
            this,
            QStringLiteral("替换快捷指令库"),
            QStringLiteral(
                "将当前 %1 条指令替换为文件中的 %2 条。\n当前串口行尾保持不变，不会发送指令。")
                .arg(atCommandLibrary_.commands().size())
                .arg(imported.commands().size()),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
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
    const QString path = QFileDialog::getSaveFileName(this,
                                                      QStringLiteral("导出快捷指令库"),
                                                      QStringLiteral("serial-commands.json"),
                                                      QStringLiteral("指令库 (*.json)"));
    if (path.isEmpty())
        return;
    std::string error;
    if (!testtools::AtCommandStore::save(
            std::filesystem::path(path.toStdWString()), atCommandLibrary_, error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), fromUtf8(error));
        return;
    }
    emit activityChanged(QStringLiteral("已导出快捷指令库：%1").arg(path));
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

bool SerialWorkspace::canSwitchWorkspaceMode(WorkspaceMode mode)
{
    testtools::SerialWorkspaceActivity activity;
    for (int index = 0; index < 2; ++index) {
        activity.channelActive[static_cast<std::size_t>(index)] = channelActive(index);
        activity.scheduledSendRunning[static_cast<std::size_t>(index)] =
            channels_[index].session.snapshot().scheduledSend.running;
    }
    activity.bidirectionalTestRunning = linkVerifier_.probe().running;
    activity.reliabilityTestRunning = linkVerifier_.stressRunning();
    const auto decision = workspaceState_.evaluateModeSwitch(mode, activity);
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::TaskRunning) {
        QMessageBox::information(
            this,
            QStringLiteral("任务正在运行"),
            QStringLiteral("请先停止当前的计划发送、双向收发验证或可靠性压测，再切换调试方式。"));
        return false;
    }
    if (decision.blockedBy == testtools::SerialModeSwitchBlock::HiddenChannelActive) {
        QMessageBox::information(this,
                                 QStringLiteral("端点 A 仍在使用"),
                                 QStringLiteral("请先断开端点 A，再切换到单串口调试或串口终端。"));
        return false;
    }
    return true;
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

    auto* form = new QFormLayout;
    auto* directionCombo = new QComboBox(&dialog);
    directionCombo->addItems(
        {QStringLiteral("A → B"), QStringLiteral("B → A"), QStringLiteral("双向同时")});
    auto* frameCountSpin = new QSpinBox(&dialog);
    frameCountSpin->setRange(1, 1000000);
    frameCountSpin->setValue(1000);
    frameCountSpin->setSuffix(QStringLiteral(" 帧"));
    auto* payloadSizeSpin = new QSpinBox(&dialog);
    payloadSizeSpin->setRange(1, static_cast<int>(IntegrityStreamVerifier::MaximumPayloadSize));
    payloadSizeSpin->setValue(256);
    payloadSizeSpin->setSuffix(QStringLiteral(" B"));
    auto* intervalSpin = new QSpinBox(&dialog);
    intervalSpin->setRange(10, 60000);
    intervalSpin->setValue(10);
    intervalSpin->setSuffix(QStringLiteral(" ms"));
    form->addRow(QStringLiteral("方向"), directionCombo);
    form->addRow(QStringLiteral("每方向帧数"), frameCountSpin);
    form->addRow(QStringLiteral("有效载荷"), payloadSizeSpin);
    form->addRow(QStringLiteral("发送间隔"), intervalSpin);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
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

void SerialWorkspace::updatePageControls()
{
    const auto firstSnapshot = channels_[0].session.snapshot();
    const auto secondSnapshot = channels_[1].session.snapshot();
    const bool firstActive = channelActive(0);
    const bool secondActive = channelActive(1);
    const bool anyActive = firstActive || secondActive;
    const bool bothOpen = firstSnapshot.open && secondSnapshot.open;
    const bool anyControlPending = channels_[0].controlPending || channels_[1].controlPending;
    const bool anyAutoSend =
        firstSnapshot.scheduledSend.running || secondSnapshot.scheduledSend.running;
    const bool missingChannelsReady =
        (firstActive || channelReady(0)) && (secondActive || channelReady(1));
    const bool pairCompatible = firstActive || secondActive || !resourceConflict();
    const bool transitioning =
        firstSnapshot.state == testtools::SerialSessionState::Connecting ||
        secondSnapshot.state == testtools::SerialSessionState::Connecting ||
        firstSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        secondSnapshot.state == testtools::SerialSessionState::Disconnecting ||
        channels_[0].disconnectPending || channels_[1].disconnectPending;
    const bool waitingForAgent = scanning_ && ((channelUsesAgent(0) && !firstActive) ||
                                               (channelUsesAgent(1) && !secondActive));
    connectBothButton_->setEnabled(!waitingForAgent && !bothOpen && !transitioning &&
                                   missingChannelsReady && pairCompatible);
    connectBothButton_->setText(bothOpen    ? QStringLiteral("两端已连接")
                                : anyActive ? QStringLiteral("连接另一端")
                                            : QStringLiteral("连接两端"));
    disconnectBothButton_->setEnabled(anyActive && !transitioning);
    bidirectionalTestButton_->setEnabled(bothOpen && !anyControlPending &&
                                         !linkVerifier_.probe().running &&
                                         !linkVerifier_.stressRunning() && !anyAutoSend);
    bidirectionalTestButton_->setText(linkVerifier_.probe().running
                                          ? QStringLiteral("正在验证…")
                                          : QStringLiteral("双向收发验证"));
    stressTestButton_->setEnabled(
        linkVerifier_.stressRunning() ||
        (bothOpen && !anyControlPending && !linkVerifier_.probe().running && !anyAutoSend));
    stressTestButton_->setText(linkVerifier_.stressRunning() ? QStringLiteral("停止可靠性压测")
                                                             : QStringLiteral("可靠性压测…"));

    const auto session = channels_[1].session.snapshot();
    const bool editable = !controlTasksRunning() && !session.scheduledSend.running;
    if (shortcutSidebar_)
        shortcutSidebar_->setActionsEnabled(editable, session.open && editable);
    updateScheduleTimer();
}
void SerialWorkspace::updateScheduleTimer()
{
    if (!scheduleTimer_)
        return;
    bool needed = linkVerifier_.stressRunning() && !linkVerifier_.awaitingReceive();
    for (const auto& channel : channels_)
        needed = needed || channel.session.snapshot().scheduledSend.running;
    if (shuttingDown_ || !needed)
        scheduleTimer_->stop();
    else if (!scheduleTimer_->isActive())
        scheduleTimer_->start();
}
void SerialWorkspace::stopPageTasks()
{
    if (scheduleTimer_)
        scheduleTimer_->stop();
    if (linkVerifier_.stressRunning())
        stopStressTest(QStringLiteral("应用正在退出"));
    linkVerifier_.stopProbe();
}
void SerialWorkspace::channelDisconnected(int index)
{
    stopAutoSend(index);
    if (linkVerifier_.stressRunning())
        stopStressTest(QStringLiteral("串口连接已断开"));
    linkVerifier_.stopProbe();
}
void SerialWorkspace::sendFailed(int index)
{
    stopAutoSend(index, false);
    if (linkVerifier_.stressRunning())
        stopStressTest(QStringLiteral("可靠性压测发送失败"));
}
void SerialWorkspace::displayGap(int)
{
    if (linkVerifier_.stressRunning()) {
        stopStressTest(QStringLiteral("接收显示积压，压测结果不完整"));
        stressResultLabel_->setText(stressResultLabel_->text() +
                                    QStringLiteral("\n接收积压，结果不完整"));
    }
    if (linkVerifier_.probe().running) {
        linkVerifier_.stopProbe();
        testResultLabel_->setText(QStringLiteral("接收积压，双向验证已停止，请重试"));
        updateGlobalControls();
    }
}
void SerialWorkspace::appendReceived(int index, const ByteBuffer& data)
{
    const bool folded = linkVerifier_.stressRunning();
    if (linkVerifier_.receive(index, data))
        updateBidirectionalResult();
    if (folded) {
        updateCounter(index);
        return;
    }
    SerialEndpointWorkspace::appendReceived(index, data);
}
std::vector<SerialEndpointWorkspace::BackgroundTask> SerialWorkspace::activeTasks() const
{
    auto result = SerialEndpointWorkspace::activeTasks();
    if (linkVerifier_.stressRunning())
        result.push_back({BackgroundTask::Kind::Stress,
                          0,
                          linkVerifier_.stressGeneration(),
                          QStringLiteral("串口调试 · 可靠性压测")});
    return result;
}
void SerialWorkspace::stopTask(const BackgroundTask& task)
{
    if (shuttingDown_ || task.channel < 0 || task.channel >= 2)
        return;
    if (task.kind == BackgroundTask::Kind::ScheduledSend &&
        channels_[task.channel].session.snapshot().scheduledSend.id == task.token)
        stopAutoSend(task.channel, true);
    else if (task.kind == BackgroundTask::Kind::Stress && linkVerifier_.stressRunning() &&
             linkVerifier_.stressGeneration() == task.token)
        stopStressTest(QStringLiteral("用户已停止可靠性压测"));
    else
        SerialEndpointWorkspace::stopTask(task);
    updateGlobalControls();
}
void SerialWorkspace::processScheduledSends()
{
    if (shuttingDown_)
        return;
    const auto now = PeriodicSendPlan::Clock::now();
    for (int index = 0; index < 2; ++index) {
        const auto result = channels_[index].session.processScheduledSend(now);
        if (result == testtools::ScheduledSendTick::Failed) {
            setChannelStatus(index, QStringLiteral("计划发送失败，连接或发送队列不可用"), true);
            stopAutoSend(index, true);
        } else if (result == testtools::ScheduledSendTick::Completed)
            stopAutoSend(index, true);
    }
    if (linkVerifier_.stressRunning() && !linkVerifier_.awaitingReceive()) {
        bool queuesDrained = true;
        for (std::size_t slot = 0; slot < linkVerifier_.directions().size(); ++slot) {
            const auto frame = linkVerifier_.nextStressFrame(slot, now);
            if (frame) {
                const bool sent = sendBytes(frame->transmitter, frame->bytes, false);
                linkVerifier_.recordStressAttempt(slot, sent, now);
                if (!sent) {
                    stopStressTest(QStringLiteral("可靠性压测发送失败"));
                    return;
                }
            }
            const auto& direction = linkVerifier_.directions()[slot];
            if (direction.active)
                queuesDrained =
                    queuesDrained &&
                    channels_[direction.transmitter].session.snapshot().pendingWrites == 0;
        }
        if (linkVerifier_.beginReceiveWait(queuesDrained)) {
            const auto generation = linkVerifier_.stressGeneration();
            emit activityChanged(QStringLiteral("压测帧已发送完成，正在等待链路尾部数据"));
            QTimer::singleShot(1500, this, [this, generation] { finishStressTest(generation); });
        }
    }
    updateScheduleTimer();
}
void SerialWorkspace::runBidirectionalTest()
{
    if (!channels_[0].session.isOpen() || !channels_[1].session.isOpen()) {
        QMessageBox::information(
            this, QStringLiteral("尚未连接"), QStringLiteral("需要先连接端点 A 和端点 B。"));
        return;
    }
    const auto frames = linkVerifier_.startProbe(
        std::to_string(QDateTime::currentMSecsSinceEpoch()),
        {lineEndingFromIndex(channels_[0].lineEndingCombo->currentIndex()),
         lineEndingFromIndex(channels_[1].lineEndingCombo->currentIndex())});
    const auto generation = linkVerifier_.probe().generation;
    updateBidirectionalResult();
    updateGlobalControls();
    if (!sendBytes(0, frames[0]) || !sendBytes(1, frames[1])) {
        linkVerifier_.stopProbe();
        testResultLabel_->setText(QStringLiteral("双向收发验证 · 发送失败"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
        updateGlobalControls();
        return;
    }
    QTimer::singleShot(3000, this, [this, generation] {
        if (linkVerifier_.probe().running && linkVerifier_.probe().generation == generation)
            updateBidirectionalResult(true);
    });
}
void SerialWorkspace::updateBidirectionalResult(bool timedOut)
{
    if (timedOut)
        linkVerifier_.expireProbe(linkVerifier_.probe().generation);
    const auto& probe = linkVerifier_.probe();
    if (probe.aToBPassed && probe.bToAPassed) {
        testResultLabel_->setText(QStringLiteral("双向收发验证 · A→B PASS  /  B→A PASS"));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusGood"));
    } else if (timedOut) {
        testResultLabel_->setText(
            QStringLiteral("双向收发验证超时 · A→B %1  /  B→A %2")
                .arg(probe.aToBPassed ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
                .arg(probe.bToAPassed ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusError"));
    } else {
        testResultLabel_->setText(
            QStringLiteral("正在验证 · A→B %1  /  B→A %2")
                .arg(probe.aToBPassed ? QStringLiteral("PASS") : QStringLiteral("等待"))
                .arg(probe.bToAPassed ? QStringLiteral("PASS") : QStringLiteral("等待")));
        setVisualRole(testResultLabel_, QStringLiteral("testStatusWorking"));
    }
    updateGlobalControls();
}
void SerialWorkspace::startStressTest(int direction,
                                      std::uint64_t frameCount,
                                      std::size_t payloadSize,
                                      int intervalMs)
{
    stopAutoSend(0);
    stopAutoSend(1);
    const auto configs = std::array<SerialConfig, 2>{channelConfig(0), channelConfig(1)};
    const int effective =
        testtools::SerialLinkVerifier::stressInterval(direction, payloadSize, intervalMs, configs);
    if (effective > intervalMs)
        QMessageBox::information(
            this,
            QStringLiteral("已按串口线速调整"),
            QStringLiteral("当前帧在所选波特率下至少需要约 %1 ms，实际发送间隔已由 %2 ms 调整为 %1 "
                           "ms，避免后台队列堆积。")
                .arg(effective)
                .arg(intervalMs));
    if (!linkVerifier_.startStress(direction, frameCount, payloadSize, intervalMs, configs))
        return;
    stressResultLabel_->setText(QStringLiteral("可靠性压测 · 正在发送"));
    setVisualRole(stressResultLabel_, QStringLiteral("testStatusWorking"));
    for (int index = 0; index < 2; ++index)
        appendNotice(index, QStringLiteral("— 可靠性压测数据流已折叠，仅更新字节计数与统计结果 —"));
    emit activityChanged(QStringLiteral("串口可靠性压测已开始"));
    updateGlobalControls();
}
void SerialWorkspace::stopStressTest(const QString& reason, bool completed)
{
    if (!linkVerifier_.stressRunning())
        return;
    bool passed = completed;
    QStringList summaries;
    for (const auto& direction : linkVerifier_.directions()) {
        if (!direction.active)
            continue;
        passed = passed && evaluateStressDirection(direction.sendPlan, direction.verifier).passed;
        summaries.push_back(stressDirectionSummary(direction));
    }
    linkVerifier_.stopStress();
    if (completed) {
        stressResultLabel_->setText(
            QStringLiteral("可靠性压测 · %1\n%2")
                .arg(passed ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                     summaries.join(QLatin1Char('\n'))));
        setVisualRole(stressResultLabel_,
                      passed ? QStringLiteral("testStatusGood")
                             : QStringLiteral("testStatusError"));
    } else {
        stressResultLabel_->setText(
            QStringLiteral("可靠性压测 · 已停止\n%1").arg(summaries.join(QLatin1Char('\n'))));
        setVisualRole(stressResultLabel_, QStringLiteral("testStatusNeutral"));
    }
    emit activityChanged(reason);
    updateGlobalControls();
}
void SerialWorkspace::finishStressTest(std::uint64_t generation)
{
    if (linkVerifier_.stressRunning() && linkVerifier_.stressGeneration() == generation)
        stopStressTest(QStringLiteral("串口可靠性压测已完成"), true);
}
QString SerialWorkspace::stressDirectionSummary(
    const testtools::SerialLinkVerifier::StressDirection& direction) const
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
