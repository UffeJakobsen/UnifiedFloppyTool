#include "hardwaremanager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QThread>

// All hardware providers
#include "mockhardwareprovider.h"
#include "greaseweazlehardwareprovider.h"
#include "fluxenginehardwareprovider.h"
#include "kryofluxhardwareprovider.h"
#include "scphardwareprovider.h"
#include "applesaucehardwareprovider.h"
#include "fc5025hardwareprovider.h"
#include "xum1541hardwareprovider.h"
#include "catweaselhardwareprovider.h"
#include "adfcopyhardwareprovider.h"
#include "usbfloppyhardwareprovider.h"

/* ───────────────────────────────────────────────────────────────────────────
 *  HardwareWorker — lives on the worker QThread, owns the provider.
 *
 *  All slots run on the worker thread. They invoke the provider's blocking
 *  methods directly (provider lives on the same thread, so its signals also
 *  emit from the worker thread; HardwareManager's queued connections marshal
 *  them back to the UI thread automatically).
 * ─────────────────────────────────────────────────────────────────────────── */
class HardwareWorker : public QObject
{
    Q_OBJECT
public:
    explicit HardwareWorker(QObject *parent = nullptr) : QObject(parent) {}
    ~HardwareWorker() override
    {
        delete m_provider;
        m_provider = nullptr;
    }

    HardwareProvider *provider() const { return m_provider; }

signals:
    void operationFinished(const QString &name);

    /* Re-emitted from provider so HardwareManager can wire one stable
     * source. Connections survive provider replacement. */
    void driveDetected(const DetectedDriveInfo &info);
    void hardwareInfoUpdated(const HardwareInfo &info);
    void statusMessage(const QString &message);
    void devicePathSuggested(const QString &path);

public slots:
    /* Provider lifecycle — runs on worker thread so we can safely
     * delete the old QObject (which lives on this thread) and create
     * the new one parented to nothing (parent must share thread). */
    void replaceProvider(HardwareProvider *next)
    {
        if (m_provider == next) return;
        if (m_provider) {
            /* HardwareProvider has its own virtual disconnect() (hardware
             * connection lifecycle) which shadows QObject::disconnect; use
             * the QObject overload explicitly. */
            QObject::disconnect(m_provider, nullptr, this, nullptr);
            m_provider->deleteLater();
            m_provider = nullptr;
        }
        m_provider = next;
        if (m_provider) {
            connect(m_provider, &HardwareProvider::driveDetected,
                    this, &HardwareWorker::driveDetected);
            connect(m_provider, &HardwareProvider::hardwareInfoUpdated,
                    this, &HardwareWorker::hardwareInfoUpdated);
            connect(m_provider, &HardwareProvider::statusMessage,
                    this, &HardwareWorker::statusMessage);
            connect(m_provider, &HardwareProvider::devicePathSuggested,
                    this, &HardwareWorker::devicePathSuggested);
        }
    }

    void applyHardwareType(const QString &hardwareType)
    {
        if (m_provider) m_provider->setHardwareType(hardwareType);
    }
    void applyDevicePath(const QString &devicePath)
    {
        if (m_provider) m_provider->setDevicePath(devicePath);
    }
    void applyBaudRate(int baudRate)
    {
        if (m_provider) m_provider->setBaudRate(baudRate);
    }

    void runDetectDrive()
    {
        if (m_provider) m_provider->detectDrive();
        emit operationFinished(QStringLiteral("detectDrive"));
    }
    void runAutoDetectDevice()
    {
        if (m_provider) m_provider->autoDetectDevice();
        emit operationFinished(QStringLiteral("autoDetectDevice"));
    }

private:
    HardwareProvider *m_provider = nullptr;
};

/* ───────────────────────────────────────────────────────────────────────────
 *  Provider factory — runs on UI thread, returns a freshly-constructed
 *  provider with no parent (so it can be moved to the worker thread).
 * ─────────────────────────────────────────────────────────────────────────── */
namespace {
HardwareProvider *makeProvider(const QString &hardwareType)
{
    if (hardwareType.contains("Mock", Qt::CaseInsensitive) ||
        hardwareType.contains("Test", Qt::CaseInsensitive)) {
        return new MockHardwareProvider();
    }
    if (hardwareType.contains("Greaseweazle", Qt::CaseInsensitive)) {
        return new GreaseweazleHardwareProvider();
    }
    if (hardwareType.contains("FluxEngine", Qt::CaseInsensitive)) {
        return new FluxEngineHardwareProvider();
    }
    if (hardwareType.contains("KryoFlux", Qt::CaseInsensitive)) {
        return new KryoFluxHardwareProvider();
    }
    if (hardwareType.contains("SuperCard", Qt::CaseInsensitive) ||
        hardwareType.contains("SCP", Qt::CaseInsensitive)) {
        return new SCPHardwareProvider();
    }
    if (hardwareType.contains("Applesauce", Qt::CaseInsensitive)) {
        return new ApplesauceHardwareProvider();
    }
    if (hardwareType.contains("FC5025", Qt::CaseInsensitive)) {
        return new FC5025HardwareProvider();
    }
    if (hardwareType.contains("XUM1541", Qt::CaseInsensitive) ||
        hardwareType.contains("ZoomFloppy", Qt::CaseInsensitive)) {
        return new Xum1541HardwareProvider();
    }
    if (hardwareType.contains("Catweasel", Qt::CaseInsensitive)) {
        return new CatweaselHardwareProvider();
    }
    if (hardwareType.contains("ADF-Copy", Qt::CaseInsensitive) ||
        hardwareType.contains("ADFCopy",  Qt::CaseInsensitive) ||
        hardwareType.contains("ADF-Drive", Qt::CaseInsensitive) ||
        hardwareType.contains("adfcopy",  Qt::CaseInsensitive)) {
        return new ADFCopyHardwareProvider();
    }
    /* Default to Greaseweazle for unknown types */
    return new GreaseweazleHardwareProvider();
}
} /* namespace */

/* ───────────────────────────────────────────────────────────────────────────
 *  HardwareManager
 * ─────────────────────────────────────────────────────────────────────────── */
HardwareManager::HardwareManager(QObject *parent)
    : QObject(parent)
{
    /* Register pointer types for Qt::QueuedConnection / BlockingQueuedConnection. */
    static const int s_pidProvider = qRegisterMetaType<HardwareProvider*>("HardwareProvider*");
    Q_UNUSED(s_pidProvider);

    ensureWorkerThread();
    /* Default to Greaseweazle provider (most common). */
    replaceProviderOnWorker(QStringLiteral("Greaseweazle"));
    m_hardwareType = QStringLiteral("Greaseweazle");
}

HardwareManager::~HardwareManager()
{
    if (m_workerThread) {
        /* Synchronously drop provider on the worker thread (its own thread
         * affinity) before stopping the loop. Worker itself is destroyed via
         * connect(thread, finished, worker, deleteLater) installed in
         * ensureWorkerThread(). */
        if (m_worker) {
            QMetaObject::invokeMethod(m_worker.data(), "replaceProvider",
                Qt::BlockingQueuedConnection,
                Q_ARG(HardwareProvider*, nullptr));
        }
        m_workerThread->quit();
        m_workerThread->wait(2000);
        delete m_workerThread;
        m_workerThread = nullptr;
    }
}

void HardwareManager::ensureWorkerThread()
{
    if (m_workerThread) return;

    m_workerThread = new QThread();
    m_workerThread->setObjectName(QStringLiteral("UFT-Hardware"));

    HardwareWorker *worker = new HardwareWorker();
    worker->moveToThread(m_workerThread);
    m_worker = worker;

    /* Worker outlives manager only briefly during dtor; tie its destruction
     * to the thread finishing. */
    connect(m_workerThread, &QThread::finished,
            worker, &QObject::deleteLater);

    /* Forward provider signals (worker is on worker thread, manager on UI
     * thread → automatic Qt::AutoConnection becomes Qt::QueuedConnection). */
    connect(worker, &HardwareWorker::driveDetected,
            this,   &HardwareManager::driveDetected);
    connect(worker, &HardwareWorker::hardwareInfoUpdated,
            this,   &HardwareManager::hardwareInfoUpdated);
    connect(worker, &HardwareWorker::statusMessage,
            this,   &HardwareManager::statusMessage);
    connect(worker, &HardwareWorker::devicePathSuggested,
            this,   &HardwareManager::devicePathSuggested);
    connect(worker, &HardwareWorker::operationFinished,
            this,   &HardwareManager::onWorkerOperationFinished);

    /* Internal request signals → worker slots, queued. */
    connect(this,   &HardwareManager::requestDetectDrive,
            worker, &HardwareWorker::runDetectDrive,
            Qt::QueuedConnection);
    connect(this,   &HardwareManager::requestAutoDetectDevice,
            worker, &HardwareWorker::runAutoDetectDevice,
            Qt::QueuedConnection);

    m_workerThread->start();
}

void HardwareManager::replaceProviderOnWorker(const QString &hardwareType)
{
    HardwareProvider *next = makeProvider(hardwareType);
    if (!next) return;
    /* Move the new provider onto the worker thread BEFORE handing it over,
     * so signal connections inside replaceProvider() see correct affinity. */
    next->moveToThread(m_workerThread);

    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.data(), "replaceProvider",
            Qt::QueuedConnection,
            Q_ARG(HardwareProvider*, next));
    } else {
        delete next;  /* should never happen */
    }
}

bool HardwareManager::setHardwareType(const QString &hardwareType)
{
    {
        QMutexLocker locker(&m_hwMutex);
        if (m_operationActive) {
            qWarning() << "HardwareManager: cannot change hardware type while operation is active";
            return false;
        }
    }

    m_hardwareType = hardwareType;
    replaceProviderOnWorker(hardwareType);
    applySettingsToWorker();
    return true;
}

bool HardwareManager::isOperationActive() const
{
    QMutexLocker locker(&m_hwMutex);
    return m_operationActive;
}

void HardwareManager::setDevicePath(const QString &devicePath)
{
    m_devicePath = devicePath;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.data(), "applyDevicePath",
            Qt::QueuedConnection,
            Q_ARG(QString, devicePath));
    }
}

void HardwareManager::setBaudRate(int baudRate)
{
    m_baudRate = baudRate;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker.data(), "applyBaudRate",
            Qt::QueuedConnection,
            Q_ARG(int, baudRate));
    }
}

void HardwareManager::applySettingsToWorker()
{
    if (!m_worker) return;
    QMetaObject::invokeMethod(m_worker.data(), "applyHardwareType",
        Qt::QueuedConnection, Q_ARG(QString, m_hardwareType));
    QMetaObject::invokeMethod(m_worker.data(), "applyDevicePath",
        Qt::QueuedConnection, Q_ARG(QString, m_devicePath));
    QMetaObject::invokeMethod(m_worker.data(), "applyBaudRate",
        Qt::QueuedConnection, Q_ARG(int, m_baudRate));
}

void HardwareManager::detectDrive()
{
    {
        QMutexLocker locker(&m_hwMutex);
        if (m_operationActive) {
            qWarning() << "HardwareManager: detectDrive ignored, operation already active:"
                       << m_activeOperationName;
            return;
        }
        m_operationActive = true;
        m_activeOperationName = QStringLiteral("detectDrive");
    }
    emit operationStarted(QStringLiteral("detectDrive"));
    emit requestDetectDrive();   /* queued → worker thread */
}

void HardwareManager::autoDetectDevice()
{
    {
        QMutexLocker locker(&m_hwMutex);
        if (m_operationActive) {
            qWarning() << "HardwareManager: autoDetectDevice ignored, operation already active:"
                       << m_activeOperationName;
            return;
        }
        m_operationActive = true;
        m_activeOperationName = QStringLiteral("autoDetectDevice");
    }
    emit operationStarted(QStringLiteral("autoDetectDevice"));
    emit requestAutoDetectDevice();
}

void HardwareManager::onWorkerOperationFinished(const QString &name)
{
    clearOperationActive();
    emit operationFinished(name);
}

void HardwareManager::clearOperationActive()
{
    QMutexLocker locker(&m_hwMutex);
    m_operationActive = false;
    m_activeOperationName.clear();
}

#include "hardwaremanager.moc"
