#include "toolbox_workspace.h"
#include "core/toolbox/toolbox_contract.h"
#include <QtCore/QRegularExpression>
#include "ui/widgets/toolbox_plot.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

namespace {
QSpinBox* spin(QWidget* parent, int low, int high, int value, const char* name, const QString& suffix = {})
{
    auto* widget = new QSpinBox(parent); widget->setRange(low, high); widget->setValue(value);
    widget->setObjectName(name); widget->setSuffix(suffix); widget->setFixedWidth(high > 999 ? 150 : 120);
    widget->setAccessibleName(QString::fromLatin1(name)); return widget;
}
QComboBox* levels(QWidget* parent, const char* name, int value = 0)
{
    auto* combo = new QComboBox(parent); combo->setObjectName(name);
    combo->addItem(QStringLiteral("低电平（0）"), 0); combo->addItem(QStringLiteral("高电平（1）"), 1); combo->setCurrentIndex(value); return combo;
}
QPushButton* button(const QString& text, const char* name, QWidget* parent)
{
    auto* value = new QPushButton(text, parent); value->setObjectName(name); value->setCursor(Qt::PointingHandCursor); return value;
}
QLabel* label(QWidget* parent, const char* name)
{
    auto* value = new QLabel(parent); value->setWordWrap(true); value->setObjectName(name);
    value->setTextFormat(Qt::PlainText); value->setTextInteractionFlags(Qt::TextSelectableByMouse); return value;
}
void detailsRow(QVBoxLayout* layout, QWidget* parent, QPlainTextEdit*& details)
{
    auto* toggle = new QToolButton(parent); toggle->setText(QStringLiteral("详情")); toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(Qt::RightArrow);
    layout->addWidget(toggle, 0, Qt::AlignLeft);
    details = new QPlainTextEdit(parent); details->setReadOnly(true); details->setMaximumHeight(145); details->setVisible(false);
    details->setObjectName("toolboxDetails"); layout->addWidget(details);
    QObject::connect(toggle, &QToolButton::toggled, details, [toggle, details](bool open) {
        details->setVisible(open); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
}
}
QWidget* ToolboxWorkspace::buildGpio(QWidget* parent)
{
    auto* page = new QWidget(parent); page->setObjectName("gpioToolPage");
    auto* root = new QVBoxLayout(page); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(10);
    auto* bar = new QHBoxLayout;
    gpioStatus_ = label(page, "gpioToolStatus"); gpioStatus_->setText(QStringLiteral("连接板端服务后查看引脚"));
    bar->addWidget(gpioStatus_, 1);
    gpioConnect_ = button(QStringLiteral("连接服务"), "gpioConnectService", page); bar->addWidget(gpioConnect_);
    gpioRefresh_ = button(QStringLiteral("刷新"), "gpioRefresh", page); bar->addWidget(gpioRefresh_); root->addLayout(bar);
    connect(gpioConnect_, &QPushButton::clicked, this, &ToolboxWorkspace::connectRequested);
    connect(gpioRefresh_, &QPushButton::clicked, this, &ToolboxWorkspace::refresh);
    auto* split = new QSplitter(Qt::Horizontal, page); split->setChildrenCollapsible(false);
    auto* list = new QWidget(split); auto* listLayout = new QVBoxLayout(list); listLayout->setContentsMargins(0, 0, 6, 0);
    auto* filterRow = new QHBoxLayout;
    filter_ = new QLineEdit(list); filter_->setObjectName("gpioFilter"); filter_->setPlaceholderText(QStringLiteral("搜索引脚、名称或占用者"));
    filterRow->addWidget(filter_, 1); listLayout->addLayout(filterRow);
    auto* lookup = new QHBoxLayout;
    pinLookup_ = new QLineEdit(list); pinLookup_->setObjectName("gpioLookup"); pinLookup_->setPlaceholderText(QStringLiteral("管脚号换算：GPIO3_B4 或 108")); lookup->addWidget(pinLookup_);
    auto* locate = button(QStringLiteral("换算 / 定位"), "gpioLocate", list); lookup->addWidget(locate); listLayout->addLayout(lookup);
    pinConversion_ = label(list, "gpioConversionResult"); pinConversion_->setText(QStringLiteral("可离线双向换算；Linux 全局号按实际控制器基址核对。")); listLayout->addWidget(pinConversion_);
    pins_ = new QTreeWidget(list); pins_->setObjectName("gpioPinList"); pins_->setRootIsDecorated(false);
    pins_->setHeaderLabels({QStringLiteral("管脚 / Linux 号"), QStringLiteral("名称 / 备注"), QStringLiteral("方向"), QStringLiteral("占用者")});
    pins_->setAlternatingRowColors(false); pins_->setSelectionMode(QAbstractItemView::SingleSelection);
    pins_->header()->setSectionResizeMode(QHeaderView::ResizeToContents); pins_->header()->setStretchLastSection(true);
    listLayout->addWidget(pins_, 1);
    connect(pins_, &QTreeWidget::itemSelectionChanged, this, &ToolboxWorkspace::selectPin);
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString& query) {
        for (int row = 0; row < pins_->topLevelItemCount(); ++row) {
            auto* item = pins_->topLevelItem(row); QString text;
            for (int column = 0; column < 4; ++column) text += item->text(column) + ' ';
            text += item->data(0, Qt::UserRole + 3).toString();
            item->setHidden(!text.contains(query, Qt::CaseInsensitive));
        }
    });
    connect(locate, &QPushButton::clicked, this, &ToolboxWorkspace::calculatePin);
    connect(pinLookup_, &QLineEdit::returnPressed, this, &ToolboxWorkspace::calculatePin);
    auto* scroll = new QScrollArea(split); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setMinimumWidth(295);
    auto* controls = new QWidget(scroll); auto* formRoot = new QVBoxLayout(controls); formRoot->setContentsMargins(12, 0, 0, 0); formRoot->setSpacing(10);
    pinInfo_ = label(controls, "gpioPinInfo"); pinInfo_->setText(QStringLiteral("选择引脚后查看详情")); formRoot->addWidget(pinInfo_);
    auto* pinToggle = new QToolButton(controls); pinToggle->setText(QStringLiteral("引脚详情")); pinToggle->setCheckable(true);
    formRoot->addWidget(pinToggle, 0, Qt::AlignLeft);
    pinDetails_ = label(controls, "gpioPinDetails"); pinDetails_->setTextFormat(Qt::PlainText); pinDetails_->hide(); formRoot->addWidget(pinDetails_);
    connect(pinToggle, &QToolButton::toggled, pinDetails_, &QWidget::setVisible);
    boardHint_ = label(controls, "gpioBoardHint"); boardHint_->setVisible(false); formRoot->addWidget(boardHint_);
    favorite_ = button(QStringLiteral("加入常用…"), "gpioFavorite", controls); formRoot->addWidget(favorite_, 0, Qt::AlignLeft);
    connect(favorite_, &QPushButton::clicked, this, &ToolboxWorkspace::toggleFavorite);
    auto* form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    level_ = levels(controls, "gpioOutputLevel"); form->addRow(QStringLiteral("输出电平"), level_);
    formRoot->addLayout(form);
    nativeOutputOptions_ = new QWidget(controls); auto* nativeOutputForm = new QFormLayout(nativeOutputOptions_); nativeOutputForm->setContentsMargins(0, 0, 0, 0);
    drive_ = new QComboBox(controls); drive_->setObjectName("gpioDrive");
    drive_->addItem(QStringLiteral("推挽"), "push_pull");
    nativeOutputForm->addRow(QStringLiteral("驱动方式"), drive_);
    holdSeconds_ = spin(controls, 1, 300, 30, "gpioHoldSeconds", QStringLiteral(" 秒")); nativeOutputForm->addRow(QStringLiteral("保持时长"), holdSeconds_);
    holdSeconds_->setAccessibleName(QStringLiteral("输出保持时长（秒）"));
    formRoot->addWidget(nativeOutputOptions_);
    auto* readWrite = new QHBoxLayout;
    read_ = button(QStringLiteral("读取电平"), "gpioRead", controls); readWrite->addWidget(read_);
    write_ = button(QStringLiteral("保持输出"), "gpioWrite", controls); write_->setProperty("primary", true); readWrite->addWidget(write_); formRoot->addLayout(readWrite);
    connect(read_, &QPushButton::clicked, this, [this] { runGpio("gpio.pin.read"); });
    connect(write_, &QPushButton::clicked, this, [this] { runGpio("gpio.pin.write"); });
    nativeExtraOptions_ = new QWidget(controls); auto* extraRoot = new QVBoxLayout(nativeExtraOptions_); extraRoot->setContentsMargins(0, 0, 0, 0);
    auto* pulseForm = new QFormLayout;
    idleLevel_ = levels(controls, "gpioIdleLevel"); pulseLevel_ = levels(controls, "gpioPulseLevel", 1);
    pulseWidth_ = spin(controls, 10, 10000, 100, "gpioPulseWidth", QStringLiteral(" ms"));
    pulseWidth_->setAccessibleName(QStringLiteral("脉冲宽度（毫秒）"));
    pulseForm->addRow(QStringLiteral("脉冲前后"), idleLevel_); pulseForm->addRow(QStringLiteral("脉冲电平"), pulseLevel_); pulseForm->addRow(QStringLiteral("脉冲宽度"), pulseWidth_);
    extraRoot->addLayout(pulseForm);
    pulse_ = button(QStringLiteral("输出单次脉冲"), "gpioPulse", controls); extraRoot->addWidget(pulse_);
    connect(pulse_, &QPushButton::clicked, this, [this] { runGpio("gpio.pin.pulse"); });
    auto* monitorForm = new QFormLayout;
    edge_ = new QComboBox(controls); edge_->setObjectName("gpioEdge");
    edge_->addItem(QStringLiteral("上升和下降沿"), "both"); edge_->addItem(QStringLiteral("上升沿"), "rising"); edge_->addItem(QStringLiteral("下降沿"), "falling");
    monitorSeconds_ = spin(controls, 1, 300, 10, "gpioMonitorSeconds", QStringLiteral(" 秒"));
    monitorSeconds_->setAccessibleName(QStringLiteral("监听时长（秒）"));
    monitorForm->addRow(QStringLiteral("输入监听"), edge_); monitorForm->addRow(QStringLiteral("监听时长"), monitorSeconds_); extraRoot->addLayout(monitorForm);
    monitor_ = button(QStringLiteral("切换输入并监听"), "gpioMonitor", controls); extraRoot->addWidget(monitor_); formRoot->addWidget(nativeExtraOptions_);
    connect(monitor_, &QPushButton::clicked, this, [this] { runGpio("gpio.edge.monitor"); });
    formRoot->addStretch(); scroll->setWidget(controls); split->setStretchFactor(0, 3); split->setStretchFactor(1, 2);
    root->addWidget(split, 1);
    gpioResult_ = label(page, "gpioToolResult"); gpioResult_->setText(QStringLiteral("读取保留原配置；控制前确认目标与参数，常用收藏可选。")); root->addWidget(gpioResult_);
    auto* actions = new QHBoxLayout;
    gpioStop_ = button(QStringLiteral("停止 / 释放引脚"), "gpioStop", page); actions->addWidget(gpioStop_);
    extend_ = button(QStringLiteral("延长保持"), "gpioExtend", page); actions->addWidget(extend_);
    gpioReconcile_ = button(QStringLiteral("复核结束"), "gpioReconcile", page); actions->addWidget(gpioReconcile_);
    actions->addStretch(); gpioExport_ = button(QStringLiteral("导出报告…"), "gpioExport", page); actions->addWidget(gpioExport_); root->addLayout(actions);
    connect(gpioStop_, &QPushButton::clicked, this, [this] { cancelTask(service_->snapshot().id); });
    connect(extend_, &QPushButton::clicked, this, [this] { service_->extendHold(holdSeconds_->value()); });
    connect(gpioReconcile_, &QPushButton::clicked, this, &ToolboxWorkspace::reconcile);
    connect(gpioExport_, &QPushButton::clicked, this, [this] { saveReport(false); });
    detailsRow(root, page, gpioDetails_); return page;
}
QWidget* ToolboxWorkspace::buildNetwork(QWidget* parent)
{
    auto* page = new QWidget(parent); page->setObjectName("networkToolPage");
    auto* root = new QVBoxLayout(page); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(12);
    auto* bar = new QHBoxLayout;
    networkStatus_ = label(page, "networkToolStatus"); networkStatus_->setText(QStringLiteral("连接板端服务后选择两端网口")); bar->addWidget(networkStatus_, 1);
    networkConnect_ = button(QStringLiteral("连接服务"), "networkConnectService", page); bar->addWidget(networkConnect_);
    networkRefresh_ = button(QStringLiteral("刷新"), "networkRefresh", page); bar->addWidget(networkRefresh_); root->addLayout(bar);
    connect(networkConnect_, &QPushButton::clicked, this, &ToolboxWorkspace::connectRequested);
    connect(networkRefresh_, &QPushButton::clicked, this, &ToolboxWorkspace::refresh);
    auto* parameters = new QWidget(page);
    auto* parameterLayout = new QVBoxLayout(parameters); parameterLayout->setContentsMargins(0, 0, 0, 0); parameterLayout->setSpacing(10);
    parameterLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    pcInterface_ = new QComboBox(parameters); pcInterface_->setObjectName("networkPcInterface"); pcInterface_->setMinimumContentsLength(16);
    boardInterface_ = new QComboBox(parameters); boardInterface_->setObjectName("networkBoardInterface");
    direction_ = new QComboBox(parameters); direction_->setObjectName("networkDirection");
    direction_->addItem(QStringLiteral("依次测试两个方向"), 2); direction_->addItem(QStringLiteral("电脑发送 → 板卡"), 0); direction_->addItem(QStringLiteral("板卡发送 → 电脑"), 1);
    duration_ = spin(parameters, 1, 60, 10, "networkDuration", QStringLiteral(" 秒"));
    duration_->setAccessibleName(QStringLiteral("每方向时长（秒）"));
    protocol_ = new QComboBox(parameters); protocol_->setObjectName("networkProtocol"); protocol_->addItem("TCP", "tcp"); protocol_->addItem("UDP", "udp");
    auto fieldLabel = [](const QString& title, QWidget* field) {
        auto* caption = new QLabel(title, field->parentWidget()); caption->setBuddy(field); return caption;
    };
    auto* addresses = new QGridLayout; addresses->setVerticalSpacing(6); addresses->setContentsMargins(0, 0, 0, 0);
    addresses->addWidget(fieldLabel(QStringLiteral("电脑网卡 / 源地址"), pcInterface_), 0, 0); addresses->addWidget(pcInterface_, 1, 0);
    addresses->addWidget(fieldLabel(QStringLiteral("板端网口 / 目标地址"), boardInterface_), 2, 0); addresses->addWidget(boardInterface_, 3, 0);
    parameterLayout->addLayout(addresses);
    auto* basic = new QGridLayout; basic->setHorizontalSpacing(18); basic->setVerticalSpacing(6); basic->setContentsMargins(0, 0, 0, 0);
    basic->setColumnStretch(0, 3); basic->setColumnStretch(1, 1);
    basic->addWidget(fieldLabel(QStringLiteral("测试方向"), direction_), 0, 0); basic->addWidget(direction_, 1, 0);
    basic->addWidget(fieldLabel(QStringLiteral("传输协议"), protocol_), 0, 1); basic->addWidget(protocol_, 1, 1);
    basic->addWidget(fieldLabel(QStringLiteral("每方向时长"), duration_), 0, 2); basic->addWidget(duration_, 1, 2, Qt::AlignRight);
    direction_->setAccessibleName(QStringLiteral("测试方向")); protocol_->setAccessibleName(QStringLiteral("传输协议"));
    parameterLayout->addLayout(basic);
    auto* advanced = new QWidget(parameters); auto* advancedRow = new QHBoxLayout(advanced); advancedRow->setContentsMargins(0, 0, 0, 0); advancedRow->setSpacing(18);
    parallel_ = spin(advanced, 1, 8, 1, "networkParallel"); port_ = spin(advanced, 5201, 5210, 5201, "networkPort");
    udpRate_ = spin(advanced, 1, 1000, 10, "networkUdpRate", QStringLiteral(" Mbps"));
    parallel_->setAccessibleName(QStringLiteral("TCP 并发流")); udpRate_->setAccessibleName(QStringLiteral("UDP 目标速率（Mbps）")); port_->setAccessibleName(QStringLiteral("测试端口"));
    advancedRow->addStretch();
    for (auto* field : {parallel_, udpRate_, port_}) {
        auto* group = new QVBoxLayout; group->setSpacing(6);
        group->addWidget(fieldLabel(field->accessibleName(), field)); group->addWidget(field); advancedRow->addLayout(group);
    }
    auto* toggle = new QToolButton(page); toggle->setObjectName("networkAdvancedParameters"); toggle->setText(QStringLiteral("高级参数")); toggle->setCheckable(true); toggle->setArrowType(Qt::RightArrow);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); bar->insertWidget(2, toggle); parameterLayout->addWidget(advanced); advanced->hide();
    for (auto* field : std::initializer_list<QWidget*>{pcInterface_, boardInterface_, direction_, duration_, protocol_, parallel_, udpRate_, port_}) {
        field->ensurePolished(); field->setMinimumHeight(field->sizeHint().height());
    }
    auto* parameterScroll = new QScrollArea(page); parameterScroll->setObjectName("networkParametersScroll");
    parameterScroll->setFrameShape(QFrame::NoFrame); parameterScroll->setWidgetResizable(true); parameterScroll->setMinimumHeight(120);
    parameterScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents); parameterScroll->setWidget(parameters); root->addWidget(parameterScroll);
    const std::vector<QWidget*> focusOrder{pcInterface_, boardInterface_, direction_, protocol_, duration_, toggle, parallel_, udpRate_, port_};
    for (std::size_t i = 1; i < focusOrder.size(); ++i) QWidget::setTabOrder(focusOrder[i - 1], focusOrder[i]);
    connect(toggle, &QToolButton::toggled, advanced, [toggle, advanced](bool visible) {
        advanced->setVisible(visible);
        toggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
    });
    plot_ = new ToolboxPlot(page); root->addWidget(plot_, 1);
    networkResult_ = label(page, "networkToolResult"); networkResult_->setText(QStringLiteral("结果将分别保留两个方向的吞吐与状态。")); root->addWidget(networkResult_);
    auto* actions = new QHBoxLayout;
    networkStart_ = button(QStringLiteral("开始测试"), "networkStart", page); networkStart_->setProperty("primary", true); actions->addWidget(networkStart_);
    networkStop_ = button(QStringLiteral("停止"), "networkStop", page); actions->addWidget(networkStop_);
    networkReconcile_ = button(QStringLiteral("复核结束"), "networkReconcile", page); actions->addWidget(networkReconcile_);
    actions->addStretch(); networkExport_ = button(QStringLiteral("导出报告…"), "networkExport", page); actions->addWidget(networkExport_); root->addLayout(actions);
    connect(networkStart_, &QPushButton::clicked, this, &ToolboxWorkspace::runNetwork);
    connect(networkStop_, &QPushButton::clicked, this, [this] { cancelTask(service_->snapshot().id); });
    connect(networkReconcile_, &QPushButton::clicked, this, &ToolboxWorkspace::reconcile);
    connect(networkExport_, &QPushButton::clicked, this, [this] { saveReport(true); });
    detailsRow(root, page, networkDetails_); return page;
}
