#include "toolbox_workspace.h"
#include "application/toolbox/gpio_inventory.h"
#include "application/agent/agent_connection_service.h"
#include "core/toolbox/toolbox_contract.h"
#include "ui/widgets/toolbox_plot.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QColor>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QScrollBar>
#include <algorithm>
#include <mutex>

using testtools::JsonValue;
using testtools::jsonString;
using testtools::jsonInteger;
using testtools::jsonBoolean;
namespace {
QString text(const std::string& value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
std::string bytes(const QString& value) { return value.toUtf8().toStdString(); }
JsonValue decode(const QVariant& value)
{
    JsonValue result; std::string error;
    if (!testtools::parseJson(bytes(value.toString()), result, error)) return JsonValue::object();
    return result;
}
JsonValue& devicePreferences(JsonValue& root, const std::string& serial)
{
    auto& devices = root.objectItems()["devices"]; if (!devices.isObject()) devices = JsonValue::object();
    auto& device = devices.objectItems()[serial]; if (!device.isObject()) device = JsonValue::object(); return device;
}
std::string pinKey(const JsonValue& pin)
{
    return testtools::gpioPreferenceKey(pin);
}
QString rate(const JsonValue& metrics, const char* key)
{
    const auto* value = metrics.find(key);
    return value && value->isInt64() ? QStringLiteral("%1 Mbps").arg(value->int64Value() / 1000000.0, 0, 'f', 2) : QStringLiteral("未知");
}
std::vector<QPointF> plotSamples(const JsonValue& samples)
{
    std::vector<QPointF> result;
    if (samples.isArray()) for (const auto& sample : samples.arrayItems()) {
        const auto* elapsed = sample.find("elapsed_ms"); const auto* bps = sample.find("bps");
        if (elapsed && bps && elapsed->isInt64() && bps->isInt64()) result.emplace_back(elapsed->int64Value() / 1000.0, bps->int64Value() / 1000000.0);
    }
    return result;
}
}
struct ToolboxWorkspace::PreferenceWriter {
    std::mutex mutex;
    std::string latest;
    std::uint64_t revision = 0;
    bool running = false;
    QString path;
};
ToolboxWorkspace::ToolboxWorkspace(QWidget* parent, std::shared_ptr<testtools::ToolboxService> service)
    : QObject(parent), preferenceWriter_(std::make_shared<PreferenceWriter>()),
      service_(service ? std::move(service) : std::make_shared<testtools::ToolboxService>(
          testtools::AgentConnectionService::createToolboxHost(bytes(QCoreApplication::applicationDirPath()))))
{
    setObjectName("toolboxWorkspace");
    gpioPage_ = buildGpio(parent); networkPage_ = buildNetwork(parent);
    timer_ = new QTimer(this); timer_->setInterval(150); connect(timer_, &QTimer::timeout, this, &ToolboxWorkspace::poll);
    gpioMetadataTimer_ = new QTimer(this); gpioMetadataTimer_->setInterval(3000);
    connect(gpioMetadataTimer_, &QTimer::timeout, this, [this] { if (gpioVisible_ && session_.openConnection) refresh(true); });
    preferencesTimer_ = new QTimer(this); preferencesTimer_->setSingleShot(true); preferencesTimer_->setInterval(250);
    connect(preferencesTimer_, &QTimer::timeout, this, &ToolboxWorkspace::savePreferences);
    for (auto* page : {gpioPage_, networkPage_}) {
        for (auto* spin : page->findChildren<QSpinBox*>()) { spin->setProperty("toolboxDefaultValue", spin->value()); connect(spin, &QSpinBox::valueChanged, this, [this] { if (!restoring_ && preferencesLoaded_) preferencesTimer_->start(); }); }
        for (auto* combo : page->findChildren<QComboBox*>()) { combo->setProperty("toolboxDefaultIndex", combo->currentIndex()); connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            updateControls(); if (!restoring_ && preferencesLoaded_) preferencesTimer_->start();
        }); }
    }
    connect(protocol_, &QComboBox::currentIndexChanged, this, [this] {
        parallel_->setMaximum(protocol_->currentData().toString() == "udp" ? 1 : 8); updateControls();
    });
    QString directory = qEnvironmentVariable("TESTTOOLS_TOOLBOX_PREFERENCES_DIRECTORY");
    if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/TestTools";
    preferenceWriter_->path = directory + "/toolbox-preferences.json";
    loadPreferences(); updateControls();
}
ToolboxWorkspace::~ToolboxWorkspace() { service_->cancel(); tasks_.wait(); }
void ToolboxWorkspace::setTarget(const testtools::AdbDevice& device)
{
    const bool changed = target_.serial != device.serial;
    if (changed && preferencesLoaded_) { preferencesTimer_->stop(); savePreferences(); }
    if (target_.serial != device.serial || device.state != "device") clearSession();
    target_ = device; if (changed) restoreParameters(); updateControls();
}
void ToolboxWorkspace::setSession(testtools::ToolboxSession session, std::vector<std::string> capabilities)
{
    session_ = std::move(session); capabilities_ = std::move(capabilities); metadataLoaded_ = false;
    if (gpioVisible_ && session_.openConnection) gpioMetadataTimer_->start();
    if (gpioVisible_ || networkVisible_) refresh(); updateControls();
}
void ToolboxWorkspace::clearSession()
{
    gpioMetadataTimer_->stop();
    session_ = {}; capabilities_.clear(); confirmations_.clear(); metadataLoaded_ = false;
    selectedPin_ = JsonValue::object(); gpio_ = JsonValue::object(); boardGpio_ = JsonValue::object(); boardNetwork_ = JsonValue::object(); hostNetwork_ = JsonValue::object();
    metadataError_.clear(); runnerAvailable_ = false; populate(); updateControls();
}
void ToolboxWorkspace::setVisible(bool gpio, bool network)
{
    gpioVisible_ = gpio; networkVisible_ = network;
    if (gpio && session_.openConnection && !closing_) gpioMetadataTimer_->start(); else gpioMetadataTimer_->stop();
    if ((gpio || network) && !metadataLoaded_ && !session_.serial.empty()) refresh();
}
void ToolboxWorkspace::refresh(bool gpioOnly)
{
    if (closing_ || metadataBusy_ || gpioMetadataBusy_ || !session_.openConnection ||
        std::find(capabilities_.begin(), capabilities_.end(), "toolbox.v1") == capabilities_.end()) return;
    if (gpioOnly) gpioMetadataBusy_ = true; else metadataBusy_ = true;
    updateControls(); timer_->start();
    const auto session = session_; const QPointer<ToolboxWorkspace> guard(this); const auto host = service_->host();
    const bool boardSupported = std::find(capabilities_.begin(), capabilities_.end(), "gpio.board.list") != capabilities_.end();
    tasks_.start([session, guard, host, gpioOnly, boardSupported] {
        JsonValue gpio = JsonValue::object(), boardGpio = JsonValue::object(), network = JsonValue::object(), local = JsonValue::object();
        bool runner = false; std::string error;
        std::shared_ptr<testtools::IAgentConnection> connection;
        try {
            connection = session.openConnection(error); if (!connection) throw std::runtime_error(error);
            const auto info = testtools::toolboxRpc(*connection, "toolbox.info", JsonValue::object());
            runner = jsonBoolean(info, "runner_available");
            JsonValue::Array all; std::int64_t cursor = 0;
            do {
                auto page = testtools::toolboxRpc(*connection, "gpio.chip.list", JsonValue::object({{"cursor", JsonValue::integer(cursor)}, {"limit", JsonValue::integer(128)}}));
                if (jsonString(page, "schema") != "gpio.inventory/v1" || !page.find("pins") || !page.find("pins")->isArray()) throw std::runtime_error("GPIO 列表格式不匹配");
                for (const auto& pin : page.find("pins")->arrayItems()) all.push_back(pin);
                gpio = page;
                const auto next = jsonInteger(page, "next_cursor");
                if (next <= cursor || next >= jsonInteger(page, "total")) break;
                cursor = next;
                if (all.size() >= 65536) throw std::runtime_error("GPIO 列表超出上限");
            } while (true);
            gpio.objectItems()["pins"] = JsonValue::array(std::move(all));
            if (boardSupported) {
                try {
                    boardGpio = testtools::toolboxRpc(*connection, "gpio.board.list", JsonValue::object());
                    if (jsonString(boardGpio, "schema") != "gpio.board.inventory/v1" || !boardGpio.find("endpoints") ||
                        !boardGpio.find("endpoints")->isArray() || boardGpio.find("endpoints")->arrayItems().size() > 128)
                        throw std::runtime_error("GPIO 控制接口列表格式不匹配");
                } catch (const std::exception& e) { boardGpio = JsonValue::object({{"unavailable", JsonValue::string(e.what())}}); }
            } else boardGpio = JsonValue::object({{"unavailable", JsonValue::string("当前服务未提供扩展 GPIO 控制接口")}});
            if (!gpioOnly) {
                network = testtools::toolboxRpc(*connection, "network.interface.list", JsonValue::object());
                if (host) { local = host->interfaces(); std::string hostError; runner = host->available(hostError) && runner; if (!hostError.empty()) error = hostError; }
            }
        } catch (const std::exception& exception) { error = exception.what(); }
        if (connection) connection->close();
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, session, gpio = std::move(gpio), boardGpio = std::move(boardGpio), network = std::move(network), local = std::move(local), runner, error, gpioOnly] {
            if (!guard) return;
            if (gpioOnly) guard->gpioMetadataBusy_ = false; else guard->metadataBusy_ = false;
            if (guard->session_.serial == session.serial && guard->session_.generation == session.generation) {
                const bool changed = testtools::serializeJson(guard->gpio_) != testtools::serializeJson(gpio) ||
                    testtools::serializeJson(guard->boardGpio_) != testtools::serializeJson(boardGpio);
                if (!gpioOnly || error.empty()) { guard->gpio_ = gpio; guard->boardGpio_ = boardGpio; }
                if (!gpioOnly) { guard->boardNetwork_ = network; guard->hostNetwork_ = local; guard->runnerAvailable_ = runner; }
                guard->metadataError_ = error; guard->metadataLoaded_ = true;
                if (!gpioOnly || (changed && error.empty())) guard->populate();
            } else if (!guard->session_.serial.empty() && (guard->gpioVisible_ || guard->networkVisible_)) guard->refresh();
            guard->updateControls();
        }, Qt::QueuedConnection);
    });
}
void ToolboxWorkspace::populate()
{
    const int scrollPosition = pins_->verticalScrollBar()->value();
    const auto oldPin = pinKey(selectedPin_);
    QSignalBlocker pinBlock(pins_); pins_->clear(); selectedPin_ = JsonValue::object();
    const auto& preference = devicePreferences(preferences_, target_.serial);
    const auto* favorites = preference.find("favorites");
    const auto displayPins = testtools::unifiedGpioInventory(gpio_, boardGpio_, capabilities_);
    for (auto pin : displayPins) {
        const auto chip = jsonString(pin, "chip");
        const auto controller = jsonString(pin, "controller_identity", chip);
        const auto remark = favorites ? testtools::gpioRemark(pin, *favorites) : std::string{};
        QString names;
        if (const auto* aliases = pin.find("aliases")) for (const auto& alias : aliases->arrayItems()) {
            if (!names.isEmpty()) names += " / "; names += text(alias.stringValue());
        }
        const auto name = remark.empty() ? names : names + (names.isEmpty() ? QString() : " · ") + text(remark);
        testtools::RockchipGpioNumber converted; std::string conversionError;
        QString pinName;
        if (testtools::convertRockchipGpio(controller.substr(0, controller.find('/')) + "_" +
            static_cast<char>('A' + jsonInteger(pin, "offset") / 8) + std::to_string(jsonInteger(pin, "offset") % 8), converted, conversionError)) pinName = text(converted.name);
        if (const auto* global = pin.find("global_number"); global && global->isInt64()) pinName += (pinName.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("gpio-%1").arg(global->int64Value());
        if (pinName.isEmpty()) pinName = chip.empty() ? QStringLiteral("映射未确认") : text(chip) + QStringLiteral(" · %1").arg(jsonInteger(pin, "offset"));
        const auto consumer = jsonString(pin, "consumer");
        auto* item = new QTreeWidgetItem(pins_, {pinName, name,
            jsonString(pin, "direction") == "output" ? QStringLiteral("输出") : jsonString(pin, "direction") == "input" ? QStringLiteral("输入") : QStringLiteral("未知"),
            jsonBoolean(pin, "used") ? (consumer.empty() ? QStringLiteral("已占用（未提供名称）") : text(consumer)) : QStringLiteral("未占用")});
        item->setToolTip(0, text(chip) + QStringLiteral(" · 行 %1\n").arg(jsonInteger(pin, "offset")) + text(controller));
        QString filterText; for (int column = 0; column < 4; ++column) filterText += item->text(column) + ' ';
        filterText += text(chip); item->setData(0, Qt::UserRole + 3, filterText); item->setHidden(!filterText.contains(filter_->text(), Qt::CaseInsensitive));
        item->setData(0, Qt::UserRole, text(testtools::serializeJson(pin)));
        item->setData(0, Qt::UserRole + 1, text(chip)); item->setData(0, Qt::UserRole + 2, static_cast<int>(jsonInteger(pin, "offset")));
        if (jsonBoolean(pin, "used") && jsonString(pin, "read_operation").empty()) for (int col = 0; col < 4; ++col) item->setForeground(col, QColor("#8a837c"));
        if (pinKey(pin) == oldPin) { pins_->setCurrentItem(item); selectedPin_ = pin; }
    }
    QSignalBlocker pcBlock(pcInterface_), boardBlock(boardInterface_);
    const auto previousPc = pcInterface_->currentData().toString(), previousBoard = boardInterface_->currentData().toString();
    pcInterface_->clear(); boardInterface_->clear();
    const auto add = [](QComboBox* combo, const JsonValue& inventory, bool local) {
        const auto* values = inventory.find("interfaces"); if (!values || !values->isArray()) return;
        for (const auto& interface : values->arrayItems()) {
            if (!jsonBoolean(interface, "physical")) continue;
            std::vector<std::string> addresses;
            if (const auto* items = interface.find("ipv4"); items && items->isArray()) for (const auto& address : items->arrayItems())
                if (address.isString() && testtools::validIpv4(address.stringValue())) addresses.push_back(address.stringValue());
            if (addresses.empty()) addresses.push_back({});
            for (const auto& address : addresses) {
                auto option = interface; option.objectItems()["selected_ip"] = JsonValue::string(address);
                const auto display = local ? jsonString(interface, "name") : jsonString(interface, "id");
                const auto* speed = interface.find(local ? "speed_bps" : "speed_mbps");
                QString suffix;
                if (speed && speed->isInt64()) suffix += QStringLiteral(" · %1 Mbps").arg(local ? speed->int64Value() / 1000000 : speed->int64Value());
                if (!jsonBoolean(interface, "up") || (!local && jsonString(interface, "operstate") != "up")) suffix += QStringLiteral(" · 未连接/已停用");
                combo->addItem(text(display) + "  " + (address.empty() ? QStringLiteral("未配置 IPv4") : text(address)) + suffix,
                    text(testtools::serializeJson(option)));
            }
        }
    };
    add(pcInterface_, hostNetwork_, true); add(boardInterface_, boardNetwork_, false);
    const auto restore = [](QComboBox* combo, const QString& previous, const JsonValue* saved) {
        auto selection = decode(previous); if (previous.isEmpty() && saved && saved->isObject()) selection = *saved;
        const auto id = jsonString(selection, "id"), ip = jsonString(selection, "selected_ip");
        if (id.empty()) {
            for (int i = 0; i < combo->count(); ++i) {
                const auto candidate = decode(combo->itemData(i));
                if (jsonBoolean(candidate, "up") && testtools::validIpv4(jsonString(candidate, "selected_ip"))) { combo->setCurrentIndex(i); return; }
            }
            return;
        }
        int sameInterface = -1;
        for (int i = 0; i < combo->count(); ++i) {
            const auto candidate = decode(combo->itemData(i));
            if (jsonString(candidate, "id") != id) continue;
            if (sameInterface < 0) sameInterface = i;
            if (jsonString(candidate, "selected_ip") == ip) { combo->setCurrentIndex(i); return; }
        }
        combo->setCurrentIndex(sameInterface);
    };
    restore(pcInterface_, previousPc, preference.find("pc_selection"));
    restore(boardInterface_, previousBoard, preference.find("board_selection"));
    pins_->verticalScrollBar()->setValue(scrollPosition);
    selectPin();
}
void ToolboxWorkspace::calculatePin()
{
    testtools::RockchipGpioNumber number; std::string error;
    if (!testtools::convertRockchipGpio(bytes(pinLookup_->text()), number, error)) { pinConversion_->setText(text(error)); return; }
    QString result = QStringLiteral("%1 = %2 × 32 + %3 × 8 + %4 = %5；行偏移 %6")
        .arg(text(number.name)).arg(number.bank).arg(number.offset / 8).arg(number.offset % 8).arg(number.number).arg(number.offset);
    QString controller;
    if (const auto* chips = gpio_.find("chips"); chips && chips->isArray()) for (const auto& chip : chips->arrayItems())
        if (jsonString(chip, "label") == "gpio" + std::to_string(number.bank)) {
            if (!controller.isEmpty()) { controller.clear(); break; }
            controller = text(jsonString(chip, "id"));
        }
    for (int row = 0; !controller.isEmpty() && row < pins_->topLevelItemCount(); ++row) {
        auto* item = pins_->topLevelItem(row);
        if (item->data(0, Qt::UserRole + 1).toString() == controller && item->data(0, Qt::UserRole + 2).toInt() == number.offset) {
            filter_->clear(); pins_->setCurrentItem(item); pins_->scrollToItem(item);
            const auto pin = decode(item->data(0, Qt::UserRole)); const auto* global = pin.find("global_number");
            result += global && global->isInt64() ? QStringLiteral("；本机 Linux 号 %1").arg(global->int64Value()) : QStringLiteral("；Linux 全局号未知");
            pinConversion_->setText(result); return;
        }
    }
    pinConversion_->setText(result + QStringLiteral("；以上为 Rockchip 算术编号，当前设备映射未确认。"));
}
void ToolboxWorkspace::selectPin()
{
    auto* item = pins_->currentItem(); selectedPin_ = item ? decode(item->data(0, Qt::UserRole)) : JsonValue::object();
    if (!item) { pinInfo_->setText(QStringLiteral("选择引脚后查看详情")); pinDetails_->clear(); }
    else {
        pinInfo_->setText(QStringLiteral("%1\n名称 / 备注：%2\n方向：%3\n占用者：%4")
            .arg(item->text(0), item->text(1), item->text(2), item->text(3)));
        QString detail = QStringLiteral("控制器：%1\n行偏移：%2\n极性：%3\n字符设备接口：v%4")
            .arg(text(jsonString(selectedPin_, "chip"))).arg(jsonInteger(selectedPin_, "offset"))
            .arg(jsonBoolean(selectedPin_, "active_low") ? QStringLiteral("有效低") : QStringLiteral("有效高"))
            .arg(jsonInteger(selectedPin_, "abi"));
        if (const auto* interfaces = selectedPin_.find("access_details")) for (const auto& interface : interfaces->arrayItems())
            detail += QStringLiteral("\n\n设备名称：%1\n驱动：%2\n访问方式：%3\n路径：%4")
                .arg(text(jsonString(interface, "name")), text(jsonString(interface, "driver")), text(jsonString(interface, "adapter")), text(jsonString(interface, "path")));
        pinDetails_->setText(detail);
    }
    updateControls();
}
void ToolboxWorkspace::updateControls()
{
    const auto state = service_->snapshot();
    const bool ready = !closing_ && session_.openConnection && session_.serial == target_.serial &&
        std::find(capabilities_.begin(), capabilities_.end(), "toolbox.v1") != capabilities_.end();
    const bool busy = state.active || state.remotePending || metadataBusy_;
    const bool blocked = conflict && conflict(target_.serial);
    const bool persistent = jsonBoolean(selectedPin_, "output_persistent");
    const bool pin = ready && !busy && !blocked;
    const auto* favorites = devicePreferences(preferences_, target_.serial).find("favorites");
    const auto remark = favorites ? testtools::gpioRemark(selectedPin_, *favorites) : std::string{};
    favorite_->setEnabled(ready && !busy && !pinKey(selectedPin_).empty() && preferencesLoaded_);
    favorite_->setText(!remark.empty() ? QStringLiteral("移出常用") : QStringLiteral("加入常用…"));
    const auto available = [&](const char* action) { return !jsonString(selectedPin_, std::string(action) + "_operation").empty(); };
    read_->setEnabled(pin && available("read")); write_->setEnabled(pin && available("output"));
    pulse_->setEnabled(pin && available("pulse")); monitor_->setEnabled(pin && available("monitor"));
    write_->setText(persistent ? QStringLiteral("设置输出") : QStringLiteral("保持输出"));
    nativeOutputOptions_->setVisible(!persistent);
    nativeExtraOptions_->setVisible(pinKey(selectedPin_).empty() || available("pulse") || available("monitor"));
    boardHint_->setVisible(!pinKey(selectedPin_).empty());
    boardHint_->setText(available("output") ? (persistent ? QStringLiteral("设置持续生效，结束操作或关闭程序后不自动恢复。") :
        QStringLiteral("保持指定时长后释放；释放后电平由硬件与驱动决定。")) : (available("read") ? QStringLiteral("当前只读：") : QString()) + text(jsonString(selectedPin_, "reason")));
    for (auto* control : {read_, write_, pulse_, monitor_}) control->setToolTip(control->isEnabled() ?
        QStringLiteral("确认目标与参数后操作；常用收藏可选。") : (available("read") ? QStringLiteral("当前只读：") : QString()) + text(jsonString(selectedPin_, "reason")));
    gpioRefresh_->setEnabled(ready && !metadataBusy_); networkRefresh_->setEnabled(ready && !metadataBusy_);
    gpioConnect_->setVisible(!ready); networkConnect_->setVisible(!ready);
    gpioConnect_->setEnabled(!closing_ && target_.state == "device"); networkConnect_->setEnabled(!closing_ && target_.state == "device");
    const bool gpioTask = state.capability.rfind("gpio.", 0) == 0;
    gpioStop_->setEnabled(state.active && gpioTask && !state.cancelling);
    gpioStop_->setText(state.capability.rfind("gpio.board.", 0) == 0 ? QStringLiteral("停止操作") : QStringLiteral("停止 / 释放引脚"));
    extend_->setVisible(!persistent || (state.active && state.capability == "gpio.pin.write"));
    networkStop_->setEnabled(state.active && !gpioTask && !state.cancelling);
    extend_->setEnabled(state.active && state.capability == "gpio.pin.write" && !state.cancelling && state.serial == target_.serial);
    gpioReconcile_->setVisible(state.remotePending && gpioTask); networkReconcile_->setVisible(state.remotePending && !gpioTask);
    gpioReconcile_->setEnabled(ready && state.serial == target_.serial); networkReconcile_->setEnabled(ready && state.serial == target_.serial);
    gpioExport_->setEnabled(gpioReport_.find("state") != nullptr); networkExport_->setEnabled(networkReport_.find("state") != nullptr);
    const auto pc = decode(pcInterface_->currentData()), boardOption = decode(boardInterface_->currentData());
    const bool networkSelected = pcInterface_->currentIndex() >= 0 && boardInterface_->currentIndex() >= 0;
    const bool networkLink = jsonBoolean(pc, "up") && jsonBoolean(boardOption, "up") && jsonString(boardOption, "operstate") == "up";
    const bool networkAddress = testtools::validIpv4(jsonString(pc, "selected_ip")) && testtools::validIpv4(jsonString(boardOption, "selected_ip"));
    networkStart_->setEnabled(ready && !busy && !blocked && runnerAvailable_ && networkSelected && networkLink && networkAddress);
    const bool udp = protocol_->currentData().toString() == "udp";
    parallel_->setEnabled(!state.active && !udp); udpRate_->setEnabled(!state.active && udp);
    for (QWidget* control : std::initializer_list<QWidget*>{level_, drive_, idleLevel_, pulseLevel_, edge_, pulseWidth_, monitorSeconds_,
                            pcInterface_, boardInterface_, direction_, protocol_, duration_, port_}) control->setEnabled(!state.active && !closing_ && preferencesLoaded_);
    QString status;
    if (!ready) status = session_.openConnection ? QStringLiteral("当前板端服务尚未提供工具箱能力，请连接新版服务") : QStringLiteral("连接板端服务后查看引脚");
    else if (metadataBusy_) status = QStringLiteral("正在读取引脚与网口信息…");
    else if (!jsonString(gpio_, "unavailable").empty() && pins_->topLevelItemCount() == 0) status = QStringLiteral("GPIO 接口暂不可用，请检查接口和访问权限，详情中可查看原因");
    else if (!metadataError_.empty()) status = QStringLiteral("信息读取异常，详情中可查看原因");
    else if (metadataLoaded_) status = QStringLiteral("%1 个引脚 · %2").arg(pins_->topLevelItemCount()).arg(text(target_.serial));
    else status = QStringLiteral("点击刷新读取可用引脚");
    gpioStatus_->setText(status);
    if (!ready) networkStatus_->setText(QStringLiteral("连接板端服务后选择两端以太网接口"));
    else if (metadataBusy_) networkStatus_->setText(QStringLiteral("正在读取两端网口…"));
    else if (!boardInterface_->count()) networkStatus_->setText(QStringLiteral("板端未发现以太网接口，请检查驱动后刷新。"));
    else if (!pcInterface_->count()) networkStatus_->setText(QStringLiteral("电脑未发现以太网接口，请检查网卡后刷新。"));
    else if (!networkSelected) networkStatus_->setText(QStringLiteral("请选择两端网口；未配置 IP 的网口也可以选择。"));
    else if (!networkLink) networkStatus_->setText(QStringLiteral("所选网口未连接或已停用，请检查网线与接口状态后刷新。"));
    else if (!networkAddress) networkStatus_->setText(QStringLiteral("所选网口未配置 IPv4；网口选择已保留，配置地址后刷新即可测速。"));
    else if (!runnerAvailable_) networkStatus_->setText(QStringLiteral("两端网速工具尚未就绪，请查看详情。"));
    else networkStatus_->setText(QStringLiteral("已选择两端以太网接口；开始时核对路由并发送测试流量。"));
    if (!metadataError_.empty()) { gpioDetails_->setPlainText(text(metadataError_)); networkDetails_->setPlainText(text(metadataError_)); }
    if (!jsonString(gpio_, "unavailable").empty()) gpioDetails_->setPlainText(text(jsonString(gpio_, "unavailable")));
}
void ToolboxWorkspace::runGpio(const std::string& requestedCapability)
{
    const std::string action = requestedCapability == "gpio.pin.read" ? "read" : requestedCapability == "gpio.pin.write" ? "output" : requestedCapability == "gpio.pin.pulse" ? "pulse" : "monitor";
    const auto capability = jsonString(selectedPin_, action + "_operation");
    if (capability.empty()) return;
    const bool persistent = jsonBoolean(selectedPin_, "output_persistent");
    const auto session = session_; const auto pin = selectedPin_;
    const auto* planned = pin.find(action + "_arguments"); if (!planned || !planned->isObject()) return;
    JsonValue args = *planned;
    if (capability == "gpio.board.set") args.objectItems()["level"] = JsonValue::integer(level_->currentData().toInt());
    if (capability == "gpio.pin.write") { args.objectItems()["level"] = JsonValue::integer(level_->currentData().toInt()); args.objectItems()["hold_sec"] = JsonValue::integer(holdSeconds_->value()); }
    if (capability == "gpio.pin.pulse") { args.objectItems()["idle_level"] = JsonValue::integer(idleLevel_->currentData().toInt()); args.objectItems()["pulse_level"] = JsonValue::integer(pulseLevel_->currentData().toInt()); args.objectItems()["pulse_ms"] = JsonValue::integer(pulseWidth_->value()); }
    if (capability == "gpio.pin.write" || capability == "gpio.pin.pulse") args.objectItems()["drive"] = JsonValue::string(bytes(drive_->currentData().toString()));
    if (capability == "gpio.edge.monitor") { args.objectItems()["edge"] = JsonValue::string(bytes(edge_->currentData().toString())); args.objectItems()["duration_sec"] = JsonValue::integer(monitorSeconds_->value()); }
    std::string error;
    if (!testtools::validateToolArguments(capability, args, error)) { gpioResult_->setText(QStringLiteral("参数无效：") + text(error)); return; }
    if (capability != "gpio.pin.read" && capability != "gpio.board.read") {
        const auto* favorites = devicePreferences(preferences_, target_.serial).find("favorites");
        const auto remark = favorites ? testtools::gpioRemark(pin, *favorites) : std::string{};
        const auto key = session.serial + ":" + std::to_string(session.generation) + ":" + capability + ":" + testtools::serializeJson(args);
        if (!confirmations_.count(key)) {
            const auto action = persistent ? QStringLiteral("设置输出") : capability == "gpio.pin.write" ? QStringLiteral("保持输出") : capability == "gpio.pin.pulse" ? QStringLiteral("输出单次脉冲") : QStringLiteral("切换输入并监听");
            const auto levelText = [](int level) { return level ? QStringLiteral("高电平") : QStringLiteral("低电平"); };
            const auto summary = persistent ? QStringLiteral("设置%1；此操作会切换为输出。\n设置持续生效，结束任务或关闭程序不自动恢复。").arg(levelText(level_->currentData().toInt())) : capability == "gpio.pin.write"
                ? QStringLiteral("推挽输出%1，保持 %2 秒").arg(levelText(level_->currentData().toInt())).arg(holdSeconds_->value())
                : capability == "gpio.pin.pulse"
                ? QStringLiteral("推挽输出：%1 → %2（%3 ms）→ %1").arg(levelText(idleLevel_->currentData().toInt()), levelText(pulseLevel_->currentData().toInt())).arg(pulseWidth_->value())
                : QStringLiteral("切换为输入，监听%1，持续 %2 秒").arg(edge_->currentText()).arg(monitorSeconds_->value());
            QMessageBox confirmation(QMessageBox::Warning, QStringLiteral("确认 GPIO 操作"),
                QStringLiteral("设备：%1\n引脚：%2 · 行 %3\n备注：%4\n操作：%5\n\n%6%7")
                    .arg(text(session.serial)).arg(pins_->currentItem() ? pins_->currentItem()->text(0) + " / " + pins_->currentItem()->text(1) : text(jsonString(pin, "chip"))).arg(jsonInteger(pin, "offset"))
                    .arg(!remark.empty() ? text(remark) : QStringLiteral("未设置（可选）")).arg(action).arg(summary)
                    .arg(persistent ? QString() : QStringLiteral("\n释放后电平由硬件与驱动决定。")),
                QMessageBox::Ok | QMessageBox::Cancel, gpioPage_);
            confirmation.setDefaultButton(QMessageBox::Cancel);
            if (confirmation.exec() != QMessageBox::Ok) return;
            if (session_.generation != session.generation || session_.serial != session.serial || pinKey(pin) != pinKey(selectedPin_)) { gpioResult_->setText(QStringLiteral("目标已变化，请重新选择")); return; }
            confirmations_.insert(key);
        }
    }
    testtools::ToolboxRequest request; request.session = session; request.capability = capability; request.arguments = args;
    if (!service_->prepare(std::move(request), conflict && conflict(session.serial), error)) { gpioResult_->setText(text(error)); return; }
    tasks_.start([service = service_] { service->executePrepared(); }); timer_->start(); updateControls(); emit taskChanged();
}
void ToolboxWorkspace::runNetwork()
{
    const auto pc = decode(pcInterface_->currentData()), board = decode(boardInterface_->currentData());
    testtools::ToolboxRequest request; request.session = session_; request.networkTest = true;
    auto& net = request.network;
    net.localInterface = static_cast<std::uint32_t>(jsonInteger(pc, "index")); net.sourceIp = jsonString(pc, "selected_ip");
    net.boardInterface = jsonString(board, "id"); net.boardIp = jsonString(board, "selected_ip");
    net.direction = direction_->currentData().toInt(); net.seconds = duration_->value(); net.parallel = parallel_->value();
    net.protocol = bytes(protocol_->currentData().toString()); net.port = port_->value(); net.udpMbps = udpRate_->value();
    const int plotSeconds = net.seconds;
    std::string error;
    if (!service_->prepare(std::move(request), conflict && conflict(session_.serial), error)) { networkResult_->setText(text(error)); return; }
    plot_->clear(plotSeconds); tasks_.start([service = service_] { service->executePrepared(); }); timer_->start(); updateControls(); emit taskChanged();
}
void ToolboxWorkspace::poll()
{
    tasks_.reapFinished(); const auto state = service_->snapshot();
    const bool gpio = state.capability.rfind("gpio.", 0) == 0;
    if (state.id) {
        auto* result = gpio ? gpioResult_ : networkResult_; auto* details = gpio ? gpioDetails_ : networkDetails_;
        QString message = text(state.message);
        if (state.serial != target_.serial) message.prepend(QStringLiteral("设备 %1：").arg(text(state.serial)));
        if (state.active && gpio) {
            const auto* data = state.progress.find("data");
            if (data && data->find("physical_level")) message += QStringLiteral(" · 当前读回 %1").arg(jsonInteger(*data, "physical_level"));
            if (state.capability == "gpio.pin.write") if (const auto* arguments = state.report.find("arguments"))
                message += jsonInteger(*arguments, "level") ? QStringLiteral(" · 设定高电平") : QStringLiteral(" · 设定低电平");
            if (data && data->find("event_count")) message += QStringLiteral(" · %1 次边沿").arg(jsonInteger(*data, "event_count"));
            if (state.capability == "gpio.pin.write") message += QStringLiteral(" · 剩余 %1 秒").arg((jsonInteger(state.progress, "remaining_ms") + 999) / 1000);
        }
        if (state.active && !gpio) if (const auto* metrics = state.progress.find("client_metrics")) {
            message += QStringLiteral(" · %1").arg(rate(*metrics, "current_bps"));
            if (const auto* samples = state.progress.find("client_samples")) plot_->setSamples(jsonString(state.progress, "direction") == "board_to_pc" ? 1 : 0, plotSamples(*samples));
        }
        if (!state.active && gpio) {
            const auto* terminal = state.progress.find("result");
            const auto* values = terminal ? terminal->find("observations") : nullptr;
            if (values && values->isObject()) {
                const auto* physical = values->find("physical_level");
                if (physical && physical->isInt64()) message += physical->int64Value() ? QStringLiteral(" · 读回高电平（1）") : QStringLiteral(" · 读回低电平（0）");
                if (state.capability == "gpio.edge.monitor") message += QStringLiteral(" · 记录 %1 次边沿").arg(jsonInteger(*values, "event_count"));
                if (jsonBoolean(*values, "released")) message += QStringLiteral(" · 已释放");
                if (state.capability == "gpio.board.set" && jsonBoolean(*values, "write_completed"))
                    message += state.state == "succeeded" ? QStringLiteral(" · 已设置并读回，设置持续生效") : QStringLiteral(" · 已提交写入，当前状态需重新核对");
            }
            if (state.detail.find("occupied") != std::string::npos) message += QStringLiteral(" · 引脚已被占用，请刷新后重试");
            if (state.detail.find("Permission denied") != std::string::npos) message += QStringLiteral(" · 权限不足，请检查服务权限");
        }
        if (!state.active && !gpio) {
            QStringList rows;
            if (const auto* runs = state.report.find("runs")) for (const auto& run : runs->arrayItems()) {
                if (const auto* samples = run.find("samples")) plot_->setSamples(jsonString(run, "direction") == "board_to_pc" ? 1 : 0, plotSamples(*samples));
                const auto direction = jsonString(run, "direction") == "pc_to_board" ? QStringLiteral("电脑 → 板卡") : QStringLiteral("板卡 → 电脑");
                if (const auto* metrics = run.find("metrics")) {
                    auto row = direction + QStringLiteral("：发送 %1，接收 %2").arg(rate(*metrics, "sender_bps"), rate(*metrics, "receiver_bps"));
                    if (const auto* retransmits = metrics->find("retransmits"); retransmits && retransmits->isInt64()) row += QStringLiteral("，重传 %1").arg(retransmits->int64Value());
                    if (const auto* jitter = metrics->find("jitter_us"); jitter && jitter->isInt64()) row += QStringLiteral("，抖动 %1 ms").arg(jitter->int64Value() / 1000.0, 0, 'f', 3);
                    if (const auto* loss = metrics->find("loss_ppm"); loss && loss->isInt64()) row += QStringLiteral("，丢包 %1%").arg(loss->int64Value() / 10000.0, 0, 'f', 3);
                    rows << row;
                }
                else rows << direction + (jsonString(run, "state") == "not_run" ? QStringLiteral("：未执行") : QStringLiteral("：失败或未确认结束"));
            }
            if (!rows.isEmpty()) message += "\n" + rows.join('\n');
        }
        result->setText(message);
        if (state.id != renderedId_ || state.state != renderedState_) {
            details->setPlainText(text(state.detail) + "\n" + text(testtools::serializeJson(state.progress)));
            if (!state.active) {
                if (gpio) gpioReport_ = state.report; else networkReport_ = state.report;
                if (gpio && gpioVisible_) refresh(true);
                emit activityChanged(text(state.message));
            }
            renderedId_ = state.id; renderedState_ = state.state; emit taskChanged();
        }
    }
    updateControls();
    if (closing_ && !state.active && tasks_.retainedTaskCount() == 0 && !shutdownEmitted_) { shutdownEmitted_ = true; timer_->stop(); emit shutdownFinished(); }
    else if (!state.active && !metadataBusy_ && tasks_.retainedTaskCount() == 0) timer_->stop();
}
void ToolboxWorkspace::cancelTask(std::uint64_t id) { service_->cancel(id); timer_->start(); updateControls(); }
bool ToolboxWorkspace::busyForDevice(const std::string& serial) const { const auto state = service_->snapshot(); return state.serial == serial && (state.active || state.remotePending); }
testtools::ToolboxSnapshot ToolboxWorkspace::snapshot() const { return service_->snapshot(); }
void ToolboxWorkspace::shutdown()
{
    if (closing_) return; closing_ = true; gpioMetadataTimer_->stop(); preferencesTimer_->stop(); savePreferences(); service_->cancel(); timer_->start(); updateControls();
}
void ToolboxWorkspace::reconcile()
{
    if (!session_.openConnection || metadataBusy_) return;
    metadataBusy_ = true; const auto session = session_; const QPointer<ToolboxWorkspace> guard(this);
    tasks_.start([service = service_, session, guard] {
        std::string error; const bool ok = service->reconcile(session, error);
        if (guard) QMetaObject::invokeMethod(guard, [guard, ok, error] {
            if (!guard) return; guard->metadataBusy_ = false;
            if (!ok) emit guard->activityChanged(QStringLiteral("任务复核失败：") + text(error));
            guard->renderedState_.clear(); guard->poll();
        }, Qt::QueuedConnection);
    }); timer_->start(); updateControls();
}
void ToolboxWorkspace::saveReport(bool network)
{
    auto report = network ? networkReport_ : gpioReport_;
    if (!report.find("state")) return;
    const auto path = QFileDialog::getSaveFileName(network ? networkPage_ : gpioPage_, QStringLiteral("导出工具报告"),
        QDir::homePath() + (network ? "/network-test.json" : "/gpio-test.json"), QStringLiteral("JSON 报告 (*.json);;文本报告 (*.txt)"));
    if (path.isEmpty()) return;
    report.objectItems()["desktop_version"] = JsonValue::string(bytes(QCoreApplication::applicationVersion()));
    report.objectItems()["exported_at"] = JsonValue::string(bytes(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)));
    testtools::JsonLimits limits; limits.max_output_bytes = 8 * 1024 * 1024; limits.max_string_bytes = 4 * 1024 * 1024; limits.max_array_items = 65536;
    const auto output = testtools::serializeJson(report, limits);
    if (output.empty()) { emit activityChanged(QStringLiteral("报告超过导出上限，原结果仍保留")); return; }
    const QPointer<ToolboxWorkspace> guard(this);
    tasks_.start([path, output, report, guard] {
        QSaveFile file(path); bool ok = file.open(QIODevice::WriteOnly);
        QByteArray data = QByteArray::fromStdString(output);
        if (path.endsWith(".txt", Qt::CaseInsensitive)) data.prepend(QStringLiteral("设备：%1\n任务：%2\n状态：%3\n\n完整记录：\n").arg(text(jsonString(report, "device")), text(jsonString(report, "capability")), text(jsonString(report, "state"))).toUtf8());
        ok = ok && file.write(data) == data.size() && file.commit();
        const auto error = file.errorString();
        if (guard) QMetaObject::invokeMethod(guard, [guard, ok, path, error] { if (guard) emit guard->activityChanged(ok ? QStringLiteral("报告已保存：") + path : QStringLiteral("报告保存失败：") + error); }, Qt::QueuedConnection);
    }); timer_->start();
}
void ToolboxWorkspace::toggleFavorite()
{
    if (pinKey(selectedPin_).empty() || !preferencesLoaded_) return;
    auto& device = devicePreferences(preferences_, target_.serial);
    auto& favorites = device.objectItems()["favorites"]; if (!favorites.isObject()) favorites = JsonValue::object();
    const auto key = pinKey(selectedPin_);
    if (!testtools::gpioRemark(selectedPin_, favorites).empty()) testtools::removeGpioFavorite(selectedPin_, favorites);
    else {
        if (favorites.objectItems().size() >= 64) { gpioResult_->setText(QStringLiteral("每台设备最多保存 64 个常用引脚")); return; }
        bool ok = false;
        const auto remark = QInputDialog::getText(gpioPage_, QStringLiteral("加入常用控制引脚"), QStringLiteral("填写用途或信号名："), QLineEdit::Normal,
            text(jsonString(selectedPin_, "name")), &ok).trimmed();
        if (!ok || remark.isEmpty() || remark.size() > 64) return;
        favorites.objectItems()[key] = JsonValue::string(bytes(remark));
    }
    populate(); savePreferences(); updateControls();
}
void ToolboxWorkspace::loadPreferences()
{
    const auto path = preferenceWriter_->path; const QPointer<ToolboxWorkspace> guard(this);
    tasks_.start([path, guard] {
        JsonValue root = JsonValue::object(); std::string error; QFile file(path);
        if (file.exists() && (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024 || !testtools::parseJson(file.readAll().toStdString(), root, error) || !root.isObject())) {
            root = JsonValue::object(); error = "工具箱偏好读取失败，本次使用默认参数";
        }
        if (guard) QMetaObject::invokeMethod(guard, [guard, root, error] {
            if (!guard) return; guard->preferences_ = root; guard->preferencesLoaded_ = true;
            guard->restoreParameters(); guard->populate(); guard->updateControls();
            if (!error.empty()) emit guard->activityChanged(text(error));
        }, Qt::QueuedConnection);
    }); timer_->start();
}
void ToolboxWorkspace::restoreParameters()
{
    if (!preferencesLoaded_ || target_.serial.empty()) return;
    restoring_ = true;
    const auto& device = devicePreferences(preferences_, target_.serial);
    const auto* values = device.find("parameters");
    {
        for (auto* page : {gpioPage_, networkPage_}) {
            for (auto* spin : page->findChildren<QSpinBox*>()) {
                const auto* value = values && values->isObject() ? values->find(bytes(spin->objectName())) : nullptr;
                spin->setValue(value && value->isInt64() && value->int64Value() >= spin->minimum() && value->int64Value() <= spin->maximum() ?
                    static_cast<int>(value->int64Value()) : spin->property("toolboxDefaultValue").toInt());
            }
            for (auto* combo : page->findChildren<QComboBox*>()) if (combo != pcInterface_ && combo != boardInterface_)
                {
                    const auto* value = values && values->isObject() ? values->find(bytes(combo->objectName())) : nullptr;
                    combo->setCurrentIndex(value && value->isInt64() && value->int64Value() >= 0 && value->int64Value() < combo->count() ?
                        static_cast<int>(value->int64Value()) : combo->property("toolboxDefaultIndex").toInt());
                }
        }
    }
    restoring_ = false;
}
void ToolboxWorkspace::savePreferences()
{
    if (!preferencesLoaded_ || target_.serial.empty()) return;
    auto& device = devicePreferences(preferences_, target_.serial);
    JsonValue::Object parameters;
    for (auto* page : {gpioPage_, networkPage_}) {
        for (auto* spin : page->findChildren<QSpinBox*>()) parameters[bytes(spin->objectName())] = JsonValue::integer(spin->value());
        for (auto* combo : page->findChildren<QComboBox*>()) if (combo != pcInterface_ && combo != boardInterface_) parameters[bytes(combo->objectName())] = JsonValue::integer(combo->currentIndex());
    }
    device.objectItems()["parameters"] = JsonValue::object(std::move(parameters));
    if (pcInterface_->currentIndex() >= 0) device.objectItems()["pc_selection"] = decode(pcInterface_->currentData());
    if (boardInterface_->currentIndex() >= 0) device.objectItems()["board_selection"] = decode(boardInterface_->currentData());
    preferences_.objectItems()["version"] = JsonValue::integer(1);
    const auto output = testtools::serializeJson(preferences_);
    if (output.empty() || output.size() > 256 * 1024) { emit activityChanged(QStringLiteral("工具箱偏好超出保存上限")); return; }
    const auto writer = preferenceWriter_; const QPointer<ToolboxWorkspace> guard(this);
    { std::lock_guard<std::mutex> lock(writer->mutex); writer->latest = output; ++writer->revision; if (writer->running) return; writer->running = true; }
    tasks_.start([writer, guard] {
        while (true) {
            std::string current; std::uint64_t revision;
            { std::lock_guard<std::mutex> lock(writer->mutex); current = writer->latest; revision = writer->revision; }
            QDir().mkpath(QFileInfo(writer->path).absolutePath()); QSaveFile file(writer->path);
            const bool ok = file.open(QIODevice::WriteOnly) && file.write(current.data(), static_cast<qint64>(current.size())) == static_cast<qint64>(current.size()) && file.commit();
            if (!ok && guard) QMetaObject::invokeMethod(guard, [guard] { if (guard) emit guard->activityChanged(QStringLiteral("工具箱偏好保存失败")); }, Qt::QueuedConnection);
            std::lock_guard<std::mutex> lock(writer->mutex);
            if (writer->revision == revision) { writer->running = false; break; }
        }
    }); timer_->start();
}
