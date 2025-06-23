#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <fcntl.h>
#include <security/_pam_types.h>
#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>

#define Z3_TOYS_SCOPED
#define Z3_STRING_IMPL
#define Z3_TOYS_IMPL
#include "lib/z3_string.h"

#define CONFIG_FILE            "/etc/pam_bluetooth.conf"
#define SYSCALL_MAX_BYTES_READ 1024
#define MAX_ITEM_LEN           256
#define MAX_DEVICES_LOOKDUP    20

#define UNUSED __attribute__ ((unused))

typedef struct {
  bdaddr_t device_addr;
  int request_update;
  int check_trusted;
  int min_strength;
} bt_config_t;

#define AUTO_CLOSE __attribute__ ((cleanup (close_fd)))
static void close_fd (int *fd) {
  if (*fd >= 0) close (*fd);
}

#define AUTO_FREE __attribute__ ((cleanup (auto_free)))
static void auto_free (char **ptr) {
  if (*ptr) {
    free (*ptr);
    *ptr = NULL;
  }
}

// Returns 1 if key-value pair found, 0 if end of buffer, -1 on error
static int parse_next_kv (
    const char *fbuffer,
    int read_res,
    int *pos,
    size_t *line,
    char *key,
    size_t *key_len,
    char *value,
    size_t *value_len,
    pam_handle_t *pamh
) {
  while (*pos < read_res) {
    if (fbuffer[*pos] == ' ') {
      (*pos)++;
      while (*pos < read_res && fbuffer[*pos] == ' ') (*pos)++;
      continue;
    }

    if (fbuffer[*pos] == '\n') {
      (*pos)++;
      while (*pos < read_res && fbuffer[*pos] == '\n') {
        (*pos)++;
        (*line)++;
      }
      continue;
    }

    if (fbuffer[*pos] == '#') {
      (*pos)++;
      while (*pos < read_res && fbuffer[*pos] != '\n') (*pos)++;
      if (*pos < read_res) (*pos)++;
      (*line)++;
      continue;
    }

    // key
    size_t n = 0;
    while (*pos < read_res && n < MAX_ITEM_LEN && fbuffer[*pos] != '=' &&
           fbuffer[*pos] != '\n' && fbuffer[*pos] != ' ') {
      key[n++] = fbuffer[(*pos)++];
    }
    key[n] = '\0';
    *key_len = n;

    // spaces before '='
    while (*pos < read_res && fbuffer[*pos] == ' ') (*pos)++;

    if (fbuffer[*pos] != '=') {
      pam_syslog (pamh, LOG_ERR, "Expected '=' after key: %s:%zu", CONFIG_FILE, *line);
      return -1;
    }
    (*pos)++;

    // spaces after '='
    while (*pos < read_res && fbuffer[*pos] == ' ') (*pos)++;

    // value
    n = 0;
    while (*pos < read_res && n < MAX_ITEM_LEN && fbuffer[*pos] != '\n') {
      value[n++] = fbuffer[(*pos)++];
    }
    value[n] = '\0';

    // trailing spaces from value
    while (n > 0 && value[n - 1] == ' ') {
      value[--n] = '\0';
    }

    // surrounding double quotes
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
      memmove (value, value + 1, n - 2);
      value[n - 2] = '\0';
      n -= 2;
    }
    *value_len = n;

    (*pos)++;  // newline
    return 1;
  }

  return 0;
}

static int read_config (pam_handle_t *pamh, bt_config_t *config) {
  AUTO_CLOSE int file = open (CONFIG_FILE, O_RDONLY);
  if (file == -1) {
    pam_syslog (pamh, LOG_ERR, "Cannot open config file: %s", CONFIG_FILE);
    return -1;
  }

  char device_str[18] = {0};  // MAC address string + `\0`
  int found_device = 0, found_strength = 0;

  AUTO_FREE char *fbuffer = malloc (SYSCALL_MAX_BYTES_READ);
  if (!fbuffer) {
    pam_syslog (pamh, LOG_ERR, "Memory allocation failed");
    return -1;
  }

  int read_res = read (file, fbuffer, SYSCALL_MAX_BYTES_READ);

  if (read_res == -1) {
    pam_syslog (pamh, LOG_ERR, "Could not read config file: %s", CONFIG_FILE);
    return -1;
  }

  if (read_res == 0) {
    pam_syslog (pamh, LOG_ERR, "Config file empty, required `device` field: %s", CONFIG_FILE);
    return -1;
  }

  // do not request a updated RSSI value
  config->request_update = 0;
  // Do not scan for paired devices around this device
  config->check_trusted = 0;

  int pos = 0;
  size_t line = 0;
  char key[MAX_ITEM_LEN], value[MAX_ITEM_LEN];
  size_t key_len, value_len;

  int parse_result;
  while ((parse_result = parse_next_kv (
              fbuffer, read_res, &pos, &line, key, &key_len, value, &value_len, pamh
          )) > 0) {
    if (strncmp (key, "device", 6) == 0) {
      strncpy (device_str, value, sizeof (device_str) - 1);

      if (str2ba (device_str, &config->device_addr) != 0) {
        pam_syslog (pamh, LOG_ERR, "Invalid MAC address line %zu: %s", line, value);
      } else {
        found_device = 1;
      }
    } else if (strncmp (key, "request_update", 14) == 0) {
      config->request_update = abs (atoi (value));
    } else if (strncmp (key, "check_trusted", 13) == 0) {
      config->check_trusted = abs (atoi (value));
    } else if (strncmp (key, "min_strength", 12) == 0) {
      int8_t strength = -abs (atoi (value));  // ensure negative

      // either user wrote 0, or NaN
      if (strength == 0) {
        pam_syslog (
            pamh, LOG_ERR, "Signal strength must be negative, on line %zu: %s", line, key
        );
        continue;
      }

      config->min_strength = strength;
      found_strength = 1;
    } else {
      pam_syslog (pamh, LOG_WARNING, "Unknown config key on line %zu: %s", line, key);
    }
  }

  if (!found_device) {
    pam_syslog (pamh, LOG_ERR, "No valid device MAC address found in config");
    return -1;
  }

  if (!found_strength) {
    pam_syslog (pamh, LOG_ERR, "No valid strength level found in config");
    return -1;
  }

  pam_syslog (pamh, LOG_DEBUG, "Config loaded successfully!");

  return 0;
}

// Returns 1 if device is trusted, 0 if not trusted or file doesn't exist, -1 on error
static int is_device_trusted (
    pam_handle_t *pamh, const char *device_mac, const char *gadget_mac
) {
  if (getuid() != 1) {
    pam_syslog (pamh, LOG_WARNING, "Cannot open Bluetooth info file without root, assuming untrusted");
    return 0;
  }

  ScopedString infof_path = z3_str (128);
  z3_pushl (&infof_path, "/var/lib/bluetooth/", 19);
  z3_pushl (&infof_path, device_mac, 17);
  z3_pushc (&infof_path, '/');
  z3_pushl (&infof_path, gadget_mac, 17);
  z3_pushl (&infof_path, "/info", 5);

  AUTO_CLOSE int file = open (infof_path.chr, O_RDONLY);
  if (file == -1) {
    pam_syslog (pamh, LOG_ERR, "Cannot open bluetooth info file: %s", infof_path.chr);
    return 0;
  }

  AUTO_FREE char *fbuffer = malloc (SYSCALL_MAX_BYTES_READ);
  if (!fbuffer) {
    pam_syslog (pamh, LOG_ERR, "Memory allocation failed");
    return -1;
  }

  int read_res = read (file, fbuffer, SYSCALL_MAX_BYTES_READ);
  if (read_res <= 0) {
    pam_syslog (pamh, LOG_DEBUG, "Could not read bluetooth info file: %s", infof_path.chr);
    return 0;
  }

  int pos = 0;
  size_t line = 0;
  char key[MAX_ITEM_LEN], value[MAX_ITEM_LEN];
  size_t key_len, value_len;

  int parse_result;
  while ((parse_result = parse_next_kv (
              fbuffer, read_res, &pos, &line, key, &key_len, value, &value_len, pamh
          )) > 0) {
    if (strncmp (key, "Trusted", 7) == 0) {
      if (strncmp (value, "true", 4) == 0) {
        return 1;
      } else {
        return 0;
      }
    }
  }

  if (parse_result < 0) {
    pam_syslog (pamh, LOG_ERR, "Parse error in bluetooth info file: %s", infof_path.chr);
    return -1;
  }

  return 0;
}

int8_t dev_get_rssi (pam_handle_t *pamh, int dev_id, uint16_t handle) {
  AUTO_CLOSE int sock = hci_open_dev (dev_id);
  if (sock < 0) {
    pam_syslog (pamh, LOG_ERR, "Device (handle: %d) hci_open_dev failed", handle);
    return -1;
  }

  int8_t rssi;
  int err = hci_read_rssi (sock, handle, &rssi, 1000);
  if (err < 0) {
    pam_syslog (pamh, LOG_ERR, "Device (handle: %d) hci_read_rssi failed", handle);
    return -1;
  }

  return rssi;
}

int8_t get_fresh_rssi (pam_handle_t *pamh, int hci_sock, uint16_t handle) {
  struct hci_request rq;
  read_rssi_rp rp;
  uint16_t cmd_handle = htobs (handle);

  memset (&rq, 0, sizeof (rq));
  rq.ogf = OGF_STATUS_PARAM;
  rq.ocf = OCF_READ_RSSI;
  rq.cparam = &cmd_handle;
  rq.clen = sizeof (cmd_handle);
  rq.rparam = &rp;
  rq.rlen = READ_RSSI_RP_SIZE;

  if (hci_send_req (hci_sock, &rq, 1000) < 0) {
    pam_syslog (pamh, LOG_ERR, "Device (handle: %d) hci_send_req failed", handle);
    return 0;
  }

  if (rp.status != 0) {
    pam_syslog (pamh, LOG_ERR, "Device (handle: %d) hci_send_req status failure", handle);
    return 0;
  }

  return rp.rssi;
}

static bool check_paired_device_proximity (
    pam_handle_t *pamh, int hci_sock, bdaddr_t *target_addr, int8_t min_strength
) {
  char name[248];
  int8_t rssi;

  // this establishes temporary connection
  if (hci_read_remote_name_with_clock_offset (
          hci_sock, target_addr, 0x02, 0, sizeof (name), name, 500
      ) < 0) {
    pam_syslog (pamh, LOG_DEBUG, "Device not reachable or powered off");
    return false;
  }

  if (hci_read_rssi (hci_sock, 0, &rssi, 100) == 0) {
    char addr_str[18];
    ba2str (target_addr, addr_str);
    pam_syslog (pamh, LOG_DEBUG, "Paired device %s nearby with RSSI: %d dBm", addr_str, rssi);

    return (rssi >= min_strength);
  }

  // RSSI read fails but name read succeeded, consider device is nearby
  pam_syslog (pamh, LOG_DEBUG, "Paired device nearby (no RSSI available)");
  return true;
}

static bool check_paired_device (
    pam_handle_t *pamh, bt_config_t *config, int hci_sock, char *bt_adapter_addrs
) {
  pam_syslog (pamh, LOG_DEBUG, "Checking for nearby paired Bluetooth device...");

  // First check if device is trusted (if check_trusted is enabled)
  if (config->check_trusted) {
    char addr_str[18];
    ba2str (&config->device_addr, addr_str);

    int trust_result = is_device_trusted (pamh, bt_adapter_addrs, addr_str);
    if (trust_result < 0) {
      pam_syslog (pamh, LOG_ERR, "Error checking trust status");
      return false;
    }

    if (trust_result == 0) {
      pam_syslog (pamh, LOG_WARNING, "Device not trusted");
      return false;
    }

    pam_syslog (pamh, LOG_DEBUG, "Device is trusted, checking proximity...");
  }

  bool proximity_result = check_paired_device_proximity (
      pamh, hci_sock, &config->device_addr, config->min_strength
  );

  return proximity_result;
}

static int check_connected_device (
    pam_handle_t *pamh, bt_config_t *config, int dev_id, int hci_sock
) {
  pam_syslog (pamh, LOG_DEBUG, "Checking for connected Bluetooth devices...");

  struct hci_conn_list_req *conn_list;
  struct hci_conn_info *conn_info;

  conn_list = malloc (sizeof (*conn_list) + MAX_DEVICES_LOOKDUP * sizeof (*conn_info));
  if (!conn_list) {
    pam_syslog (pamh, LOG_ERR, "Memory allocation failed");
    return -1;
  }

  conn_list->dev_id = dev_id;
  conn_list->conn_num = MAX_DEVICES_LOOKDUP;
  conn_info = conn_list->conn_info;

  int get_con_res = ioctl (hci_sock, HCIGETCONNLIST, conn_list);
  if (get_con_res < 0) {
    pam_syslog (pamh, LOG_ERR, "Failed to get connection list");
    free (conn_list);
    return 0;
  }

  pam_syslog (pamh, LOG_DEBUG, "Found %d connected devices", conn_list->conn_num);

  if (conn_list->conn_num == 0) {
    free (conn_list);
    return 0;
  }


  // check each connected device
  char addr_str[18];
  for (int i = 0; i < conn_list->conn_num; i++) {
    ba2str (&conn_info[i].bdaddr, addr_str);

    if (bacmp (&conn_info[i].bdaddr, &config->device_addr) == 0) {
      int8_t rssi = (config->request_update)
                      ? get_fresh_rssi (pamh, hci_sock, conn_info[i].handle)
                      : dev_get_rssi (pamh, conn_list->dev_id, conn_info[i].handle);

      // Fallback to cache values
      rssi = rssi != 0 ? rssi : dev_get_rssi (pamh, dev_id, conn_info[i].handle);

      pam_syslog (
          pamh, LOG_DEBUG, "Device %s found with RSSI: %d dBm (need: %d dBm)", addr_str, rssi,
          config->min_strength
      );

      if (rssi == 0) {
        pam_syslog (pamh, LOG_WARNING, "Device signal strength is not valid, ignored");
        return 0;
      } else if (rssi >= config->min_strength) {
        pam_syslog (pamh, LOG_INFO, "Device signal strength sufficient for authentication");
        return 1;
      } else {
        pam_syslog (pamh, LOG_WARNING, "Device found but signal too weak");
        return -1;
      }
    }
  }

  free (conn_list);

  return 0;
}

// bluetooth device signal strength using BlueZ, unlocking if found matching
static bool check_bluetooth_device (pam_handle_t *pamh, bt_config_t *config) {
  // default HCI device
  int dev_id = hci_get_route (NULL);
  if (dev_id < 0) {
    pam_syslog (pamh, LOG_ERR, "No Bluetooth adapter found");
    return false;
  }

  // struct hci_dev_info bt_adapter;
  // int stat__ = hci_devinfo (dev_id, &bt_adapter);
  // if (stat__ < 0) {
  //   pam_syslog (pamh, LOG_ERR, "Could not get info of current Bluetooth adapter");
  //   return false;
  // }

  bdaddr_t local_addr;
  if (hci_devba (dev_id, &local_addr) < 0) {
    pam_syslog (pamh, LOG_ERR, "Could not get local adapter address");
    return false;
  }

  char bt_adapter_addrs[18];
  int did = ba2str (&local_addr, bt_adapter_addrs);
  // int did = ba2str (&bt_adapter.bdaddr, bt_adapter_addrs);

  if (did < 0) {
    pam_syslog (pamh, LOG_ERR, "Failed to get MAC string");
    return false;
  }

  pam_syslog (pamh, LOG_DEBUG, "Current listener device %s", bt_adapter_addrs);

  // HCI socket
  AUTO_CLOSE int hci_sock = hci_open_dev (dev_id);
  if (hci_sock < 0) {
    pam_syslog (pamh, LOG_ERR, "Cannot open HCI socket");
    return false;
  }

  int conn_is = check_connected_device (pamh, config, dev_id, hci_sock);
  if (conn_is == 0) return check_paired_device (pamh, config, hci_sock, bt_adapter_addrs);

  return (conn_is == 1);
}

PAM_EXTERN int pam_sm_authenticate (
    pam_handle_t *pamh, int flags UNUSED, int argc, const char **argv
) {
  bt_config_t config;
  int allow_with_password = 0;

  for (int i = 0; i < argc; i++) {
    if (strcmp (argv[i], "allow_with_password") == 0) {
      allow_with_password = 1;
    }
  }

  if (read_config (pamh, &config) != 0) {
    return PAM_AUTH_ERR;
  }

  const char *password = NULL;
  int retval = pam_get_authtok (pamh, PAM_AUTHTOK, &password, NULL);
  if (retval != PAM_SUCCESS) {
    pam_syslog (pamh, LOG_ERR, "Failed to get password");
    return retval;
  }

  int has_password = (password && password[0] != '\0');

  if (has_password && !allow_with_password) {
    pam_syslog (pamh, LOG_DEBUG, "Non-empty password provided, rejecting");
    return PAM_AUTH_ERR;
  }

  pam_syslog (pamh, LOG_DEBUG, "Initiating Bluetooth authentication");

  // check Bluetooth device
  if (check_bluetooth_device (pamh, &config)) {
    pam_syslog (pamh, LOG_DEBUG, "Bluetooth authentication successful");
    return PAM_SUCCESS;
  } else {
    pam_syslog (pamh, LOG_DEBUG, "Bluetooth authentication failed");
    return PAM_AUTH_ERR;
  }
}

PAM_EXTERN int pam_sm_setcred (
    pam_handle_t *pamh UNUSED, int flags UNUSED, int argc UNUSED, const char **argv UNUSED
) {
  return PAM_SUCCESS;
}
