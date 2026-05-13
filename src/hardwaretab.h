#pragma once
/**
 * @file hardwaretab.h
 * @brief Hardware Tab - Floppy Controller Management
 * 
 * Source/Destination Mode:
 * - Source: Flux devices only (Greaseweazle, SCP, KryoFlux)
 * - Destination: Flux devices + USB Floppy Drive
 * 
 * Activation:
 * - Buttons only active when Workflow Tab has Flux/USB selected
 * - Disabled when Workflow Tab has "Image File" selected
 */

#ifndef HARDWARETAB_H
#define HARDWARETAB_H

#include <QWidget>
#include <QTimer>
#include <QButtonGroup>

#include <memory>

#include "uft/hal/outcomes.h"   /* forensic Sum-Types — handlers take refs */

namespace Ui { class TabHardware; }

class HardwareTab;              /* fwd-decl needed before the codegen
                                 * namespace below references it */
struct DetectedDriveInfo;
struct HardwareInfo;

/* MF-157 — see src/hardware_providers/greaseweazle_provider_v2.h.
 * The V2 type lives in `uft::hal` (not the global namespace), so the
 * forward declaration must enter the right namespace. */
namespace uft::hal { class GreaseweazleProviderV2; }

/* MF-157 (P1.4): forward-declaration of the codegen-emitted wire-up
 * function. Implemented in generated/tab_hardware_wiring.gen.cpp (output
 * of tools/wiring_codegen.py from forms/tab_hardware.actions.yaml +
 * forms/tab_hardware.ui). The function needs deep access to HardwareTab's
 * private ui-pointer and the GreaseweazleProviderV2 instance — granted
 * via a friend declaration on the class below.
 *
 * `::HardwareTab` is fully qualified so the parameter type cannot be
 * mistaken for a fwd-decl into `uft::gui::generated::`. */
namespace uft::gui::generated {
    void wire_hardware_tab(::HardwareTab *self);
}

class HardwareTab : public QWidget
{
    Q_OBJECT

    /* MF-157 (P1.4): the codegen-emitted wire-up function needs to reach
     * `ui->btnX` (private), `m_gwProviderV2` (private), and the public
     * handler overload set declared below. Granting friendship to that
     * one function is preferable to making `ui` public — the deep access
     * stays narrowly scoped to one auto-generated translation unit that
     * lives in `generated/`. */
    friend void ::uft::gui::generated::wire_hardware_tab(::HardwareTab *self);

public:
    explicit HardwareTab(QWidget *parent = nullptr);
    ~HardwareTab();
    
    enum ControllerRole {
        RoleSource = 0,
        RoleDestination = 1
    };
    
    bool isConnected() const { return m_connected; }
    QString currentController() const { return m_controllerType; }
    ControllerRole currentRole() const { return m_controllerRole; }

    // MF-110 — exposed so WorkflowTab/FluxCaptureJob can drive the same
    // open Greaseweazle handle. Returns nullptr when not connected.
    // The pointer is owned by HardwareTab; do not free it from the caller.
    void *gwDevice() const { return m_gwDevice; }
    int detectedTracks() const { return m_detectedTracks; }
    int detectedHeads() const { return m_detectedHeads; }
    
    // Device info getters
    QString getDeviceName() const;
    QString getFirmwareVersion() const { return m_firmwareVersion; }
    QString getPortName() const { return m_portName; }
    int getHardwareModel() const { return m_hwModel; }

    /* ──────────────────────────────────────────────────────────────────
     *  MF-157 (P1.4) — Type-Driven HAL V2 surface
     *
     *  `currentProviderV2()` is the single accessor consumed by the
     *  codegen-emitted `wire_hardware_tab(self)` (see
     *  `forms/tab_hardware.actions.yaml`'s `provider_source:` entry).
     *  It returns the GreaseweazleProviderV2 instance when the GW
     *  controller is connected, and nullptr otherwise.
     *
     *  The handler overload set below is invoked by the codegen-emitted
     *  std::visit dispatch (one overload per variant alternative). Each
     *  handler is RESPONSIBLE for surfacing the forensic detail of its
     *  variant — never collapse `SectorMarginal`'s divergent_reads to a
     *  single sample (rule F-3), never reduce ProviderError to one line
     *  (rule F-4 — what / why / fix all surfaced).
     *
     *  Adding a new alternative to a Sum-Type in `outcomes.h` will fail
     *  to compile here until a matching overload is added. That is the
     *  forensic contract turning "we should preserve every case" into a
     *  build break.
     * ────────────────────────────────────────────────────────────────── */
    ::uft::hal::GreaseweazleProviderV2 *currentProviderV2() const noexcept;

    /* MotorOutcome variant — one overload per alternative (excl. shared
     * ProviderError / HardwareDisconnected / CapabilityRequiresPolicy
     * which are routed through the show* methods further below). */
    void onMotorOutcome(const ::uft::hal::MotorRunning &v);
    void onMotorOutcome(const ::uft::hal::MotorStopped &v);
    void onMotorOutcome(const ::uft::hal::MotorStalled &v);

    /* SeekOutcome variant. */
    void onSeekOutcome(const ::uft::hal::SeekArrived &v);
    void onSeekOutcome(const ::uft::hal::SeekOvershot &v);
    void onSeekOutcome(const ::uft::hal::SeekTrack0Failed &v);

    /* RpmOutcome variant. */
    void onRpmOutcome(const ::uft::hal::RpmMeasured &v);

    /* DetectOutcome variant. */
    void onDetectOutcome(const ::uft::hal::DriveDetected &v);
    void onDetectOutcome(const ::uft::hal::DriveAbsent &v);

    /* FluxOutcome variant. */
    void onFluxOutcome(const ::uft::hal::FluxCaptured &v);
    void onFluxOutcome(const ::uft::hal::FluxMarginal &v);
    void onFluxOutcome(const ::uft::hal::FluxUnreadable &v);

    /* Cross-variant alternatives — every Outcome variant carries these
     * three. Single implementation each (F-4 enforced — what/why/fix
     * surfaced verbatim). */
    void showProviderError(const ::uft::hal::ProviderError &e);
    void showHardwareDisconnected(const ::uft::hal::HardwareDisconnected &d);
    void showPolicyRequired(const ::uft::hal::CapabilityRequiresPolicy &p);

signals:
    void connectionChanged(bool connected);
    void statusMessage(const QString& message);
    void deviceInfoChanged(const QString& deviceName, const QString& firmware);

public slots:
    /**
     * @brief Enable/disable based on Workflow Tab selection
     * @param sourceMode true if Workflow source is Flux/USB, false if File
     * @param destMode true if Workflow dest is Flux/USB, false if File
     */
    void setWorkflowModes(bool sourceIsHardware, bool destIsHardware);

private slots:
    // Connection
    void onRefreshPorts();
    void autoRefreshPorts();
    void onConnect();
    void onDisconnect();
    void onControllerChanged(int index);
    
    // Role selection (Source/Destination)
    void onRoleChanged(int roleId);
    
    // Detection mode
    void onDetectionModeChanged();
    void onDetectDrive();
    
    // Motor control
    void onMotorOn();
    void onMotorOff();
    void onAutoSpinDownChanged(bool enabled);
    
    // Drive settings (Manual mode)
    void onDriveTypeChanged(int index);
    void onTracksChanged(int index);
    void onHeadsChanged(int index);
    void onDensityChanged(int index);
    void onRPMChanged(int index);
    
    // Advanced settings
    void onDoubleStepChanged(bool enabled);
    void onIgnoreIndexChanged(bool enabled);
    void onStepDelayChanged(int value);
    void onSettleTimeChanged(int value);
    
    // Tests
    void onSeekTest();
    void onReadTest();
    void onRPMTest();
    void onCalibrate();

    /* MF-169 (P1.17): `onUnifiedCapture()` removed with the V1 hierarchy.
     * It consumed the V1 `unified_hal_bridge`, both of which are gone.
     * The V2 wire_action codegen path (P1.4) replaces it for the
     * capability-bound test buttons. */

private:
    void setupConnections();
    void setupButtonGroups();
    void detectSerialPorts();
    void detectParallelPorts();
    
    // Controller list management
    void populateControllerList();
    void updateControllerListForRole();
    
    // UI State Management
    void updateUIState();
    void setConnectionState(bool connected);
    void setDetectionMode(bool autoDetect);
    void updateDriveSettingsEnabled();
    void updateMotorControlsEnabled();
    void updateAdvancedEnabled();
    void updateTestButtonsEnabled();
    void updateRoleButtonsEnabled();
    
    // Status updates
    void updateStatus(const QString& status, bool isError = false);
    void clearDetectedInfo();
    void setDetectedInfo(const QString& model, const QString& firmware, 
                         const QString& rpm, const QString& index);
    
    // Auto-detection
    void autoDetectDrive();
    void applyDetectedSettings(const QString& driveType, int tracks, 
                               int heads, const QString& density, int rpm);
    
    Ui::TabHardware *ui;
    QButtonGroup *m_detectionModeGroup;
    QButtonGroup *m_roleGroup;
    
    // State
    bool m_connected;
    bool m_autoDetect;
    bool m_motorRunning;
    ControllerRole m_controllerRole;
    
    // Workflow state (from WorkflowTab)
    bool m_sourceIsHardware;
    bool m_destIsHardware;
    
    // Hardware info
    QString m_controllerType;
    QString m_portName;
    QString m_firmwareVersion;
    int m_hwModel;              // Hardware model (e.g., F1=1, F7=7)
    void *m_gwDevice;           // HAL device handle (uft_gw_device_t*)

    /* MF-169 (P1.17): The V1 `HardwareManager` dispatcher was deleted
     * with the V1 provider hierarchy. Routing for non-Greaseweazle
     * controllers via V2 providers is task P1.18 — until then those
     * controllers display a "no V2 routing wired" message on Connect. */

    /* MF-157 (P1.4): V2 mixin-composition wrapper around the C-HAL
     * Greaseweazle handle (m_gwDevice). Created on connect, reset on
     * disconnect. The forward-declared dtor in greaseweazle_provider_v2.h
     * is sufficient for unique_ptr's destructor instantiation here
     * because HardwareTab's destructor is defined in hardwaretab.cpp
     * where the V2 type is complete. */
    std::unique_ptr<::uft::hal::GreaseweazleProviderV2> m_gwProviderV2;

    /* Re-runs `wire_hardware_tab(this)` so Phase-1 disconnect, Phase-2
     * disable, and Phase-3 wire-up reflect the current `currentProviderV2()`.
     * Called from `setupConnections()` (initial), `onConnect()` (after the
     * V2 wrapper is created), and `onDisconnect()` (before/after the
     * V2 wrapper is destroyed). */
    void rewireV2();

    // Detected drive info
    QString m_detectedModel;
    int m_detectedTracks;
    int m_detectedHeads;
    QString m_detectedDensity;
    int m_detectedRPM;
    
    // Timers
    QTimer *m_motorTimer;
    QTimer *m_statusTimer;
    QTimer *m_portRefreshTimer;
};

#endif // HARDWARETAB_H
