#include "ui/pages/toolbox_workspace.h"
#include "application/toolbox/toolbox_service.h"
#include "core/toolbox/toolbox_contract.h"
#include "ui/theme.h"
#include "ui/widgets/toolbox_plot.h"
#include "application/agent/agent_connection_service.h"
#include "application/agent/agent_service_status.h"
#include "core/protocol/agent_version.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyleOptionSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <atomic>
#include <iostream>
#include <thread>

using namespace testtools;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool waitFor(const std::function<bool()>& condition, int timeout = 5000)
{
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < timeout) QTest::qWait(10);
    return condition();
}
struct Backend {
    std::atomic_bool ethernet{false}, clientDone{false}, gpioUnavailable{false}, noAddress{false}, externallyClaimed{false};
    std::atomic_bool boardAvailable{false}, boardWritable{true};
    int boardLevel = 0;
    JsonValue arguments = JsonValue::object();
    std::atomic<int> starts{0};
    std::string capability, operation, job;
};
class Connection final : public IAgentConnection {
  public:
    explicit Connection(std::shared_ptr<Backend> value) : backend(std::move(value)) {}
    std::shared_ptr<Backend> backend;
    bool connect(const std::string&, std::uint16_t, std::string&) override { return true; }
    void close() override {}
    bool request(const std::string& input, std::string& output, std::string& error) override
    {
        JsonValue request; if (!parseJson(input, request, error)) return false;
        const auto method = jsonString(request, "method"); const auto& params = *request.find("params");
        JsonValue result = JsonValue::object();
        if (method == "toolbox.info") result = JsonValue::object({{"schema", JsonValue::string("toolbox.info/v1")}, {"instance_id", JsonValue::string(std::string(32, 'a'))}, {"runner_available", JsonValue::boolean(true)}});
        else if (method == "gpio.chip.list") {
            JsonValue::Array pins;
            for (int i = 0; !backend->gpioUnavailable && i < 16; ++i) pins.push_back(JsonValue::object({{"chip", JsonValue::string("gpiochip4")}, {"offset", JsonValue::integer(i)},
                {"name", JsonValue::string(i == 0 ? "occupied" : "test-input")}, {"direction", JsonValue::string("input")}, {"abi", JsonValue::integer(2)},
                {"global_number", JsonValue::integer(128 + i)}, {"used", JsonValue::boolean(i == 0 || (i == 1 && backend->boardAvailable) || (i == 15 && backend->externallyClaimed))}, {"consumer", JsonValue::string(i == 0 ? "kernel-driver" : i == 15 && backend->externallyClaimed ? "external-fixture" : "")}, {"active_low", JsonValue::boolean(false)}}));
            result = JsonValue::object({{"schema", JsonValue::string("gpio.inventory/v1")}, {"pins", JsonValue::array(std::move(pins))},
                {"chips", JsonValue::array({JsonValue::object({{"id", JsonValue::string("gpiochip4")}, {"name", JsonValue::string("gpiochip4")}, {"label", JsonValue::string("gpio4")}})})},
                {"next_cursor", JsonValue::integer(backend->gpioUnavailable ? 0 : 16)}, {"total", JsonValue::integer(backend->gpioUnavailable ? 0 : 16)},
                {"complete", JsonValue::boolean(!backend->gpioUnavailable)}, {"unavailable", JsonValue::string(backend->gpioUnavailable ? "fixture GPIO permission denied" : "")}});
        } else if (method == "gpio.board.list") {
            JsonValue::Array endpoints;
            if (backend->boardAvailable) for (int i = 0; i < 2; ++i) endpoints.push_back(JsonValue::object({
                {"source", JsonValue::string("board")}, {"endpoint_id", JsonValue::string(std::string(32, i ? 'c' : 'b'))},
                {"adapter", JsonValue::string("rocktech-gpio/v1")}, {"name", JsonValue::string(i ? "otg_drv_vbus" : "gpio1")},
                {"chip", JsonValue::string("gpiochip4")}, {"offset", JsonValue::integer(i)}, {"global_number", JsonValue::integer(128 + i)},
                {"rockchip_name", JsonValue::string(i ? "GPIO4_A1" : "GPIO4_A0")}, {"controller_identity", JsonValue::string("gpio4/gpiochip4")},
                {"preference_key", JsonValue::string(i ? "board:virtual/otg_drv_vbus/value" : "board:virtual/gpio1/value")},
                {"path", JsonValue::string(i ? "/sys/class/rock_gpio/otg_drv_vbus/value" : "/sys/class/rock_gpio/gpio1/value")},
                {"driver", JsonValue::string("rocktech_gpio")}, {"used", JsonValue::boolean(true)}, {"direction", JsonValue::string("output")},
                {"consumer", JsonValue::string(i ? "otg_drv_vbus" : "gpio1")}, {"readable", JsonValue::boolean(true)},
                {"writable", JsonValue::boolean(!i && backend->boardWritable)}, {"value_semantics", JsonValue::string("physical")},
                {"reason", JsonValue::string(i ? "该用途节点当前仅提供读取" : backend->boardWritable ? "" : "接口访问权限不足")}}));
            result = JsonValue::object({{"schema", JsonValue::string("gpio.board.inventory/v1")}, {"endpoints", JsonValue::array(std::move(endpoints))}});
        } else if (method == "network.interface.list") {
            JsonValue::Array interfaces;
            if (backend->ethernet) interfaces.push_back(JsonValue::object({{"id", JsonValue::string("eth0")}, {"up", JsonValue::boolean(true)},
                {"physical", JsonValue::boolean(true)}, {"operstate", JsonValue::string("up")}, {"speed_mbps", JsonValue::integer(1000)},
                {"ipv4", backend->noAddress ? JsonValue::array() : JsonValue::array({JsonValue::string("192.0.2.2")})}}));
            result = JsonValue::object({{"schema", JsonValue::string("network.inventory/v1")}, {"interfaces", JsonValue::array(std::move(interfaces))}});
        } else if (method == "toolbox.prepare") result = JsonValue::object({{"operation_id", JsonValue::string("op-" + std::to_string(backend->starts + 1))}, {"instance_id", JsonValue::string(std::string(32, 'a'))}});
        else if (method == "job.start" || method == "job.progress" || method == "job.cancel") {
            if (method == "job.start") {
                backend->capability = jsonString(params, "capability_id"); backend->operation = jsonString(params, "operation_id");
                backend->job = "job-" + std::to_string(++backend->starts); backend->clientDone = false;
                backend->arguments = *params.find("arguments");
                if (backend->capability == "gpio.board.set") backend->boardLevel = static_cast<int>(jsonInteger(backend->arguments, "level"));
            }
            const bool active = backend->capability == "network.iperf3.server" && !backend->clientDone;
            result = JsonValue::object({{"schema", JsonValue::string("toolbox.job/v1")}, {"instance_id", JsonValue::string(std::string(32, 'a'))},
                {"operation_id", JsonValue::string(backend->operation)}, {"job_id", JsonValue::string(backend->job)}, {"capability_id", JsonValue::string(backend->capability)},
                {"active", JsonValue::boolean(active)}, {"complete", JsonValue::boolean(true)}, {"state", JsonValue::string(active ? "running" : "succeeded")},
                {"stage", JsonValue::string(active ? "ready" : "completed")}, {"data", JsonValue::object({{"physical_level", JsonValue::integer(0)}})},
                {"events", JsonValue::array()}, {"next_cursor", JsonValue::integer(0)},
                {"result", active ? JsonValue::null() : JsonValue::object({{"observations", JsonValue::object({
                    {"physical_level", JsonValue::integer(backend->capability.rfind("gpio.board.", 0) == 0 ? backend->boardLevel : 0)},
                    {"write_completed", JsonValue::boolean(backend->capability == "gpio.board.set")},
                    {"released", JsonValue::boolean(backend->capability.rfind("gpio.board.", 0) != 0)}})}})}});
        } else if (method == "job.renew") result = JsonValue::object({{"renewed", JsonValue::boolean(true)}});
        else { error = "unexpected fixture RPC"; return false; }
        output = serializeJson(JsonValue::object({{"protocol", JsonValue::integer(1)}, {"request_id", JsonValue::string(jsonString(request, "request_id"))},
            {"ok", JsonValue::boolean(true)}, {"result", result}})); return true;
    }
};
class Host final : public IToolboxHost {
  public:
    explicit Host(std::shared_ptr<Backend> value) : backend(std::move(value)) {}
    std::shared_ptr<Backend> backend;
    JsonValue interfaces() override { return JsonValue::object({{"interfaces", JsonValue::array({JsonValue::object({
        {"index", JsonValue::integer(1)}, {"name", JsonValue::string("Fixture Ethernet")}, {"id", JsonValue::string("fixture-ethernet")},
        {"up", JsonValue::boolean(true)}, {"physical", JsonValue::boolean(true)}, {"speed_bps", JsonValue::integer(1000000000)},
        {"ipv4", JsonValue::array({JsonValue::string("192.0.2.1")})}})})}}); }
    bool available(std::string&) override { return true; }
    bool validateRoute(std::uint32_t, const std::string&, const std::string&, std::string&) override { return true; }
    ToolProcessResult run(const std::vector<std::string>&, int, const std::atomic_bool&, const std::function<bool(const char*, std::size_t)>& sink) override
    {
        const std::string start = "{\"event\":\"start\",\"data\":{\"connected\":[{\"local_host\":\"192.0.2.1\",\"remote_host\":\"192.0.2.2\"}]}}\n{\"event\":\"interval\",\"data\":{\"sum\":{\"bits_per_second\":935000000.5,\"end\":1.0}}}\n";
        bool ok = sink(start.data(), start.size()); std::this_thread::sleep_for(std::chrono::milliseconds(350));
        const std::string end = "{\"event\":\"end\",\"data\":{\"sum_sent\":{\"bits_per_second\":935000000.5,\"retransmits\":0},\"sum_received\":{\"bits_per_second\":934000000.3}}}\n";
        ok = sink(end.data(), end.size()) && ok; backend->clientDone = true;
        return {true, false, !ok, ok ? 0 : -1, {}};
    }
};
}
int liveNetworkCheck(const QString& serial, int port, const QString& runtime, const QString& output, bool inspectOnly)
{
    auto service = std::make_shared<ToolboxService>(AgentConnectionService::createToolboxHost(runtime.toUtf8().toStdString()));
    QWidget owner; ToolboxWorkspace workspace(&owner, service); auto* page = workspace.networkPage();
    owner.resize(1200, 760); workspace.gpioPage()->hide(); page->resize(owner.size()); owner.show(); page->show();
    workspace.setTarget({serial.toStdString(), "device", "MTB-720"});
    ToolboxSession session; session.serial = serial.toStdString(); session.generation = 1;
    session.openConnection = [port](std::string& error) -> std::shared_ptr<IAgentConnection> {
        auto client = AgentConnectionService::createClient();
        if (!client->connect("127.0.0.1", static_cast<std::uint16_t>(port), error) || !probeAgentService(*client, AgentVersion, error)) { client->close(); return {}; }
        return client;
    };
    workspace.setSession(session, {"toolbox.v1", "gpio.pin.read", "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "network.iperf3.server"}); workspace.setVisible(false, true);
    auto* pc = page->findChild<QComboBox*>("networkPcInterface"); auto* board = page->findChild<QComboBox*>("networkBoardInterface");
    check(waitFor([&] { return pc->count() && board->count(); }, 20000), "real Ethernet metadata missing");
    auto select = [](QComboBox* combo, const std::string& ip) {
        for (int i = 0; i < combo->count(); ++i) { JsonValue value; std::string error;
            if (parseJson(combo->itemData(i).toString().toStdString(), value, error) && jsonString(value, "selected_ip") == ip) { combo->setCurrentIndex(i); return true; }
        }
        return false;
    };
    check(select(pc, "192.168.1.211") && select(board, "192.168.1.111"), "intended LAN addresses are unavailable; no alternate path will be used");
    const auto* plot = static_cast<ToolboxPlot*>(page->findChild<QWidget*>("networkThroughputPlot"));
    try {
        for (int udp = 0; !inspectOnly && udp < 2; ++udp) {
            const QString name = udp ? "udp" : "tcp";
            auto* protocol = page->findChild<QComboBox*>("networkProtocol"); protocol->setCurrentIndex(protocol->findData(udp ? "udp" : "tcp"));
            page->findChild<QSpinBox*>("networkDuration")->setValue(udp ? 5 : 10);
            page->findChild<QSpinBox*>("networkPort")->setValue(5209);
            page->findChild<QSpinBox*>("networkUdpRate")->setValue(20);
            auto* start = page->findChild<QPushButton*>("networkStart"); check(start->isEnabled(), "real Ethernet start gate is closed"); start->click();
            QElapsedTimer elapsed; elapsed.start(); bool captured = false;
            while (service->snapshot().active && elapsed.elapsed() < 60000) {
                QTest::qWait(40);
                if (!captured && plot->sampleCount(0) >= 6) { page->grab().save(output + "/live-" + name + "-running.png"); captured = true; }
            }
            const auto state = service->snapshot();
            JsonLimits limits; limits.max_output_bytes = 8 * 1024 * 1024; limits.max_string_bytes = 4 * 1024 * 1024;
            const auto data = QByteArray::fromStdString(serializeJson(state.report, limits));
            QFile file(output + "/live-" + name + "-report.json"); check(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "live report save failed"); file.close();
            check(waitFor([&] { return !service->snapshot().active; }, 10000), "real network task did not stop");
            QTest::qWait(200); page->grab().save(output + "/live-" + name + "-result.png");
            if (state.state != "succeeded") throw std::runtime_error(name.toStdString() + ": " + state.detail);
            check(plot->sampleCount(0) >= (udp ? 8 : 18) && plot->sampleCount(1) >= (udp ? 8 : 18), "real samples were lost in the UI chart");
            page->findChild<QCheckBox*>("networkPlotZoom")->setChecked(true); QTest::qWait(50); page->grab().save(output + "/live-" + name + "-zoom.png");
            page->findChild<QCheckBox*>("networkPlotZoom")->setChecked(false);
            std::cout << name.toStdString() << " succeeded; chart samples=" << plot->sampleCount(0) << "/" << plot->sampleCount(1) << '\n';
        }
    } catch (...) {
        service->cancel(); workspace.shutdown(); waitFor([&] { return !service->snapshot().active; }, 15000); throw;
    }
    if (inspectOnly) {
        page->grab().save(output + "/live-network-idle.png"); page->hide(); auto* gpio = workspace.gpioPage(); gpio->resize(owner.size()); gpio->show(); workspace.setVisible(true, false);
        gpio->findChild<QLineEdit*>("gpioLookup")->setText("108"); gpio->findChild<QPushButton*>("gpioLocate")->click(); QTest::qWait(80);
        check(gpio->findChild<QLabel*>("gpioConversionResult")->text().contains("108"), "live GPIO mapping conversion missing");
        gpio->grab().save(output + "/live-gpio-conversion.png");
    }
    bool closed = false; QObject::connect(&workspace, &ToolboxWorkspace::shutdownFinished, [&] { closed = true; }); workspace.shutdown();
    check(waitFor([&] { return closed; }), "live workspace did not drain workers");
    return 0;
}

int liveBoardCheck(const QString& serial, int port, const QString& runtime, const QString& output)
{
    auto service = std::make_shared<ToolboxService>(AgentConnectionService::createToolboxHost(runtime.toUtf8().toStdString()));
    QWidget owner; ToolboxWorkspace workspace(&owner, service); auto* page = workspace.gpioPage(); workspace.networkPage()->hide();
    owner.resize(1366, 768); page->resize(owner.size()); owner.show(); page->show();
    workspace.setTarget({serial.toStdString(), "device", "Board GPIO"});
    ToolboxSession session; session.serial = serial.toStdString(); session.generation = 1;
    session.openConnection = [port](std::string& error) -> std::shared_ptr<IAgentConnection> {
        auto connection = AgentConnectionService::createClient();
        if (!connection->connect("127.0.0.1", static_cast<std::uint16_t>(port), error) || !probeAgentService(*connection, AgentVersion, error)) return {};
        return connection;
    };
    workspace.setSession(session, {"toolbox.v1", "gpio.pin.read", "gpio.board.list", "gpio.board.read", "gpio.board.set"}); workspace.setVisible(true, false);
    auto* pins = page->findChild<QTreeWidget*>("gpioPinList");
    check(waitFor([&] { return pins->topLevelItemCount() > 0 && page->findChild<QPushButton*>("gpioRefresh")->isEnabled(); }, 10000), "live board GPIO inventory or preferences missing");
    int selection = -1, endpoints = 0;
    for (int row = 0; row < pins->topLevelItemCount(); ++row) {
        JsonValue pin; std::string error;
        if (parseJson(pins->topLevelItem(row)->data(0, Qt::UserRole).toString().toUtf8().toStdString(), pin, error) && jsonString(pin, "read_operation") == "gpio.board.read") {
            ++endpoints; if (selection < 0) selection = row;
        }
    }
    check(endpoints == (serial == "66c8c84745c4330" ? 11 : 1) && selection >= 0, "unified inventory lost adapted operations");
    pins->setCurrentItem(pins->topLevelItem(selection));
    auto* read = page->findChild<QPushButton*>("gpioRead"); check(read->isEnabled(), "driver-owned live GPIO was not readable"); read->click();
    check(waitFor([&] { return service->snapshot().id && !service->snapshot().active; }, 10000), "live board read did not finish");
    check(service->snapshot().state == "succeeded", "live board GPIO read failed");
    check(service->snapshot().capability == "gpio.board.read", "live UI read used native GPIO instead of company interface");
    check(waitFor([&] { return !page->findChild<QPushButton*>("gpioStop")->isEnabled() && page->findChild<QPushButton*>("gpioExport")->isEnabled(); }), "live board result was not rendered");
    QFile report(output + "/live-board-read.json"); check(report.open(QIODevice::WriteOnly), "cannot save live board read report");
    report.write(QByteArray::fromStdString(serializeJson(service->snapshot().report))); report.close();
    for (const auto& size : {QSize(1366, 768), QSize(1920, 1080)}) {
        owner.resize(size); page->resize(size); QTest::qWait(80);
        check(page->grab().save(output + QString("/live-board-%1x%2.png").arg(size.width()).arg(size.height())), "live board snapshot failed");
    }
    bool closed = false; QObject::connect(&workspace, &ToolboxWorkspace::shutdownFinished, [&] { closed = true; });
    workspace.shutdown(); check(waitFor([&] { return closed; }), "live board UI did not stop cleanly");
    std::cout << "Live board GPIO read passed; endpoints=" << pins->topLevelItemCount() << "; no writes.\n"; return 0;
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setStyleSheet(testtools::ui::applicationStyleSheet());
    QTemporaryDir temporary;
    qputenv("TESTTOOLS_TOOLBOX_PREFERENCES_DIRECTORY", temporary.path().toUtf8());
    const auto output = qEnvironmentVariable("TESTTOOLS_TEST_OUTPUT", temporary.path()); QDir().mkpath(output);
    try {
        if (argc == 6 && std::string(argv[1]) == "--live-board-inspect") {
            QDir().mkpath(QString::fromUtf8(argv[5]));
            return liveBoardCheck(QString::fromUtf8(argv[2]), std::stoi(argv[3]), QString::fromUtf8(argv[4]), QString::fromUtf8(argv[5]));
        }
        if (argc == 6 && (std::string(argv[1]) == "--live-network" || std::string(argv[1]) == "--live-inspect")) {
            QDir().mkpath(QString::fromUtf8(argv[5]));
            return liveNetworkCheck(QString::fromUtf8(argv[2]), std::stoi(argv[3]), QString::fromUtf8(argv[4]), QString::fromUtf8(argv[5]), std::string(argv[1]) == "--live-inspect");
        }
        if (argc == 4 && std::string(argv[1]) == "--chart-report") {
            QFile file(QString::fromUtf8(argv[2])); check(file.open(QIODevice::ReadOnly), "report file open failed"); JsonValue report; std::string error;
            check(parseJson(file.readAll().toStdString(), report, error), "report JSON invalid");
            ToolboxPlot plot; plot.resize(1100, 340); plot.clear(static_cast<int>(jsonInteger(*report.find("arguments"), "duration_sec", 10))); plot.show();
            for (const auto& run : report.find("runs")->arrayItems()) {
                std::vector<QPointF> points; for (const auto& sample : run.find("samples")->arrayItems()) points.emplace_back(jsonInteger(sample, "elapsed_ms") / 1000.0, jsonInteger(sample, "bps") / 1000000.0);
                plot.setSamples(jsonString(run, "direction") == "board_to_pc" ? 1 : 0, points);
            }
            QTest::qWait(30); check(plot.grab().save(QString::fromUtf8(argv[3])), "report chart save failed");
            plot.findChild<QCheckBox*>("networkPlotZoom")->setChecked(true); QTest::qWait(20); check(plot.grab().save(QString::fromUtf8(argv[3]) + ".zoom.png"), "zoom report chart save failed");
            return 0;
        }
        auto backend = std::make_shared<Backend>(); auto service = std::make_shared<ToolboxService>(std::make_shared<Host>(backend));
        QWidget owner; ToolboxWorkspace workspace(&owner, service);
        auto* gpio = workspace.gpioPage(); auto* network = workspace.networkPage();
        check(!gpio->findChild<QComboBox*>("gpioSource"), "GPIO requires choosing an access source");
        owner.resize(1140, 630); gpio->resize(1140, 630); network->resize(1140, 630);
        network->hide(); owner.show();
        check(!gpio->findChild<QPushButton*>("gpioRead")->isEnabled(), "GPIO read enabled without service");
        gpio->findChild<QLineEdit*>("gpioLookup")->setText("108"); gpio->findChild<QPushButton*>("gpioLocate")->click();
        check(gpio->findChild<QLabel*>("gpioConversionResult")->text().contains("GPIO3_B4"), "offline GPIO conversion missing");
        workspace.setTarget({"fixture-device", "device", "Fixture"});
        ToolboxSession session; session.serial = "fixture-device"; session.generation = 1;
        session.openConnection = [backend](std::string&) { return std::make_shared<Connection>(backend); };
        workspace.setSession(session, {"toolbox.v1", "gpio.pin.read", "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "network.iperf3.server"}); workspace.setVisible(true, false);
        auto* duration = network->findChild<QSpinBox*>("networkDuration");
        gpio->hide(); network->show(); QTest::qWait(40);
        check(duration->width() <= 130 && duration->suffix() == QStringLiteral(" 秒"), "duration stretches or repeats direction unit");
        duration->setValue(10);
        QStyleOptionSpinBox option; option.initFrom(duration); option.rect = duration->rect(); option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
        const auto up = duration->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, duration);
        const auto down = duration->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, duration);
        QTest::mouseClick(duration, Qt::LeftButton, Qt::NoModifier, up.center()); check(duration->value() == 11, "visible up button cannot increment");
        QTest::mouseClick(duration, Qt::LeftButton, Qt::NoModifier, down.center()); check(duration->value() == 10, "visible down button cannot decrement");
        duration->setFocus(); duration->selectAll(); QTest::keyClicks(duration, "60"); QTest::keyClick(duration, Qt::Key_Enter);
        QTest::keyClick(duration, Qt::Key_Up); check(duration->value() == 60, "manual entry or upper bound failed");
        duration->setValue(1); QTest::keyClick(duration, Qt::Key_Down); check(duration->value() == 1, "lower bound failed");
        auto* networkDirection = network->findChild<QComboBox*>("networkDirection");
        networkDirection->setFocus(); QTest::keyClick(networkDirection, Qt::Key_Tab);
        check(network->findChild<QComboBox*>("networkProtocol")->hasFocus(), "Tab order does not follow parameter groups");
        QTest::keyClick(network->findChild<QComboBox*>("networkProtocol"), Qt::Key_Tab);
        check(duration->hasFocus(), "duration missing from keyboard order");
        duration->setValue(10); duration->setEnabled(false); QTest::mouseClick(duration, Qt::LeftButton, Qt::NoModifier, up.center()); check(duration->value() == 10, "disabled spinbox changed");
        duration->setEnabled(true); network->hide(); gpio->show();
        auto* pins = gpio->findChild<QTreeWidget*>("gpioPinList");
        check(waitFor([&] { return pins->topLevelItemCount() == 16; }), "GPIO inventory missing");
        pins->setCurrentItem(pins->topLevelItem(0));
        check(!gpio->findChild<QPushButton*>("gpioRead")->isEnabled(), "occupied GPIO read enabled");
        pins->setCurrentItem(pins->topLevelItem(15));
        auto* read = gpio->findChild<QPushButton*>("gpioRead");
        check(read->isEnabled() && gpio->findChild<QPushButton*>("gpioWrite")->isEnabled(), "GPIO control still requires favorites");
        check(!network->findChild<QPushButton*>("networkStart")->isEnabled(), "network enabled without board Ethernet");
        QTimer::singleShot(20, [] { for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) box->reject(); });
        gpio->findChild<QPushButton*>("gpioWrite")->click(); check(backend->starts == 0, "output without favorite bypassed confirmation cancellation");
        QTimer::singleShot(20, [] { for (auto* widget : QApplication::topLevelWidgets()) if (auto* dialog = qobject_cast<QInputDialog*>(widget)) { dialog->setTextValue(QStringLiteral("空闲测试引脚")); dialog->accept(); } });
        gpio->findChild<QPushButton*>("gpioFavorite")->click();
        check(gpio->findChild<QPushButton*>("gpioWrite")->isEnabled(), "favorite was not persisted in model");
        QTimer::singleShot(20, [] { for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) box->reject(); });
        gpio->findChild<QPushButton*>("gpioWrite")->click(); check(backend->starts == 0, "cancelled confirmation sent GPIO output");
        read->click();
        check(waitFor([&] { return service->snapshot().id && !service->snapshot().active; }), "read did not finish");
        check(waitFor([&] { return gpio->findChild<QPushButton*>("gpioExport")->isEnabled(); }), "completed report unavailable");
        backend->externallyClaimed = true;
        check(waitFor([&] { return !read->isEnabled() && pins->currentItem() && pins->currentItem()->text(3) == "external-fixture"; }, 5000),
              "external GPIO occupancy did not refresh automatically");
        backend->externallyClaimed = false; gpio->findChild<QPushButton*>("gpioRefresh")->click();
        check(waitFor([&] { return read->isEnabled(); }), "GPIO release was not reflected after refresh");
        gpio->show(); QTest::qWait(50); check(gpio->grab().save(output + "/toolbox-gpio-fixture.png"), "GPIO snapshot failed"); gpio->hide();
        backend->ethernet = true; backend->noAddress = true; network->findChild<QPushButton*>("networkRefresh")->click();
        check(waitFor([&] { return network->findChild<QComboBox*>("networkBoardInterface")->count() == 1; }), "unaddressed Ethernet disappeared");
        check(network->findChild<QComboBox*>("networkBoardInterface")->currentIndex() == 0 && !network->findChild<QPushButton*>("networkStart")->isEnabled() &&
            network->findChild<QLabel*>("networkToolStatus")->text().contains("IPv4"), "unaddressed Ethernet selection or start gate incorrect");
        backend->noAddress = false; network->findChild<QPushButton*>("networkRefresh")->click();
        check(waitFor([&] { return network->findChild<QPushButton*>("networkStart")->isEnabled(); }), "network path controls unavailable");
        network->show(); workspace.setVisible(false, true); network->findChild<QPushButton*>("networkStart")->click();
        check(waitFor([&] { return !service->snapshot().active && service->snapshot().state == "succeeded"; }), "network UI fixture did not finish");
        check(waitFor([&] { return network->findChild<QPushButton*>("networkExport")->isEnabled(); }), "network report unavailable");
        QTest::qWait(40); check(network->grab().save(output + "/toolbox-network-fixture.png"), "network snapshot failed");
        auto* parameterScroll = network->findChild<QScrollArea*>("networkParametersScroll");
        const int collapsedScrollMaximum = parameterScroll->verticalScrollBar()->maximum();
        auto* advancedToggle = network->findChild<QToolButton*>("networkAdvancedParameters");
        check(network->rect().contains(QRect(advancedToggle->mapTo(network, QPoint()), advancedToggle->size())), "advanced parameter entry is clipped");
        advancedToggle->click();
        check(waitFor([&] { return !network->findChild<QWidget*>("networkPort")->isHidden() && network->findChild<QWidget*>("networkPort")->parentWidget()->isVisible(); }), "advanced fields did not expand");
        QTest::qWait(40);
        auto* startButton = network->findChild<QPushButton*>("networkStart");
        check(network->height() == 630 && network->rect().contains(QRect(startButton->mapTo(network, QPoint()), startButton->size())),
              "expanded advanced settings clipped the primary action");
        auto* boardField = network->findChild<QWidget*>("networkBoardInterface");
        const auto boardRight = boardField->mapTo(network, boardField->rect().topRight()).x();
        const auto durationRight = duration->mapTo(network, duration->rect().topRight()).x();
        check(std::abs(boardRight - durationRight) <= 2, "duration right edge does not align with address fields");
        int previousRight = -1;
        for (const auto* name : {"networkDirection", "networkProtocol", "networkDuration"}) {
            const auto* field = network->findChild<QWidget*>(name); const auto position = field->mapTo(network, QPoint());
            check(field->height() >= 28 && position.x() > previousRight, "basic parameter controls overlap");
            check(std::abs(position.y() + field->height() / 2 - duration->mapTo(network, QPoint()).y() - duration->height() / 2) <= 1, "basic parameters are not aligned in one row");
            previousRight = position.x() + field->width() - 1;
        }
        previousRight = -1;
        for (const auto* name : {"networkParallel", "networkUdpRate", "networkPort"}) {
            const auto* field = network->findChild<QWidget*>(name); const auto position = field->mapTo(network, QPoint());
            check(field->height() >= 28 && position.x() > previousRight, "advanced parameter groups overlap");
            previousRight = position.x() + field->width() - 1;
        }
        auto* portField = network->findChild<QWidget*>("networkPort"); parameterScroll->ensureWidgetVisible(portField); QTest::qWait(20);
        check(parameterScroll->viewport()->rect().contains(QRect(portField->mapTo(parameterScroll->viewport(), QPoint()), portField->size())),
              "advanced port field was not reachable through scrolling");
        check(network->grab().save(output + "/toolbox-network-advanced-fixture.png"), "advanced network snapshot failed");
        advancedToggle->click(); check(waitFor([&] { return parameterScroll->verticalScrollBar()->maximum() == collapsedScrollMaximum; }), "collapsed settings did not restore their height");
        const auto* plot = static_cast<ToolboxPlot*>(network->findChild<QWidget*>("networkThroughputPlot"));
        check(plot->sampleCount(0) >= 1 && plot->sampleCount(1) >= 1, "sequential directions lost a chart series");
        network->findChild<QCheckBox*>("networkPlotZoom")->setChecked(true);
        check(backend->starts == 3, "unexpected number of hardware/test start requests");
        backend->boardAvailable = true; ++session.generation;
        workspace.setSession(session, {"toolbox.v1", "gpio.pin.read", "gpio.pin.write", "gpio.pin.pulse", "gpio.edge.monitor", "gpio.board.list", "gpio.board.read", "gpio.board.set", "network.iperf3.server"});
        check(waitFor([&] { return pins->topLevelItemCount() == 16 && pins->topLevelItem(0)->text(1).contains("gpio1"); }), "unified GPIO lost adapted name or duplicated physical rows");
        network->hide(); gpio->show(); workspace.setVisible(true, false);
        pins->setCurrentItem(pins->topLevelItem(0));
        check(read->isEnabled() && gpio->findChild<QPushButton*>("gpioWrite")->isEnabled() && gpio->findChild<QPushButton*>("gpioWrite")->text().contains(QStringLiteral("设置")),
            "driver-owned board GPIO control was blocked by native occupancy");
        check(!gpio->findChild<QPushButton*>("gpioPulse")->isEnabled() && !gpio->findChild<QPushButton*>("gpioMonitor")->isEnabled() &&
            !gpio->findChild<QSpinBox*>("gpioHoldSeconds")->isVisible(), "board GPIO exposed unsupported timing or input operations");
        const auto beforeRead = service->snapshot().id; read->click();
        check(waitFor([&] { return service->snapshot().id > beforeRead && !service->snapshot().active; }), "board GPIO read did not finish");
        check(backend->capability == "gpio.board.read" && backend->arguments.find("endpoint_id") && !backend->arguments.find("path"), "board read did not use fixed endpoint capability");
        check(waitFor([&] { return gpio->findChild<QPushButton*>("gpioWrite")->isEnabled(); }), "board controls did not unlock after read");
        const auto startsBeforeBoardSet = backend->starts.load();
        QTimer::singleShot(20, [] { for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) box->reject(); });
        gpio->findChild<QPushButton*>("gpioWrite")->click(); check(backend->starts == startsBeforeBoardSet, "board set ignored confirmation cancellation");
        gpio->findChild<QComboBox*>("gpioOutputLevel")->setCurrentIndex(1);
        bool boardConfirmationAccurate = false;
        QTimer::singleShot(20, [&] { for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) {
            boardConfirmationAccurate = box->text().contains(QStringLiteral("持续生效")) && box->text().contains("gpio1") && !box->text().contains(QStringLiteral("保持 30 秒"));
            box->done(QMessageBox::Ok);
        } });
        const auto beforeSet = service->snapshot().id; gpio->findChild<QPushButton*>("gpioWrite")->click();
        check(waitFor([&] { return service->snapshot().id > beforeSet && !service->snapshot().active; }), "board set did not finish");
        check(boardConfirmationAccurate && backend->capability == "gpio.board.set" && jsonInteger(backend->arguments, "level") == 1 &&
            !backend->arguments.find("hold_sec") && !backend->arguments.find("drive"), "board output used native hold/drive semantics");
        check(waitFor([&] { return gpio->findChild<QLabel*>("gpioToolResult")->text().contains(QStringLiteral("持续生效")); }), "persistent board output result missing");
        check(gpio->grab().save(output + "/toolbox-board-gpio-fixture.png"), "board GPIO screenshot failed");
        for (const auto& size : {QSize(1366, 768), QSize(1920, 1080)}) {
            owner.resize(size); gpio->resize(size); QTest::qWait(20);
            auto* action = gpio->findChild<QPushButton*>("gpioWrite");
            check(gpio->rect().contains(QRect(action->mapTo(gpio, QPoint()), action->size())), "board GPIO action was clipped");
            check(gpio->grab().save(output + QString("/toolbox-board-gpio-%1x%2.png").arg(size.width()).arg(size.height())), "board GPIO resolution screenshot failed");
        }
        owner.resize(1140, 630); gpio->resize(1140, 630);
        pins->setCurrentItem(pins->topLevelItem(1));
        check(read->isEnabled() && !gpio->findChild<QPushButton*>("gpioWrite")->isEnabled() && gpio->findChild<QLabel*>("gpioBoardHint")->text().contains(QStringLiteral("只读")), "protected-purpose board GPIO allowed writing");
        gpio->findChild<QLineEdit*>("gpioFilter")->setText("gpio1");
        check(!pins->topLevelItem(0)->isHidden(), "name search lost adapted alias");
        gpio->findChild<QLineEdit*>("gpioLookup")->setText("GPIO4_A2"); gpio->findChild<QPushButton*>("gpioLocate")->click();
        check(pins->currentItem() && pins->currentItem()->text(0).contains("GPIO4_A2"), "unified lookup missed native GPIO");
        ++session.generation;
        workspace.setSession(session, {"toolbox.v1", "gpio.pin.read", "network.iperf3.server"});
        check(waitFor([&] { return pins->topLevelItemCount() == 16 && !pins->topLevelItem(0)->text(1).contains("gpio1"); }), "old Agent lost native inventory");
        pins->setCurrentItem(pins->topLevelItem(0));
        check(!read->isEnabled(), "missing adapter bypassed occupancy protection");
        backend->gpioUnavailable = true; gpio->findChild<QPushButton*>("gpioRefresh")->click();
        check(waitFor([&] { return pins->topLevelItemCount() == 0 && gpio->findChild<QLabel*>("gpioToolStatus")->text().contains(QStringLiteral("不可用")); }),
              "unavailable GPIO interface had no actionable status");
        check(!gpio->findChild<QPushButton*>("gpioRead")->isEnabled() && network->findChild<QPushButton*>("networkStart")->isEnabled(),
              "GPIO unavailability did not preserve independent network availability");
        bool closed = false; QObject::connect(&workspace, &ToolboxWorkspace::shutdownFinished, [&] { closed = true; });
        workspace.shutdown(); check(waitFor([&] { return closed; }), "toolbox shutdown did not drain workers");
        std::cout << "Toolbox UI fixture passed; no real devices or network traffic.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
