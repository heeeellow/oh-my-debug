#pragma once
#include "application/toolbox/toolbox_service.h"
#include "application/support/background_task_group.h"
#include "core/serial/serial_types.h"
#include <QtCore/QObject>
#include <set>

class QWidget; class QLabel; class QPushButton; class QTreeWidget; class QTreeWidgetItem;
class QComboBox; class QSpinBox; class QLineEdit; class QPlainTextEdit; class QTimer;
class ToolboxPlot;

class ToolboxWorkspace final : public QObject {
    Q_OBJECT
  public:
    explicit ToolboxWorkspace(QWidget* parent, std::shared_ptr<testtools::ToolboxService> service = {});
    ~ToolboxWorkspace() override;
    QWidget* gpioPage() const { return gpioPage_; }
    QWidget* networkPage() const { return networkPage_; }
    void setTarget(const testtools::AdbDevice& device);
    void setSession(testtools::ToolboxSession session, std::vector<std::string> capabilities);
    void clearSession();
    void setVisible(bool gpio, bool network);
    void shutdown();
    void cancelTask(std::uint64_t id);
    bool busyForDevice(const std::string& serial) const;
    testtools::ToolboxSnapshot snapshot() const;
    std::function<bool(const std::string&)> conflict;
  signals:
    void activityChanged(const QString& message);
    void taskChanged();
    void shutdownFinished();
    void connectRequested();
  private:
    QWidget* buildGpio(QWidget* parent);
    QWidget* buildNetwork(QWidget* parent);
    void refresh(bool gpioOnly = false);
    void calculatePin();
    void populate();
    void updateControls();
    void selectPin();
    void runGpio(const std::string& capability);
    void runNetwork();
    void poll();
    void saveReport(bool network);
    void loadPreferences();
    void savePreferences();
    void restoreParameters();
    void toggleFavorite();
    void reconcile();
    struct PreferenceWriter;
    std::shared_ptr<PreferenceWriter> preferenceWriter_;
    testtools::JsonValue preferences_ = testtools::JsonValue::object();
    std::set<std::string> confirmations_;
    std::shared_ptr<testtools::ToolboxService> service_;
    testtools::ToolboxSession session_;
    testtools::AdbDevice target_;
    std::vector<std::string> capabilities_;
    testtools::BackgroundTaskGroup tasks_;
    testtools::JsonValue gpio_ = testtools::JsonValue::object(), boardNetwork_ = testtools::JsonValue::object(), hostNetwork_ = testtools::JsonValue::object();
    testtools::JsonValue selectedPin_ = testtools::JsonValue::object(), gpioReport_ = testtools::JsonValue::object(), networkReport_ = testtools::JsonValue::object();
    bool metadataBusy_ = false, metadataLoaded_ = false, gpioVisible_ = false, networkVisible_ = false;
    bool gpioMetadataBusy_ = false;
    bool closing_ = false, shutdownEmitted_ = false, preferencesLoaded_ = false, restoring_ = false;
    bool runnerAvailable_ = false;
    std::uint64_t renderedId_ = 0;
    std::string renderedState_, metadataError_;
    QWidget *gpioPage_ = nullptr, *networkPage_ = nullptr;
    QWidget *nativeOutputOptions_ = nullptr, *nativeExtraOptions_ = nullptr;
    QLabel *boardHint_ = nullptr, *pinDetails_ = nullptr;
    testtools::JsonValue boardGpio_ = testtools::JsonValue::object();
    QTreeWidget* pins_ = nullptr;
    QLineEdit *filter_ = nullptr, *pinLookup_ = nullptr;
    QLabel *gpioStatus_ = nullptr, *networkStatus_ = nullptr, *pinInfo_ = nullptr, *gpioResult_ = nullptr, *networkResult_ = nullptr, *pinConversion_ = nullptr;
    QPushButton *gpioRefresh_ = nullptr, *networkRefresh_ = nullptr, *gpioConnect_ = nullptr, *networkConnect_ = nullptr;
    QPushButton *favorite_ = nullptr, *read_ = nullptr, *write_ = nullptr, *pulse_ = nullptr, *monitor_ = nullptr;
    QPushButton *gpioStop_ = nullptr, *networkStop_ = nullptr, *extend_ = nullptr, *networkStart_ = nullptr;
    QPushButton *gpioExport_ = nullptr, *networkExport_ = nullptr, *gpioReconcile_ = nullptr, *networkReconcile_ = nullptr;
    QComboBox *level_ = nullptr, *drive_ = nullptr, *idleLevel_ = nullptr, *pulseLevel_ = nullptr, *edge_ = nullptr;
    QSpinBox *holdSeconds_ = nullptr, *pulseWidth_ = nullptr, *monitorSeconds_ = nullptr;
    QComboBox *pcInterface_ = nullptr, *boardInterface_ = nullptr, *direction_ = nullptr, *protocol_ = nullptr;
    QSpinBox *duration_ = nullptr, *parallel_ = nullptr, *port_ = nullptr, *udpRate_ = nullptr;
    QPlainTextEdit *gpioDetails_ = nullptr, *networkDetails_ = nullptr;
    QTimer *timer_ = nullptr, *preferencesTimer_ = nullptr, *gpioMetadataTimer_ = nullptr;
    ToolboxPlot* plot_ = nullptr;
};
