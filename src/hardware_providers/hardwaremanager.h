#ifndef HARDWAREMANAGER_H
#define HARDWAREMANAGER_H

#include <QObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>

#include "hardwareprovider.h"

/*
  HardwareManager (HW-01 MF-147 worker-thread refactor)
  - Owns exactly one active provider, living on a private worker QThread
  - Public API stays synchronous-call-style, but provider work runs async
  - Re-emits provider signals for the UI on the UI thread
  - operationStarted/Finished let the UI disable buttons during I/O

  Threading contract:
    HardwareManager itself lives on the UI thread.
    The provider lives on m_workerThread.
    Cross-thread calls go through Qt::QueuedConnection.
*/

class HardwareWorker;  /* private, defined in hardwaremanager.cpp */

class HardwareManager : public QObject
{
    Q_OBJECT

public:
    explicit HardwareManager(QObject *parent = nullptr);
    ~HardwareManager() override;

    bool setHardwareType(const QString &hardwareType);
    void setDevicePath(const QString &devicePath);
    void setBaudRate(int baudRate);
    bool isOperationActive() const;

public slots:
    void detectDrive();
    void autoDetectDevice();

signals:
    /* Forwarded provider signals (cross-thread queued). */
    void driveDetected(const DetectedDriveInfo &info);
    void hardwareInfoUpdated(const HardwareInfo &info);
    void statusMessage(const QString &message);
    void devicePathSuggested(const QString &path);

    /* UI-thread lifecycle (HW-01 MF-147). */
    void operationStarted(const QString &operationName);
    void operationFinished(const QString &operationName);

    /* Internal: trigger worker on its own thread. Connect via QueuedConnection. */
    void requestDetectDrive();
    void requestAutoDetectDevice();

private slots:
    void onWorkerOperationFinished(const QString &name);

private:
    void ensureWorkerThread();
    void replaceProviderOnWorker(const QString &hardwareType);
    void applySettingsToWorker();
    void clearOperationActive();

    QString m_hardwareType;
    QString m_devicePath;
    int m_baudRate = 0;

    QThread *m_workerThread = nullptr;
    QPointer<HardwareWorker> m_worker;  /* lives on m_workerThread */

    /* Thread safety: prevents hardware type change during active operations.
     * m_operationActive is set on the UI thread before emitting a request,
     * and cleared on the UI thread when the worker emits finished(). */
    mutable QMutex m_hwMutex;
    bool m_operationActive = false;
    QString m_activeOperationName;
};

#endif // HARDWAREMANAGER_H
