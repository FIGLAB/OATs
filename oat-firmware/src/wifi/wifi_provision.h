#ifndef WIFI_PROVISION_H
#define WIFI_PROVISION_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wifi_provision_init()
 *
 * Call once during boot (before tasks that need WiFi).
 *
 * Boot sequence:
 *  1. Looks for saved SSID/password in NVS.
 *     If found → connects to WiFi, starts HTTP server, returns.
 *  2. If no saved credentials (or first boot):
 *     Starts a WiFi access point called "Tamabotchi-Setup".
 *     Connect your laptop to that network, open http://192.168.4.1,
 *     enter your home WiFi credentials and submit.
 *     The device saves them to NVS and reboots — from then on it
 *     connects automatically on every boot (step 1).
 */
void wifi_provision_init(void);

/** Returns true if the device is in AP provisioning mode. */
bool wifi_is_provisioning(void);

/** SSID of the setup access point (only valid when wifi_is_provisioning() is true). */
const char *wifi_get_ap_ssid(void);

/** IP address of the setup access point, e.g. "192.168.4.1". */
const char *wifi_get_ap_ip(void);

/** Unique device ID derived from MAC address, e.g. "A1B2C3D4E5F6". */
const char *wifi_get_dev_id(void);

/** Enter AP provisioning mode on next boot without erasing saved credentials.
 *  If the user submits new credentials they overwrite the old ones.
 *  If the user does nothing, the device reboots after a timeout and
 *  reconnects using the existing saved credentials. */
void wifi_enter_provisioning_mode(void);

/** Connect (or reconnect) to WiFi using the credentials stored in NVS.
 *  No-op if already connected. Logs a warning if no credentials are saved. */
void wifi_on(void);

/** Erase saved WiFi credentials from NVS and reboot into AP provisioning mode. */
void wifi_forget_credentials(void);

/** Disconnect and turn off the WiFi radio to save power.
 *  Call after all HTTP activity (file sync, etc.) is complete. */
void wifi_off(void);

/** Register a callback invoked when the AP provisioning timeout expires.
 *  wifi_off() is called automatically before the callback.
 *  If no callback is set, the device reboots on timeout (legacy behaviour). */
void wifi_set_ap_timeout_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_H
