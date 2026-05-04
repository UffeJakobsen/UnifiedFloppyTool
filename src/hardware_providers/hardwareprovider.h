#ifndef HARDWAREPROVIDER_H
#define HARDWAREPROVIDER_H

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QVector>

/* ═══════════════════════════════════════════════════════════════════════════════
 * Data Structures
 * ═══════════════════════════════════════════════════════════════════════════════ */

struct DetectedDriveInfo {
    QString type;
    int tracks = 0;
    int heads = 0;
    QString density;
    QString rpm;
    QString model;
};

struct HardwareInfo {
    QString provider;
    QString vendor;
    QString product;
    QString firmware;
    QString clock;
    QString connection;
    QStringList toolchain;
    QStringList formats;
    QString notes;
    bool isReady = false;
};

struct ReadParams {
    int cylinder = 0;
    int head = 0;
    int revolutions = 2;
    int retries = 3;          // Added
    bool rawFlux = false;
    QString format;
};

struct WriteParams {
    int cylinder = 0;
    int head = 0;
    int retries = 3;          // Added
    bool verify = true;
    bool precomp = true;
    QString format;
};

/* MF-149 (rule H-9): the alias members `valid` and `errorMessage`
 * carry a [[deprecated]] attribute so that named access — e.g.
 * `td.errorMessage` in user code — surfaces a compile-time warning.
 *
 * gcc emits the warning twice: once at the genuine call-site (which
 * is what we want), and once at the struct's implicit special-member
 * generation in every including TU (false positive — the user did
 * nothing wrong). To avoid drowning every translation unit in noise,
 * we wrap the deprecated members in a localized warning-suppression
 * pragma. The suppression is scoped to the struct definition; user
 * code that names `.valid` or `.errorMessage` outside this header
 * still gets the warning. */
#if defined(__GNUC__) || defined(__clang__)
#  define UFT_H9_DEPRECATED_PUSH \
       _Pragma("GCC diagnostic push") \
       _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")
#  define UFT_H9_DEPRECATED_POP _Pragma("GCC diagnostic pop")
#else
#  define UFT_H9_DEPRECATED_PUSH
#  define UFT_H9_DEPRECATED_POP
#endif

struct TrackData {
    int cylinder = 0;
    int head = 0;
    QByteArray data;
    QByteArray rawFlux;
    bool success = false;
    QString error;
    int badSectors = 0;
    int goodSectors = 0;

    UFT_H9_DEPRECATED_PUSH
    [[deprecated("MF-149 (rule H-9): use TrackData::success; will be removed in v5.0")]]
    bool valid = false;
    [[deprecated("MF-149 (rule H-9): use TrackData::error; will be removed in v5.0")]]
    QString errorMessage;
    UFT_H9_DEPRECATED_POP
};

struct OperationResult {
    bool success = false;
    QString error;
    int retriesUsed = 0;

    UFT_H9_DEPRECATED_PUSH
    [[deprecated("MF-149 (rule H-9): use OperationResult::error; will be removed in v5.0")]]
    QString errorMessage;
    UFT_H9_DEPRECATED_POP
};

/* MF-149 (rule H-9) write helpers.
 *
 * During the deprecation window (v4.x), callers want to set BOTH the
 * canonical field AND its alias so older readers keep working. These
 * inline helpers do that in one place and locally suppress the
 * deprecated-write warning the gcc attribute would otherwise emit.
 *
 * After v5.0 removes `valid` / `errorMessage`, these helpers degrade
 * to simple field writes (the `_PUSH/_POP` becomes a no-op).
 *
 * Callers should write:
 *     uft_set_track_success(td, true);                  // sets both
 *     uft_set_track_error(td, "msg");                   // sets both
 *     uft_set_op_error(opr, "msg");                     // sets both
 * instead of `td.valid = ...` / `td.errorMessage = ...`. */
inline void uft_set_track_success(TrackData &td, bool ok) {
    td.success = ok;
    UFT_H9_DEPRECATED_PUSH
    td.valid = ok;
    UFT_H9_DEPRECATED_POP
}
inline void uft_set_track_error(TrackData &td, const QString &msg) {
    td.error = msg;
    UFT_H9_DEPRECATED_PUSH
    td.errorMessage = msg;
    UFT_H9_DEPRECATED_POP
}
inline void uft_set_op_error(OperationResult &opr, const QString &msg) {
    opr.error = msg;
    UFT_H9_DEPRECATED_PUSH
    opr.errorMessage = msg;
    UFT_H9_DEPRECATED_POP
}

Q_DECLARE_METATYPE(DetectedDriveInfo)
Q_DECLARE_METATYPE(HardwareInfo)
Q_DECLARE_METATYPE(ReadParams)
Q_DECLARE_METATYPE(WriteParams)
Q_DECLARE_METATYPE(TrackData)
Q_DECLARE_METATYPE(OperationResult)

/* ═══════════════════════════════════════════════════════════════════════════════
 * HardwareProvider Base Class
 * ═══════════════════════════════════════════════════════════════════════════════ */

class HardwareProvider : public QObject
{
    Q_OBJECT

public:
    explicit HardwareProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~HardwareProvider() override = default;

    // Basic Interface (required)
    virtual QString displayName() const = 0;

public slots:
    /* MF-147 (HW-01): these are slots so HardwareManager can dispatch them
     * onto a worker thread via Qt::QueuedConnection without blocking the
     * UI thread. Provider implementations may do blocking serial / USB /
     * QProcess I/O here. */
    virtual void setHardwareType(const QString &hardwareType) = 0;
    virtual void setDevicePath(const QString &devicePath) = 0;
    virtual void setBaudRate(int baudRate) = 0;
    virtual void detectDrive() = 0;
    virtual void autoDetectDevice() = 0;

public:

    // Connection Management
    virtual bool connect() { return false; }
    virtual void disconnect() {}
    virtual bool isConnected() const { return false; }

    // Motor & Head Control
    virtual bool setMotor(bool on) { Q_UNUSED(on); return false; }
    virtual bool seekCylinder(int cylinder) { Q_UNUSED(cylinder); return false; }
    virtual bool selectHead(int head) { Q_UNUSED(head); return false; }
    virtual int currentCylinder() const { return -1; }

    // Read Operations
    virtual TrackData readTrack(const ReadParams &params) { 
        Q_UNUSED(params); 
        return TrackData(); 
    }
    virtual QByteArray readRawFlux(int cylinder, int head, int revolutions = 2) { 
        Q_UNUSED(cylinder); Q_UNUSED(head); Q_UNUSED(revolutions);
        return QByteArray(); 
    }
    virtual QVector<TrackData> readDisk(int startCyl = 0, int endCyl = -1, int heads = 2) {
        Q_UNUSED(startCyl); Q_UNUSED(endCyl); Q_UNUSED(heads);
        return QVector<TrackData>();
    }

    // Write Operations
    virtual OperationResult writeTrack(const WriteParams &params, const QByteArray &data) {
        Q_UNUSED(params); Q_UNUSED(data);
        return OperationResult();
    }
    virtual bool writeRawFlux(int cylinder, int head, const QByteArray &fluxData) {
        Q_UNUSED(cylinder); Q_UNUSED(head); Q_UNUSED(fluxData);
        return false;
    }

    // Utility
    virtual bool getGeometry(int &tracks, int &heads) {
        Q_UNUSED(tracks); Q_UNUSED(heads);
        return false;
    }
    virtual double measureRPM() { return 0.0; }
    virtual bool recalibrate() { return false; }

signals:
    void driveDetected(const DetectedDriveInfo &info);
    void hardwareInfoUpdated(const HardwareInfo &info);
    void statusMessage(const QString &message);
    void devicePathSuggested(const QString &path);
    void connectionStateChanged(bool connected);
    void operationError(const QString &error);
    void progressChanged(int current, int total);
    void trackRead(int cylinder, int head, bool success);
    void trackWritten(int cylinder, int head, bool success);
    // Added missing signals
    void trackReadComplete(int cylinder, int head, bool success);
    void trackWriteComplete(int cylinder, int head, bool success);
};

#endif // HARDWAREPROVIDER_H
