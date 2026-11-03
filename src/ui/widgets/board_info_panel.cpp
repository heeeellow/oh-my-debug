#include "ui/widgets/board_info_panel.h"
#include "ui/icons.h"
#include "ui/theme.h"
#include <QtCore/QDateTime>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <map>

namespace {
using S = testtools::BoardInfoState;
QString text(const std::string& value)
{
    return testtools::ui::fromUtf8(value);
}
QString title(const std::string& id)
{
    return id == "memory.capacity"       ? QStringLiteral("系统可见内存")
           : id == "storage.device.info" ? QStringLiteral("存储设备信息")
                                         : QStringLiteral("存储健康与 I/O 计数");
}
QString stateName(S value)
{
    switch (value) {
    case S::NotRead:
        return QStringLiteral("未读取");
    case S::Unavailable:
        return QStringLiteral("未提供");
    case S::Reading:
        return QStringLiteral("正在读取");
    case S::Completed:
        return QStringLiteral("读取完成");
    case S::Cancelled:
        return QStringLiteral("已停止");
    case S::TimedOut:
        return QStringLiteral("读取超时");
    case S::Failed:
        return QStringLiteral("读取失败");
    }
    return {};
}
bool has(const std::vector<std::string>& values, const std::string& id)
{
    return std::find(values.begin(), values.end(), id) != values.end();
}
const std::vector<std::string>& dashboardCapabilities()
{
    // MemTotal 已在系统摘要显示，看板不重复发起内存任务。
    static const std::vector<std::string> ids{"storage.device.info", "storage.device.health"};
    return ids;
}
} // namespace
BoardInfoPanel::BoardInfoPanel(QWidget* parent,
                               std::shared_ptr<testtools::BoardInfoService> service)
    : QWidget(parent),
      service_(service ? std::move(service) : std::make_shared<testtools::BoardInfoService>())
{
    setObjectName("boardInfoPanel");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(9);
    grid->setColumnMinimumWidth(0, 76);
    grid->setColumnStretch(1, 1);
    const auto addRow = [&](int row, const QString& name, const char* object, QLabel*& value) {
        auto* caption = new QLabel(name, this);
        caption->setObjectName("cardEyebrow");
        value = new QLabel(QStringLiteral("—"), this);
        value->setObjectName(object);
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        grid->addWidget(caption, row, 0, Qt::AlignTop);
        grid->addWidget(value, row, 1);
    };
    addRow(0, QStringLiteral("存储容量"), "boardStorageSummary", storageSummary_);
    addRow(1, QStringLiteral("健康数据"), "boardHealthSummary", healthSummary_);
    layout->addLayout(grid);
    auto* controls = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setObjectName("boardInfoStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->setStyleSheet("color:#77716c;font-size:12px;");
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    controls->addWidget(status_, 1);
    all_ = new QPushButton(QStringLiteral("刷新"), this);
    all_->setObjectName("boardReadAll");
    all_->setIcon(testtools::ui::appIcon(testtools::ui::Icon::Refresh));
    all_->setIconSize({16, 16});
    controls->addWidget(all_);
    layout->addLayout(controls);
    connect(all_, &QPushButton::clicked, this, [this] {
        autoReadPending_ = false;
        start(dashboardCapabilities());
    });
    details_ = new QPushButton(QStringLiteral("存储详情…"), this);
    details_->setObjectName("boardInfoDetails");
    controls->addWidget(details_);
    connect(details_, &QPushButton::clicked, this, &BoardInfoPanel::showDetails);
    cancel_ = new QPushButton(QStringLiteral("停止"), this);
    cancel_->setObjectName("boardInfoCancel");
    controls->addWidget(cancel_);
    connect(cancel_, &QPushButton::clicked, this, [this] { cancelTask(service_->snapshot().id); });
    timer_ = new QTimer(this);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &BoardInfoPanel::poll);
    timer_->start();
    render();
    updateControls();
}
BoardInfoPanel::~BoardInfoPanel()
{
    if (session_.alive)
        *session_.alive = false;
    service_->cancel();
    tasks_.wait();
}
void BoardInfoPanel::setTarget(const serialdemo::AdbDevice& device)
{
    const bool changed = target_.serial != device.serial || device.state != "device";
    target_ = device;
    if (changed)
        clearSession();
    updateControls();
}
void BoardInfoPanel::clearSession()
{
    if (session_.alive)
        *session_.alive = false;
    service_->cancel();
    session_ = {};
    autoReadPending_ = true;
    rendered_.clear();
    render();
    updateControls();
}
void BoardInfoPanel::setDashboardVisible(bool visible)
{
    dashboardVisible_ = visible;
    maybeAutoRefresh();
}
void BoardInfoPanel::setSession(
    std::string serial,
    std::uint64_t generation,
    std::shared_ptr<testtools::IAgentConnection> connection,
    std::vector<std::string> capabilities,
    std::function<std::shared_ptr<testtools::IAgentConnection>(std::string&)> openConnection)
{
    if (session_.serial == serial && session_.generation == generation &&
        session_.connection == connection)
        return;
    clearSession();
    session_ = {std::move(serial),
                generation,
                std::move(connection),
                std::move(capabilities),
                std::make_shared<std::atomic<bool>>(true),
                std::move(openConnection)};
    rendered_.clear();
    render();
    updateControls();
    maybeAutoRefresh();
}
testtools::BoardInfoSnapshot BoardInfoPanel::snapshot() const
{
    return service_->snapshot();
}
bool BoardInfoPanel::busyForDevice(const std::string& serial) const
{
    const auto state = service_->snapshot();
    return state.serial == serial &&
           (state.active || (state.remotePending && state.generation == session_.generation &&
                             session_.alive && session_.alive->load()));
}
void BoardInfoPanel::shutdown()
{
    closing_ = true;
    service_->cancel();
    updateControls();
}
void BoardInfoPanel::cancelTask(std::uint64_t id)
{
    service_->cancel(id);
    updateControls();
}
bool BoardInfoPanel::start(const std::vector<std::string>& capabilities)
{
    if (closing_ || target_.state != "device" || target_.serial != session_.serial ||
        !session_.alive || !session_.alive->load())
        return false;
    testtools::BoardInfoRequest request;
    request.session = session_;
    request.capabilities = capabilities;
    std::string error;
    if (!service_->prepare(
            std::move(request), maintenanceBusy && maintenanceBusy(target_.serial), error)) {
        emit activityChanged(text(error));
        return false;
    }
    wasActive_ = true;
    rendered_.clear();
    render();
    updateControls();
    emit taskChanged();
    tasks_.start([service = service_] { service->executePrepared(); });
    return true;
}
void BoardInfoPanel::maybeAutoRefresh()
{
    if (!dashboardVisible_ || !autoReadPending_ || closing_ || service_->snapshot().active ||
        !session_.connection || !session_.alive || !session_.alive->load() ||
        (maintenanceBusy && maintenanceBusy(target_.serial)))
        return;
    const auto& ids = dashboardCapabilities();
    if (std::none_of(ids.begin(), ids.end(), [this](const auto& id) {
            return has(session_.capabilities, id);
        }))
        return;
    if (start(ids))
        autoReadPending_ = false;
}
void BoardInfoPanel::updateControls()
{
    const auto state = service_->snapshot();
    const bool ready = !closing_ && !state.active && target_.state == "device" &&
                       session_.serial == target_.serial && session_.connection && session_.alive &&
                       session_.alive->load() &&
                       !(maintenanceBusy && maintenanceBusy(target_.serial));
    bool any = false;
    for (const auto& id : dashboardCapabilities())
        any = any || has(session_.capabilities, id);
    all_->setEnabled(ready && any);
    all_->setToolTip(QStringLiteral("进入页面后自动加载；需要更新时可手动刷新"));
    cancel_->setVisible(state.active);
    cancel_->setEnabled(state.active && !state.cancelling && !closing_);
    details_->setEnabled(state.serial == target_.serial &&
                         state.generation == session_.generation && !state.records.empty());
    status_->setToolTip(text(state.serial) + QStringLiteral(" · ") + text(state.message));
    if (state.active)
        status_->setText(state.cancelling ? QStringLiteral("正在停止…")
                                          : QStringLiteral("正在加载存储信息…"));
    else if (!session_.connection)
        status_->setText(QStringLiteral("服务就绪后自动加载"));
    else if (state.serial == target_.serial && state.generation == session_.generation &&
             state.id) {
        std::int64_t updated = 0;
        for (const auto& row : state.records)
            updated = std::max(updated, row.finishedAtMs);
        const bool incomplete =
            state.remotePending ||
            std::any_of(state.records.begin(), state.records.end(), [](const auto& row) {
                return row.state == S::Failed || row.state == S::TimedOut ||
                       row.state == S::Cancelled;
            });
        status_->setText(incomplete ? QStringLiteral("部分数据未读取")
                         : updated
                             ? QStringLiteral("更新于 ") +
                                   QDateTime::fromMSecsSinceEpoch(updated).toString("HH:mm:ss")
                             : QStringLiteral("暂无存储数据"));
    } else
        status_->setText(any ? QStringLiteral("等待自动加载")
                             : QStringLiteral("服务未提供存储信息"));
}
void BoardInfoPanel::render()
{
    const auto state = service_->snapshot();
    const bool current = state.serial == target_.serial &&
                         state.generation == session_.generation && session_.alive &&
                         session_.alive->load();
    std::string signature = std::to_string(state.id) + (current ? ":current" : ":empty");
    if (current)
        for (const auto& row : state.records)
            signature += ":" + std::to_string(int(row.state)) + ":" +
                         std::to_string(row.fields.size()) + row.error;
    if (signature == rendered_)
        return;
    rendered_ = signature;
    for (const auto& id : dashboardCapabilities()) {
        const testtools::BoardInfoRecord* record = nullptr;
        if (current)
            for (const auto& row : state.records)
                if (row.capability == id) {
                    record = &row;
                    break;
                }
        auto* label = id == "storage.device.info" ? storageSummary_ : healthSummary_;
        QString summary = QStringLiteral("—");
        QString detail;
        if (record) {
            summary = stateName(record->state);
            detail = text(record->error);
            if (record->state == S::Completed) {
                if (id == "storage.device.info") {
                    std::vector<std::string> groups;
                    std::map<std::string, std::string> capacities, types;
                    for (const auto& field : record->fields) {
                        if (field.group.empty())
                            continue;
                        if (std::find(groups.begin(), groups.end(), field.group) == groups.end())
                            groups.push_back(field.group);
                        if (field.name == "设备容量")
                            capacities[field.group] = field.value;
                        if (field.name == "设备类型")
                            types[field.group] = field.value;
                    }
                    QStringList lines;
                    for (const auto& group : groups) {
                        QString type = text(types[group]);
                        if (type == "emmc")
                            type = QStringLiteral("eMMC");
                        else if (type == "ufs")
                            type = QStringLiteral("UFS");
                        if (type.isEmpty() || type == QStringLiteral("未知"))
                            type = text(group);
                        else if (groups.size() > 1)
                            type += " " + text(group).section(':', 1);
                        QString capacity = text(capacities[group]);
                        const int unit = capacity.indexOf("GiB");
                        if (unit >= 0)
                            capacity = capacity.left(unit + 3);
                        if (capacity.isEmpty())
                            capacity = QStringLiteral("未知");
                        lines << type + QStringLiteral(" · ") + capacity;
                        detail += text(group) + " · " + text(capacities[group]) + "\n";
                    }
                    summary = lines.isEmpty() ? QStringLiteral("未知") : lines.join('\n');
                } else {
                    bool available = false, missing = false;
                    for (const auto& field : record->fields) {
                        missing = missing || field.value == "未知";
                        available = available || (!field.group.empty() && field.value != "未知");
                    }
                    summary = !available ? QStringLiteral("未知")
                              : missing  ? QStringLiteral("已读取，部分字段未提供")
                                         : QStringLiteral("已读取");
                    detail = QStringLiteral(
                        "设备原始寿命字段与 I/O 计数可在存储详情查看；已读取不代表设备合格。");
                }
            }
        } else if (session_.connection && !has(session_.capabilities, id))
            summary = QStringLiteral("未提供");
        label->setText(summary);
        label->setToolTip(detail.trimmed());
        label->setStyleSheet(record && (record->state == S::Failed || record->state == S::TimedOut)
                                 ? "color:#a34a42;"
                                 : "");
    }
}
void BoardInfoPanel::populateDetails(QTreeWidget* values,
                                     const testtools::BoardInfoSnapshot& state) const
{
    const bool current = state.serial == target_.serial &&
                         state.generation == session_.generation && session_.alive &&
                         session_.alive->load();
    QSignalBlocker block(values);
    values->clear();
    for (const auto& id : dashboardCapabilities()) {
        const testtools::BoardInfoRecord* record = nullptr;
        if (current)
            for (const auto& row : state.records)
                if (row.capability == id) {
                    record = &row;
                    break;
                }
        const auto status = record ? record->state
                            : session_.connection && !has(session_.capabilities, id)
                                ? S::Unavailable
                                : S::NotRead;
        auto* item = new QTreeWidgetItem(values,
                                         {title(id),
                                          stateName(status),
                                          QStringLiteral("—"),
                                          record ? text(record->error) : QString()});
        item->setToolTip(3, record ? text(record->error) : QString());
        if (record && record->finishedAtMs)
            item->setToolTip(1,
                             QDateTime::fromMSecsSinceEpoch(record->finishedAtMs)
                                 .toString("yyyy-MM-dd HH:mm:ss"));
        if (status == S::Failed || status == S::TimedOut)
            item->setForeground(1, QColor("#a34a42"));
        if (!record)
            continue;
        std::map<std::string, QTreeWidgetItem*> groups;
        for (const auto& field : record->fields) {
            if (id == "memory.capacity" && field.group.empty()) {
                item->setText(2, text(field.value));
                item->setText(3, text(field.note));
                item->setToolTip(3, text(field.note));
                continue;
            }
            auto* parent = item;
            if (!field.group.empty()) {
                auto found = groups.find(field.group);
                if (found == groups.end())
                    found =
                        groups
                            .emplace(
                                field.group,
                                new QTreeWidgetItem(
                                    item, {text(field.group), {}, QStringLiteral("设备报告"), {}}))
                            .first;
                parent = found->second;
            }
            auto* value = new QTreeWidgetItem(
                parent, {text(field.name), {}, text(field.value), text(field.note)});
            value->setToolTip(2, text(field.value));
            value->setToolTip(3, text(field.note));
        }
        if (id == "memory.capacity" && !record->fields.empty())
            item->setText(2, text(record->fields.front().value));
        for (const auto& field : record->fields)
            if (!field.group.empty()) {
                auto* group = groups.at(field.group);
                if (field.name == "设备容量")
                    group->setText(2, text(field.value));
                if (field.name == "Pre-EOL")
                    group->setText(2, QStringLiteral("Pre-EOL ") + text(field.value));
                if (field.name == "寿命字段 A" || field.name == "寿命字段 B")
                    group->setText(2,
                                   group->text(2) + QStringLiteral(" · ") + text(field.name) + " " +
                                       text(field.value));
                group->setToolTip(2, group->text(2));
            }
        item->setExpanded(true);
    }
}
void BoardInfoPanel::poll()
{
    tasks_.reapFinished();
    const auto state = service_->snapshot();
    render();
    updateControls();
    if (wasActive_ && !state.active) {
        wasActive_ = false;
        emit taskChanged();
        emit activityChanged(text(state.serial) + QStringLiteral(" · ") + text(state.message));
    }
    maybeAutoRefresh();
    if (closing_ && !state.active && tasks_.retainedTaskCount() == 0 && !shutdownEmitted_) {
        shutdownEmitted_ = true;
        timer_->stop();
        emit shutdownFinished();
    }
}
void BoardInfoPanel::showDetails()
{
    const auto state = service_->snapshot();
    if (state.serial != target_.serial || state.generation != session_.generation)
        return;
    using J = testtools::JsonValue;
    J::Array records;
    for (const auto& row : state.records) {
        J value = J::object({{"capability", J::string(row.capability)},
                             {"state", J::string(testtools::ui::toUtf8(stateName(row.state)))},
                             {"error", J::string(row.error)}});
        if (row.job) {
            value.objectItems()["job_id"] = J::string(row.job->jobId);
            if (row.job->result) {
                const auto& result = *row.job->result;
                value.objectItems()["schema"] = J::string(result.observationSchema);
                value.objectItems()["data"] = result.observations;
                value.objectItems()["summary"] = J::string(result.summary);
                value.objectItems()["log"] = J::string(result.log);
                if (result.provider)
                    value.objectItems()["provider"] =
                        J::object({{"kind", J::string(result.provider->kind)},
                                   {"id", J::string(result.provider->id)},
                                   {"version", J::integer(result.provider->version)}});
            }
        }
        records.push_back(std::move(value));
    }
    const auto result = testtools::serializeJson(J::object(
        {{"device", J::string(state.serial)}, {"records", J::array(std::move(records))}}));
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("存储详情 · %1").arg(text(state.serial)));
    dialog.resize(940, 500);
    auto* layout = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget(&dialog);
    auto* values = new QTreeWidget(tabs);
    values->setObjectName("boardInfoDetailValues");
    values->setColumnCount(4);
    values->setHeaderLabels({QStringLiteral("信息项"),
                             QStringLiteral("状态"),
                             QStringLiteral("当前值"),
                             QStringLiteral("说明")});
    values->setAlternatingRowColors(true);
    values->setColumnWidth(0, 200);
    values->setColumnWidth(1, 90);
    values->setColumnWidth(2, 280);
    populateDetails(values, state);
    tabs->addTab(values, QStringLiteral("详细信息"));
    auto* content = new QPlainTextEdit(tabs);
    content->setReadOnly(true);
    content->setPlainText(text(result));
    tabs->addTab(content, QStringLiteral("原始数据"));
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(&dialog);
    buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
