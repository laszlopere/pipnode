/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * This file is part of Pipnode.  Pipnode is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License version 3, with the additional permission described in
 * LICENSE.PLUGIN-EXCEPTION, as published by the Free Software Foundation.
 *
 * Pipnode is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; see the GNU General Public License for more details.  You
 * should have received a copy of the license in the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PN_MESH_CONNECTION_H
#define PN_MESH_CONNECTION_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* One Meshtastic channel slot as parsed out of the device.
 *
 * @name may be NULL or "" for an unnamed primary; the device's UI
 * treats both as "Default".  @psk is owned by the struct and may be
 * NULL on a disabled slot.  @role: 0 = DISABLED, 1 = PRIMARY,
 * 2 = SECONDARY, matching the Meshtastic Channel.Role enum. */
typedef struct
{
    guint32  index;
    gchar   *name;
    guint32  role;
    guint8  *psk;
    gsize    psk_size;
} PnMeshChannel;

void          pn_mesh_channel_free (PnMeshChannel *channel);

/* One entry from the device's NodeInfo database -- itself and every
 * other node it has heard.  Fields that the firmware did not include
 * for a given node stay at their zero value; @last_heard / @snr are
 * the most common "missing" pair (a freshly-flashed device that has
 * never heard a peer has only its own NodeInfo, and even that may
 * lack @snr).  Strings are owned by the struct; freed by
 * pn_mesh_node_free().  @is_us flags the entry whose @num matches
 * MyNodeInfo.my_node_num, so the page can paint the owning device's
 * own row distinctly. */
typedef struct
{
    guint32  num;             /* NodeInfo.num (the node's numeric id)   */
    gchar   *id;              /* User.id, hex-prefixed "!abcd1234"      */
    gchar   *long_name;       /* User.long_name                         */
    gchar   *short_name;      /* User.short_name                        */
    guint32  hw_model;        /* User.hw_model (HardwareModel enum)     */
    guint32  role;            /* User.role (DeviceRole enum)            */
    gfloat   snr;             /* NodeInfo.snr (dB; 0 = unset)           */
    guint32  last_heard;      /* NodeInfo.last_heard (unix seconds)     */
    guint32  hops_away;       /* NodeInfo.hops_away (0 = direct)        */
    guint32  battery_level;   /* DeviceMetrics.battery_level (0..101)   */
    gfloat   voltage;         /* DeviceMetrics.voltage (volts)          */
    gboolean is_us;           /* TRUE iff num == my_node_num            */
} PnMeshNode;

void          pn_mesh_node_free (PnMeshNode *node);

/* Read-only snapshot of the device state captured by the handshake
 * plus the get_device_metadata admin round-trip.  Strings owned by
 * the struct; channels owned by the struct. */
typedef struct
{
    /* From the handshake (MyNodeInfo + matching NodeInfo + Channels). */
    guint32     my_node_num;
    gchar      *owner_id;
    gchar      *owner_long_name;   /* <=39 chars by device contract */
    gchar      *owner_short_name;  /* <=4 chars by device contract  */
    guint32    owner_hw_model;     /* HardwareModel enum            */
    GPtrArray *channels;           /* PnMeshChannel*, index-ordered */
    GPtrArray *nodes;              /* PnMeshNode*, every NodeInfo the
                                    * device exposed (its own + every
                                    * peer it has heard).  Owner is
                                    * also present here; the entry has
                                    * is_us=TRUE.                    */
    gboolean   config_complete;    /* config_complete_id seen       */

    /* From AdminMessage.get_device_metadata_response.  Empty / 0
     * when the request was not issued or its response did not arrive
     * within budget; the dialog falls back to the User.hw_model field
     * for hardware id and shows "—" for firmware. */
    gboolean    have_metadata;
    gchar      *firmware_version;  /* DeviceMetadata.firmware_version */
    guint32     hw_model;          /* DeviceMetadata.hw_model         */
    guint32     role;              /* DeviceMetadata.role             */
    gboolean    has_wifi;
    gboolean    has_bluetooth;
    gboolean    has_ethernet;
    gboolean    can_shutdown;

    /* From FromRadio.config(LoRaConfig).  Populated during the
     * handshake when the device streams its config blocks; FALSE
     * means we didn't see one (or no device connected).  Field
     * meanings match the Meshtastic protobuf -- see TODO #29 for
     * the wire layout. */
    gboolean    have_lora_config;
    gboolean    lora_use_preset;    /* 1 = use named modem preset    */
    guint32     lora_modem_preset;  /* ModemPreset enum (LONG_FAST…) */
    guint32     lora_region;        /* Region enum (US/EU_868/…)     */
    guint32     lora_hop_limit;     /* 0..7                          */
    gboolean    lora_tx_enabled;
    guint32     lora_tx_power;      /* dBm                           */
    guint32     lora_channel_num;   /* fine-tune channel offset      */

    /* From FromRadio.moduleConfig (field 9) = ModuleConfig {
     *     external_notification (3) = ExternalNotificationConfig }.
     * Same FALSE-means-unseen contract as have_lora_config; the
     * page paints em-dashes and disables Apply until the device
     * has sent its current values.  Like LoRa, writes go through
     * the matching set_*_config_async so the verify-cycle picks
     * the device's reply up as the new truth. */
    gboolean    have_ext_notification;
    gboolean    en_enabled;
    guint32     en_output_ms;
    guint32     en_output;
    guint32     en_output_vibra;
    guint32     en_output_buzzer;
    gboolean    en_active;            /* TRUE = active-high           */
    gboolean    en_alert_message;
    gboolean    en_alert_message_vibra;
    gboolean    en_alert_message_buzzer;
    gboolean    en_alert_bell;
    gboolean    en_alert_bell_vibra;
    gboolean    en_alert_bell_buzzer;
    gboolean    en_use_pwm;
    guint32     en_nag_timeout;       /* seconds; 0 = no nag        */
    gboolean    en_use_i2s_as_buzzer;

    /* From FromRadio.moduleConfig (field 9) = ModuleConfig {
     *     mqtt (1) = MqttConfig }.  json_enabled is upstream-
     * deprecated and intentionally not exposed.  map_report_settings
     * is captured as opaque bytes so the writer can ship them back
     * verbatim -- the sub-fields are not exposed in the UI yet. */
    gboolean    have_mqtt;
    gboolean    mqtt_enabled;
    gchar      *mqtt_address;
    gchar      *mqtt_username;
    gchar      *mqtt_password;
    gboolean    mqtt_encryption_enabled;
    gboolean    mqtt_tls_enabled;
    gchar      *mqtt_root;
    gboolean    mqtt_proxy_to_client_enabled;
    gboolean    mqtt_map_reporting_enabled;
    GBytes     *mqtt_map_report_settings;  /* opaque sub-message    */

    /* From FromRadio.moduleConfig (field 9) = ModuleConfig {
     *     telemetry (6) = TelemetryConfig }.  Five logical sub-
     * systems (device / environment / air / power / health), each
     * with its own enable + interval + screen toggles. */
    gboolean    have_telemetry;
    gboolean    tel_device_telemetry_enabled;
    guint32     tel_device_update_interval;
    gboolean    tel_environment_measurement_enabled;
    guint32     tel_environment_update_interval;
    gboolean    tel_environment_screen_enabled;
    gboolean    tel_environment_display_fahrenheit;
    gboolean    tel_air_quality_enabled;
    guint32     tel_air_quality_interval;
    gboolean    tel_air_quality_screen_enabled;
    gboolean    tel_power_measurement_enabled;
    guint32     tel_power_update_interval;
    gboolean    tel_power_screen_enabled;
    gboolean    tel_health_measurement_enabled;
    guint32     tel_health_update_interval;
    gboolean    tel_health_screen_enabled;
} PnMeshState;

/* A live session to one device: an open serial fd plus the state
 * captured during the want_config_id handshake.  Owns its fd until
 * close(); designed to be held by the dialog for as long as the user
 * is configuring that device.  Phase 2d does the handshake and
 * exposes the state read-only; later phases (write paths, test
 * message, live monitor) extend it without changing this contract. */
typedef struct _PnMeshConnection PnMeshConnection;

/* Synchronous: open @tty_path, drain stale bytes, send ToRadio{
 * want_config_id = 1 }, read FromRadio frames until config_complete_id
 * arrives or 3 seconds of silence elapse (pip-mesh's pattern: some
 * firmwares never emit config_complete_id, so the budget is the
 * fallback signal).  Designed to run inside a GTask worker thread. */
PnMeshConnection *pn_mesh_connection_open_sync (const gchar  *tty_path,
                                                GError      **error);

void              pn_mesh_connection_close (PnMeshConnection *self);

/* Borrowed snapshot of what the handshake captured.  Valid until
 * pn_mesh_connection_close().  Phase 2e renders this on the Identity
 * page; later phases mutate via dedicated write functions and
 * refresh the state in place. */
const PnMeshState *pn_mesh_connection_get_state (PnMeshConnection *self);

const gchar       *pn_mesh_connection_get_tty   (PnMeshConnection *self);

/* GTask wrapper: runs open_sync() on a worker thread, fires
 * @callback on the calling thread.  Pair with finish() to collect
 * the result. */
void               pn_mesh_connection_open_async (
        const gchar         *tty_path,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

PnMeshConnection *pn_mesh_connection_open_finish (
        GAsyncResult        *result,
        GError             **error);

/* ------------------------------------------------------------------ */
/*  Admin protocol — Phase 3                                            */
/* ------------------------------------------------------------------ */

/* Send AdminMessage.set_owner with the given long/short names; both
 * may be NULL/"" to leave that field unchanged (pip-mesh contract).
 * After the write the function settles (sleep 0.5 s, per pip-mesh's
 * post-write pattern) and re-runs a want_config_id handshake so the
 * in-memory state reflects whatever the device now reports.  Returns
 * %TRUE on success; on failure @error is set and the state is left
 * untouched. */
gboolean pn_mesh_connection_set_owner_sync (PnMeshConnection *self,
                                            const gchar      *long_name,
                                            const gchar      *short_name,
                                            GError          **error);

void     pn_mesh_connection_set_owner_async (
        PnMeshConnection    *self,
        const gchar         *long_name,
        const gchar         *short_name,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

/* %TRUE on success; on failure %FALSE with @error set.  Use this
 * inside @callback to learn the outcome of set_owner_async. */
gboolean pn_mesh_connection_set_owner_finish (GAsyncResult *result,
                                              GError      **error);

/* All-at-once write of the LoRaConfig fields.  The caller has to
 * supply every field even when only changing one -- the device
 * replaces the whole sub-config in one shot, so omitting fields
 * would reset them to proto3 defaults (region=UNSET would brick
 * RF until a fresh handshake).  Read the current values from
 * PnMeshState->lora_* first, mutate the ones you care about,
 * pass them all back here. */
typedef struct
{
    gboolean use_preset;
    guint32  modem_preset;
    guint32  region;
    guint32  hop_limit;
    gboolean tx_enabled;
    guint32  tx_power;
    guint32  channel_num;
} PnMeshLoraConfigWrite;

gboolean pn_mesh_connection_set_lora_config_sync (
        PnMeshConnection            *self,
        const PnMeshLoraConfigWrite *cfg,
        GError                     **error);

void     pn_mesh_connection_set_lora_config_async (
        PnMeshConnection            *self,
        const PnMeshLoraConfigWrite *cfg,
        GCancellable                *cancellable,
        GAsyncReadyCallback          callback,
        gpointer                     user_data);

gboolean pn_mesh_connection_set_lora_config_finish (
        GAsyncResult                *result,
        GError                     **error);

/* ------------------------------------------------------------------ */
/*  ExternalNotification (ModuleConfig) — Phase 9                       */
/* ------------------------------------------------------------------ */

/* All-at-once write of ExternalNotificationConfig.  Same contract as
 * the LoRa writer: the device replaces the whole sub-config in one
 * shot so every field must be supplied even if only one is changing.
 * Read the current PnMeshState->en_* values first, mutate the ones
 * you care about, pass them all back here. */
typedef struct
{
    gboolean enabled;
    guint32  output_ms;
    guint32  output;
    guint32  output_vibra;
    guint32  output_buzzer;
    gboolean active;
    gboolean alert_message;
    gboolean alert_message_vibra;
    gboolean alert_message_buzzer;
    gboolean alert_bell;
    gboolean alert_bell_vibra;
    gboolean alert_bell_buzzer;
    gboolean use_pwm;
    guint32  nag_timeout;
    gboolean use_i2s_as_buzzer;
} PnMeshExtNotificationWrite;

gboolean pn_mesh_connection_set_ext_notification_sync (
        PnMeshConnection                  *self,
        const PnMeshExtNotificationWrite  *cfg,
        GError                           **error);

void     pn_mesh_connection_set_ext_notification_async (
        PnMeshConnection                  *self,
        const PnMeshExtNotificationWrite  *cfg,
        GCancellable                      *cancellable,
        GAsyncReadyCallback                callback,
        gpointer                           user_data);

gboolean pn_mesh_connection_set_ext_notification_finish (
        GAsyncResult                      *result,
        GError                           **error);

/* ------------------------------------------------------------------ */
/*  MQTT (ModuleConfig) — Phase 10                                      */
/* ------------------------------------------------------------------ */

/* All-at-once write of MqttConfig.  Same contract as the other
 * config writers: ship every field every time or the device resets
 * the omitted ones to proto3 zero.  @map_report_settings is the
 * opaque sub-message bytes the caller read back from
 * PnMeshState->mqtt_map_report_settings (may be NULL = empty).
 * The caller is responsible for re-shipping it verbatim. */
typedef struct
{
    gboolean       enabled;
    const gchar   *address;
    const gchar   *username;
    const gchar   *password;
    gboolean       encryption_enabled;
    gboolean       tls_enabled;
    const gchar   *root;
    gboolean       proxy_to_client_enabled;
    gboolean       map_reporting_enabled;
    GBytes        *map_report_settings;  /* may be NULL */
} PnMeshMqttConfigWrite;

gboolean pn_mesh_connection_set_mqtt_config_sync (
        PnMeshConnection             *self,
        const PnMeshMqttConfigWrite  *cfg,
        GError                      **error);

void     pn_mesh_connection_set_mqtt_config_async (
        PnMeshConnection             *self,
        const PnMeshMqttConfigWrite  *cfg,
        GCancellable                 *cancellable,
        GAsyncReadyCallback           callback,
        gpointer                      user_data);

gboolean pn_mesh_connection_set_mqtt_config_finish (
        GAsyncResult                 *result,
        GError                      **error);

/* ------------------------------------------------------------------ */
/*  Telemetry (ModuleConfig) — Phase 11                                 */
/* ------------------------------------------------------------------ */

/* All-at-once write of TelemetryConfig.  No sub-messages here, so
 * no opaque round-trip needed; just every typed field. */
typedef struct
{
    gboolean device_telemetry_enabled;
    guint32  device_update_interval;
    gboolean environment_measurement_enabled;
    guint32  environment_update_interval;
    gboolean environment_screen_enabled;
    gboolean environment_display_fahrenheit;
    gboolean air_quality_enabled;
    guint32  air_quality_interval;
    gboolean air_quality_screen_enabled;
    gboolean power_measurement_enabled;
    guint32  power_update_interval;
    gboolean power_screen_enabled;
    gboolean health_measurement_enabled;
    guint32  health_update_interval;
    gboolean health_screen_enabled;
} PnMeshTelemetryConfigWrite;

gboolean pn_mesh_connection_set_telemetry_config_sync (
        PnMeshConnection                  *self,
        const PnMeshTelemetryConfigWrite  *cfg,
        GError                           **error);

void     pn_mesh_connection_set_telemetry_config_async (
        PnMeshConnection                  *self,
        const PnMeshTelemetryConfigWrite  *cfg,
        GCancellable                      *cancellable,
        GAsyncReadyCallback                callback,
        gpointer                           user_data);

gboolean pn_mesh_connection_set_telemetry_config_finish (
        GAsyncResult                      *result,
        GError                           **error);

/* ------------------------------------------------------------------ */
/*  Channels                                                            */
/* ------------------------------------------------------------------ */

/* Write one channel slot.  @role: 0 = DISABLED (delete), 1 = PRIMARY,
 * 2 = SECONDARY.  @psk may be NULL with @psk_size = 0 (used both for
 * a DISABLED write and for a channel that wants no PSK -- the
 * device's "default" channel).  @name may be NULL or "" to clear it.
 *
 * Sends a BARE AdminMessage.set_channel (no begin_edit /
 * commit_edit transaction) -- pip-mesh's pattern for per-channel
 * Add and Delete: takes effect immediately, no flash write, no
 * device reboot.  Bulk multi-channel edits (Phase 6+ "import from
 * QR") will need the transactional pattern. */
gboolean pn_mesh_connection_set_channel_sync (
        PnMeshConnection *self,
        guint32           index,
        const gchar      *name,
        const guint8     *psk,
        gsize             psk_size,
        guint32           role,
        GError          **error);

void     pn_mesh_connection_set_channel_async (
        PnMeshConnection    *self,
        guint32              index,
        const gchar         *name,
        const guint8        *psk,
        gsize                psk_size,
        guint32              role,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

gboolean pn_mesh_connection_set_channel_finish (GAsyncResult *result,
                                                GError      **error);

G_END_DECLS

#endif /* PN_MESH_CONNECTION_H */
