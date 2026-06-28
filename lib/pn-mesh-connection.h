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
 * 2 = SECONDARY, matching the Meshtastic Channel.Role enum.
 *
 * @uplink_enabled / @downlink_enabled gate the MQTT gateway bridge for
 * this channel.  @position_precision is the ChannelSettings
 * ModuleSettings field: 0 = no position shared, 32 = precise location,
 * a value in between = an approximate (reduced-precision) location. */
typedef struct
{
    guint32  index;
    gchar   *name;
    guint32  role;
    guint8  *psk;
    gsize    psk_size;
    gboolean uplink_enabled;
    gboolean downlink_enabled;
    guint32  position_precision;
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

    /* From FromRadio.moduleConfig (field 9) = ModuleConfig {
     *     ambient_lighting (11) = AmbientLightingConfig }.  Drives an
     * addressable RGB status LED (NeoPixel / WS2812).  led_state is the
     * master on/off; current sets the per-channel drive level and
     * red/green/blue are 0-255 colour components. */
    gboolean    have_ambient_lighting;
    gboolean    al_led_state;
    guint32     al_current;
    guint32     al_red;
    guint32     al_green;
    guint32     al_blue;

    /* From FromRadio.config(PositionConfig).  Same FALSE-means-unseen
     * contract as have_lora_config.  Every scalar that the firmware
     * sends is parsed so the writer can ship them back verbatim --
     * proto3 defaults would otherwise reset omitted fields.  pos_gps_
     * enabled is the upstream-deprecated boolean; modern firmwares
     * gate GPS via @pos_gps_mode instead, but the legacy field is
     * still set on round-trip so older firmware keeps working. */
    gboolean    have_position;
    gboolean    pos_position_broadcast_smart_enabled;
    gboolean    pos_fixed_position;
    gboolean    pos_gps_enabled;
    guint32     pos_gps_update_interval;       /* seconds; 0 = device default */
    guint32     pos_position_broadcast_secs;   /* seconds */
    guint32     pos_position_flags;            /* bitfield (PositionFlag enum) */
    guint32     pos_rx_gpio;
    guint32     pos_tx_gpio;
    gint32      pos_broadcast_smart_min_distance;        /* metres */
    guint32     pos_broadcast_smart_min_interval_secs;   /* seconds */
    guint32     pos_gps_en_gpio;
    guint32     pos_gps_mode;                  /* GpsMode enum */

    /* LIVE GPS readout (not a setting): the local node's own
     * NodeInfo.position, captured for the is_us node during the
     * handshake.  Distinct from the pos_* PositionConfig settings above
     * -- these are what the GPS is actually reporting right now and back
     * the "test the GPS after applying settings" flow.  @gps_have_live_
     * position is FALSE until the device sends a Position for itself
     * (no GPS data yet / GPS still warming up). */
    gboolean    gps_have_live_position;
    guint32     gps_live_time;                 /* GPS fix unix time, 0 = none */
    guint32     gps_live_fix_type;             /* u-blox fix type, 0 = no fix */
    guint32     gps_live_sats_in_view;         /* satellites the GPS can see */

    /* From FromRadio.config(PowerConfig).  Same contract as the other
     * Config sub-blocks. */
    gboolean    have_power;
    gboolean    pow_is_power_saving;
    guint32     pow_on_battery_shutdown_after_secs;
    gfloat      pow_adc_multiplier_override;             /* 0 = device default */
    guint32     pow_wait_bluetooth_secs;
    guint32     pow_sds_secs;
    guint32     pow_ls_secs;
    guint32     pow_min_wake_secs;
    guint32     pow_device_battery_ina_address;

    /* From FromRadio.config(DeviceConfig).  Same contract as the other
     * Config sub-blocks: every field the firmware sent is parsed so the
     * writer can ship them back verbatim.  @dev_is_managed is the
     * upstream-deprecated boolean (moved to SecurityConfig); we round-
     * trip it so older firmware keeps a consistent state. */
    gboolean    have_device;
    guint32     dev_role;                                /* DeviceRole enum */
    gboolean    dev_serial_enabled;
    guint32     dev_button_gpio;
    guint32     dev_buzzer_gpio;
    guint32     dev_rebroadcast_mode;                    /* RebroadcastMode enum */
    guint32     dev_node_info_broadcast_secs;
    gboolean    dev_double_tap_as_button_press;
    gboolean    dev_is_managed;                          /* deprecated upstream */
    gboolean    dev_disable_triple_click;
    gchar      *dev_tzdef;                               /* may be NULL = "" */
    gboolean    dev_led_heartbeat_disabled;
    guint32     dev_buzzer_mode;                         /* BuzzerMode enum */

    /* From FromRadio.config(NetworkConfig).  @net_ipv4_config is the
     * opaque IpV4Config sub-message bytes (four fixed32 fields the UI
     * does not expose); shipped back verbatim on Apply so static-IP
     * settings survive a write.  Same opaque round-trip pattern as
     * @mqtt_map_report_settings. */
    gboolean    have_network;
    gboolean    net_wifi_enabled;
    gchar      *net_wifi_ssid;
    gchar      *net_wifi_psk;
    gchar      *net_ntp_server;
    gboolean    net_eth_enabled;
    guint32     net_address_mode;                        /* AddressMode enum */
    GBytes     *net_ipv4_config;                         /* opaque sub-message */
    gchar      *net_rsyslog_server;
    guint32     net_enabled_protocols;                   /* bitfield */
    gboolean    net_ipv6_enabled;

    /* From FromRadio.config(SecurityConfig).  Public/private/admin keys
     * are bytes (32-byte X25519 keypair, 0..N admin keys); captured as
     * GBytes / GPtrArray-of-GBytes so the writer can ship them back
     * verbatim.  The page presents the keys read-only (base64) — typing
     * 32 random bytes by hand is not a productive UI. */
    gboolean    have_security;
    GBytes     *sec_public_key;                          /* may be NULL */
    GBytes     *sec_private_key;                         /* may be NULL */
    GPtrArray  *sec_admin_keys;                          /* of GBytes*, never NULL */
    gboolean    sec_is_managed;
    gboolean    sec_serial_enabled;
    gboolean    sec_debug_log_api_enabled;
    gboolean    sec_admin_channel_enabled;

    /* From FromRadio.config(DisplayConfig).  Same FALSE-means-unseen
     * contract as the other Config sub-blocks; every field is parsed so
     * the writer can ship them back verbatim.  @disp_compass_orientation
     * is the only field the UI does not surface (board/mount specific);
     * it is round-tripped unchanged. */
    gboolean    have_display;
    guint32     disp_screen_on_secs;                     /* 0 = always on */
    guint32     disp_gps_format;                         /* GpsCoordinateFormat enum */
    guint32     disp_auto_screen_carousel_secs;          /* 0 = no carousel */
    gboolean    disp_compass_north_top;
    gboolean    disp_flip_screen;
    guint32     disp_units;                              /* DisplayUnits enum */
    guint32     disp_oled;                               /* OledType enum */
    guint32     disp_displaymode;                        /* DisplayMode enum */
    gboolean    disp_heading_bold;
    gboolean    disp_wake_on_tap_or_motion;
    guint32     disp_compass_orientation;                /* CompassOrientation enum */
    gboolean    disp_use_12h_clock;

    /* From FromRadio.config(BluetoothConfig).  Same contract.  Named
     * @have_bluetooth_config to keep it distinct from @has_bluetooth,
     * which is the device-metadata capability flag (does the hardware
     * have a BT radio at all). */
    gboolean    have_bluetooth_config;
    gboolean    bt_enabled;
    guint32     bt_mode;                                 /* PairingMode enum */
    guint32     bt_fixed_pin;                            /* 6-digit PIN for FIXED_PIN */
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
/*  Ambient Lighting (ModuleConfig) — Phase 11 (TODO #48.8)            */
/* ------------------------------------------------------------------ */

/* All-at-once write of AmbientLightingConfig.  No sub-messages here,
 * so no opaque round-trip needed; just every typed field.  Same
 * proto3-defaults contract as the other writers: ship them all. */
typedef struct
{
    gboolean led_state;
    guint32  current;
    guint32  red;
    guint32  green;
    guint32  blue;
} PnMeshAmbientLightingConfigWrite;

gboolean pn_mesh_connection_set_ambient_lighting_sync (
        PnMeshConnection                       *self,
        const PnMeshAmbientLightingConfigWrite *cfg,
        GError                                **error);

void     pn_mesh_connection_set_ambient_lighting_async (
        PnMeshConnection                       *self,
        const PnMeshAmbientLightingConfigWrite *cfg,
        GCancellable                           *cancellable,
        GAsyncReadyCallback                     callback,
        gpointer                                user_data);

gboolean pn_mesh_connection_set_ambient_lighting_finish (
        GAsyncResult                           *result,
        GError                                **error);

/* ------------------------------------------------------------------ */
/*  Position (Config sub-block) — Phase 12                              */
/* ------------------------------------------------------------------ */

/* All-at-once write of PositionConfig.  Same proto3-defaults contract
 * as the LoRa writer: read PnMeshState->pos_* first, mutate the ones
 * you care about, ship them ALL back here. */
typedef struct
{
    gboolean position_broadcast_smart_enabled;
    gboolean fixed_position;
    gboolean gps_enabled;          /* deprecated upstream; round-trip only */
    guint32  gps_update_interval;
    guint32  position_broadcast_secs;
    guint32  position_flags;
    guint32  rx_gpio;
    guint32  tx_gpio;
    gint32   broadcast_smart_min_distance;
    guint32  broadcast_smart_min_interval_secs;
    guint32  gps_en_gpio;
    guint32  gps_mode;
} PnMeshPositionConfigWrite;

gboolean pn_mesh_connection_set_position_config_sync (
        PnMeshConnection                 *self,
        const PnMeshPositionConfigWrite  *cfg,
        GError                          **error);

void     pn_mesh_connection_set_position_config_async (
        PnMeshConnection                 *self,
        const PnMeshPositionConfigWrite  *cfg,
        GCancellable                     *cancellable,
        GAsyncReadyCallback               callback,
        gpointer                          user_data);

gboolean pn_mesh_connection_set_position_config_finish (
        GAsyncResult                     *result,
        GError                          **error);

/* ------------------------------------------------------------------ */
/*  Live GPS probe — Phase 15                                           */
/* ------------------------------------------------------------------ */

/* One snapshot of the device's own live position, used to test whether
 * the GPS is present and communicating after applying GPS settings.
 * @device_responded is FALSE when the device stayed silent (e.g. still
 * rebooting after the config write) -- the caller should retry rather
 * than treat it as a failure.  @have_position is FALSE when the device
 * answered but has not produced any position for itself yet (GPS still
 * acquiring).  A GPS that is wired and talking will report
 * @sats_in_view > 0 well before it gets a @fix_type fix. */
typedef struct
{
    gboolean device_responded;
    gboolean have_position;
    guint32  time;            /* GPS fix unix time, 0 = none */
    guint32  fix_type;        /* u-blox fix type, 0 = no fix */
    guint32  sats_in_view;
} PnMeshGpsProbe;

/* Re-read the device's node database and snapshot the local node's live
 * position.  Runs on a worker thread (it owns the serial fd for the
 * duration, same as the set_*_config writers), so the dialog's monitor
 * pump must be quiesced (push the busy overlay) while it is in flight. */
void     pn_mesh_connection_probe_gps_async (
        PnMeshConnection                 *self,
        GCancellable                     *cancellable,
        GAsyncReadyCallback               callback,
        gpointer                          user_data);

gboolean pn_mesh_connection_probe_gps_finish (
        GAsyncResult                     *result,
        PnMeshGpsProbe                   *out,
        GError                          **error);

/* ------------------------------------------------------------------ */
/*  Power (Config sub-block) — Phase 12                                 */
/* ------------------------------------------------------------------ */

/* All-at-once write of PowerConfig.  @adc_multiplier_override is a
 * float (wire type 5); 0.0f means "let the firmware use its built-in
 * calibration for this hardware". */
typedef struct
{
    gboolean is_power_saving;
    guint32  on_battery_shutdown_after_secs;
    gfloat   adc_multiplier_override;
    guint32  wait_bluetooth_secs;
    guint32  sds_secs;
    guint32  ls_secs;
    guint32  min_wake_secs;
    guint32  device_battery_ina_address;
} PnMeshPowerConfigWrite;

gboolean pn_mesh_connection_set_power_config_sync (
        PnMeshConnection              *self,
        const PnMeshPowerConfigWrite  *cfg,
        GError                       **error);

void     pn_mesh_connection_set_power_config_async (
        PnMeshConnection              *self,
        const PnMeshPowerConfigWrite  *cfg,
        GCancellable                  *cancellable,
        GAsyncReadyCallback            callback,
        gpointer                       user_data);

gboolean pn_mesh_connection_set_power_config_finish (
        GAsyncResult                  *result,
        GError                       **error);

/* ------------------------------------------------------------------ */
/*  Device (Config sub-block) — Phase 14                                */
/* ------------------------------------------------------------------ */

/* All-at-once write of DeviceConfig.  Same proto3-defaults contract as
 * the other Config writers: ship every field every time or the device
 * resets the omitted ones to zero on its next save.  @tzdef may be
 * NULL or "" for an unset timezone string. */
typedef struct
{
    guint32      role;
    gboolean     serial_enabled;
    guint32      button_gpio;
    guint32      buzzer_gpio;
    guint32      rebroadcast_mode;
    guint32      node_info_broadcast_secs;
    gboolean     double_tap_as_button_press;
    gboolean     is_managed;            /* deprecated upstream; round-trip only */
    gboolean     disable_triple_click;
    const gchar *tzdef;                 /* may be NULL */
    gboolean     led_heartbeat_disabled;
    guint32      buzzer_mode;
} PnMeshDeviceConfigWrite;

gboolean pn_mesh_connection_set_device_config_sync (
        PnMeshConnection               *self,
        const PnMeshDeviceConfigWrite  *cfg,
        GError                        **error);

void     pn_mesh_connection_set_device_config_async (
        PnMeshConnection               *self,
        const PnMeshDeviceConfigWrite  *cfg,
        GCancellable                   *cancellable,
        GAsyncReadyCallback             callback,
        gpointer                        user_data);

gboolean pn_mesh_connection_set_device_config_finish (
        GAsyncResult                   *result,
        GError                        **error);

/* ------------------------------------------------------------------ */
/*  Network (Config sub-block) — Phase 14                               */
/* ------------------------------------------------------------------ */

/* All-at-once write of NetworkConfig.  @ipv4_config is the opaque
 * IpV4Config sub-message bytes the caller read back from
 * PnMeshState->net_ipv4_config; the caller re-ships it verbatim (may
 * be NULL = empty/unset).  Strings may be NULL or "". */
typedef struct
{
    gboolean     wifi_enabled;
    const gchar *wifi_ssid;
    const gchar *wifi_psk;
    const gchar *ntp_server;
    gboolean     eth_enabled;
    guint32      address_mode;
    GBytes      *ipv4_config;           /* may be NULL */
    const gchar *rsyslog_server;
    guint32      enabled_protocols;
    gboolean     ipv6_enabled;
} PnMeshNetworkConfigWrite;

gboolean pn_mesh_connection_set_network_config_sync (
        PnMeshConnection                *self,
        const PnMeshNetworkConfigWrite  *cfg,
        GError                         **error);

void     pn_mesh_connection_set_network_config_async (
        PnMeshConnection                *self,
        const PnMeshNetworkConfigWrite  *cfg,
        GCancellable                    *cancellable,
        GAsyncReadyCallback              callback,
        gpointer                         user_data);

gboolean pn_mesh_connection_set_network_config_finish (
        GAsyncResult                    *result,
        GError                         **error);

/* ------------------------------------------------------------------ */
/*  Security (Config sub-block) — Phase 14                              */
/* ------------------------------------------------------------------ */

/* All-at-once write of SecurityConfig.  Key bytes are passed through
 * verbatim: the dialog does not let the user edit them, but every
 * field still has to be shipped on Apply or the device resets them to
 * proto3 zero (which would wipe the device's identity).  @public_key,
 * @private_key may be NULL with their respective size 0 = unset;
 * @admin_keys may be NULL or empty for "no admin keys configured". */
typedef struct
{
    GBytes      *public_key;            /* may be NULL */
    GBytes      *private_key;           /* may be NULL */
    GPtrArray   *admin_keys;            /* of GBytes*; may be NULL */
    gboolean     is_managed;
    gboolean     serial_enabled;
    gboolean     debug_log_api_enabled;
    gboolean     admin_channel_enabled;
} PnMeshSecurityConfigWrite;

gboolean pn_mesh_connection_set_security_config_sync (
        PnMeshConnection                 *self,
        const PnMeshSecurityConfigWrite  *cfg,
        GError                          **error);

void     pn_mesh_connection_set_security_config_async (
        PnMeshConnection                 *self,
        const PnMeshSecurityConfigWrite  *cfg,
        GCancellable                     *cancellable,
        GAsyncReadyCallback               callback,
        gpointer                          user_data);

gboolean pn_mesh_connection_set_security_config_finish (
        GAsyncResult                     *result,
        GError                          **error);

/* ------------------------------------------------------------------ */
/*  Display (Config sub-block) — Phase 13                               */
/* ------------------------------------------------------------------ */

/* All-at-once write of DisplayConfig.  Same proto3-defaults contract as
 * the other Config writers: ship every field every time or the device
 * resets the omitted ones to zero on its next save.  @compass_orientation
 * is read back from PnMeshState->disp_compass_orientation and re-shipped
 * verbatim -- the dialog does not expose it. */
typedef struct
{
    guint32  screen_on_secs;
    guint32  gps_format;
    guint32  auto_screen_carousel_secs;
    gboolean compass_north_top;
    gboolean flip_screen;
    guint32  units;
    guint32  oled;
    guint32  displaymode;
    gboolean heading_bold;
    gboolean wake_on_tap_or_motion;
    guint32  compass_orientation;       /* round-trip only */
    gboolean use_12h_clock;
} PnMeshDisplayConfigWrite;

gboolean pn_mesh_connection_set_display_config_sync (
        PnMeshConnection                *self,
        const PnMeshDisplayConfigWrite  *cfg,
        GError                         **error);

void     pn_mesh_connection_set_display_config_async (
        PnMeshConnection                *self,
        const PnMeshDisplayConfigWrite  *cfg,
        GCancellable                    *cancellable,
        GAsyncReadyCallback              callback,
        gpointer                         user_data);

gboolean pn_mesh_connection_set_display_config_finish (
        GAsyncResult                    *result,
        GError                         **error);

/* ------------------------------------------------------------------ */
/*  Bluetooth (Config sub-block) — Phase 13                             */
/* ------------------------------------------------------------------ */

/* All-at-once write of BluetoothConfig.  @mode is the PairingMode enum
 * (0 = RANDOM_PIN, 1 = FIXED_PIN, 2 = NO_PIN); @fixed_pin is only used
 * by the device when @mode == FIXED_PIN but is shipped regardless. */
typedef struct
{
    gboolean enabled;
    guint32  mode;
    guint32  fixed_pin;
} PnMeshBluetoothConfigWrite;

gboolean pn_mesh_connection_set_bluetooth_config_sync (
        PnMeshConnection                  *self,
        const PnMeshBluetoothConfigWrite  *cfg,
        GError                           **error);

void     pn_mesh_connection_set_bluetooth_config_async (
        PnMeshConnection                  *self,
        const PnMeshBluetoothConfigWrite  *cfg,
        GCancellable                      *cancellable,
        GAsyncReadyCallback                callback,
        gpointer                           user_data);

gboolean pn_mesh_connection_set_bluetooth_config_finish (
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
/* @transactional wraps the write in begin_edit_settings /
 * commit_edit_settings, which is required for an in-place edit of an
 * existing slot to persist.  Use FALSE for add-to-free-slot and delete
 * (a bare set_channel, which avoids the commit-to-flash + possible
 * reboot). */
gboolean pn_mesh_connection_set_channel_sync (
        PnMeshConnection *self,
        guint32           index,
        const gchar      *name,
        const guint8     *psk,
        gsize             psk_size,
        guint32           role,
        gboolean          uplink_enabled,
        gboolean          downlink_enabled,
        guint32           position_precision,
        gboolean          transactional,
        GError          **error);

void     pn_mesh_connection_set_channel_async (
        PnMeshConnection    *self,
        guint32              index,
        const gchar         *name,
        const guint8        *psk,
        gsize                psk_size,
        guint32              role,
        gboolean             uplink_enabled,
        gboolean             downlink_enabled,
        guint32              position_precision,
        gboolean             transactional,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

gboolean pn_mesh_connection_set_channel_finish (GAsyncResult *result,
                                                GError      **error);

/* ------------------------------------------------------------------ */
/*  Test page (Phase 7) — text send + live receive                      */
/* ------------------------------------------------------------------ */

/* One TEXT_MESSAGE_APP packet observed on the connection -- either
 * received from the radio while the monitor is active, or echoed
 * back as a synthetic event after a successful local send (so the
 * log on the Test page reads as a single chat transcript without
 * the page having to track its own outgoing history).  Strings are
 * owned by the event; free with pn_mesh_text_event_free. */
typedef struct
{
    guint32   from_node;     /* MeshPacket.from (0 = unknown / local) */
    guint32   channel;       /* MeshPacket.channel (0 = primary)      */
    gchar    *text;          /* UTF-8 payload                          */
    gint64    epoch_us;      /* g_get_real_time() at capture           */
    gboolean  outgoing;      /* TRUE iff synthesised by send_text_*    */
} PnMeshTextEvent;

void  pn_mesh_text_event_free (PnMeshTextEvent *event);

/* Pop the next text event from the connection's queue, or %NULL if
 * the queue is empty.  Safe to call from any thread but in practice
 * the dialog drains it from the main thread on a periodic timer.
 * The caller owns the returned event and must free it with
 * pn_mesh_text_event_free. */
PnMeshTextEvent *pn_mesh_connection_take_text_event (PnMeshConnection *self);

/* Drain any bytes the serial fd has buffered, feed them to the
 * frame reader, and parse out anything decodable -- TEXT_MESSAGE_APP
 * packets get queued via take_text_event.  Non-blocking: returns
 * promptly even when the device is silent.  MUST be called from the
 * main thread, and MUST NOT be called while a write/handshake is in
 * flight on a worker thread (the dialog enforces this via its
 * busy_count, skipping the pump when any page is mid-write).
 *
 * Returns the number of frames processed this tick, for diagnostics
 * (most ticks will return 0). */
gint              pn_mesh_connection_pump_monitor (PnMeshConnection *self);

/* Broadcast @text on @channel_index as a TEXT_MESSAGE_APP packet
 * (MeshPacket { to=0xFFFFFFFF, channel=@channel_index, decoded=Data
 * { portnum=TEXT_MESSAGE_APP, payload=@text } }).  No want_ack, no
 * settle, no verify-cycle -- a single frame write and done.  The
 * connection also synthesises an "outgoing" PnMeshTextEvent on
 * success so callers using the event queue see their own sends in
 * the same stream as incoming traffic. */
gboolean pn_mesh_connection_send_text_sync (PnMeshConnection *self,
                                            guint32           channel_index,
                                            const gchar      *text,
                                            GError          **error);

void     pn_mesh_connection_send_text_async (
        PnMeshConnection    *self,
        guint32              channel_index,
        const gchar         *text,
        GCancellable        *cancellable,
        GAsyncReadyCallback  callback,
        gpointer             user_data);

gboolean pn_mesh_connection_send_text_finish (GAsyncResult *result,
                                              GError      **error);

G_END_DECLS

#endif /* PN_MESH_CONNECTION_H */
