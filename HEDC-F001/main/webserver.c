#include "webserver.h"
#include "simple_dns_server.h"
#include "webpage.h"
#include "settings.h"
#include "alarm.h"
#include "audio.h"
#include "display.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

static const char *TAG = "webserver";

#define AP_SSID     "dotclock"
#define AP_PASS     "dotclock1"
#define AP_CHANNEL  1
#define AP_MAX_CONN 2
#define STORAGE_BASE "/storage"

static httpd_handle_t server = NULL;
static bool sta_connected = false;
static bool ap_enabled = false;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// Declared in alarm.c
extern bool time_is_set(void);
extern void time_set_tz(int offset_minutes);
extern void time_set_epoch(int64_t epoch);
extern int64_t time_get_epoch(void);

// --- URL decode helper ---

static void url_decode(char *dst, const char *src, size_t dst_len)
{
	size_t di = 0;
	for (size_t si = 0; src[si] && di < dst_len - 1; si++) {
		if (src[si] == '%' && src[si + 1] && src[si + 2]) {
			char hex[3] = { src[si + 1], src[si + 2], '\0' };
			dst[di++] = (char)strtol(hex, NULL, 16);
			si += 2;
		} else if (src[si] == '+') {
			dst[di++] = ' ';
		} else {
			dst[di++] = src[si];
		}
	}
	dst[di] = '\0';
}

// Extract value for a key from url-encoded body. Returns true if found.
static bool parse_form_value(const char *body, const char *key,
                             char *val, size_t val_len)
{
	size_t klen = strlen(key);
	const char *p = body;
	while ((p = strstr(p, key)) != NULL) {
		// Check it's at start or preceded by &
		if (p != body && *(p - 1) != '&') { p += klen; continue; }
		if (p[klen] != '=') { p += klen; continue; }
		p += klen + 1;
		const char *end = strchr(p, '&');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len >= val_len) len = val_len - 1;
		char encoded[256];
		if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
		memcpy(encoded, p, len);
		encoded[len] = '\0';
		url_decode(val, encoded, val_len);
		return true;
	}
	return false;
}

// --- Handlers ---

static esp_err_t root_get_handler(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	return httpd_resp_send(req, html_content, html_content_len);
}

static esp_err_t files_get_handler(httpd_req_t *req)
{
	DIR *dir = opendir(STORAGE_BASE);
	if (!dir) {
		httpd_resp_set_type(req, "application/json");
		return httpd_resp_send(req, "[]", 2);
	}

	char buf[1024];
	int pos = 0;
	buf[pos++] = '[';
	bool first = true;

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_type != DT_REG) continue;

		char path[300];
		snprintf(path, sizeof(path), STORAGE_BASE "/%s", entry->d_name);
		struct stat st;
		if (stat(path, &st) != 0) continue;

		int written = snprintf(buf + pos, sizeof(buf) - pos,
			"%s{\"name\":\"%s\",\"size\":%ld}",
			first ? "" : ",", entry->d_name, (long)st.st_size);
		if (written < 0 || pos + written >= (int)sizeof(buf) - 2) break;
		pos += written;
		first = false;
	}
	closedir(dir);

	buf[pos++] = ']';
	buf[pos] = '\0';

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, buf, pos);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
	static char buf[4096];
	char filename[64] = "";
	int total = req->content_len;
	int received = 0;
	FILE *f = NULL;
	bool header_parsed = false;
	int body_start = 0;
	char boundary[128] = "";
	int boundary_len = 0;

	const char *ct = NULL;
	size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
	char ct_buf[256];
	if (ct_len > 0 && ct_len < sizeof(ct_buf)) {
		httpd_req_get_hdr_value_str(req, "Content-Type", ct_buf, sizeof(ct_buf));
		ct = ct_buf;
		char *bp = strstr(ct, "boundary=");
		if (bp) {
			bp += 9;
			snprintf(boundary, sizeof(boundary), "--%s", bp);
			boundary_len = strlen(boundary);
		}
	}

	while (received < total) {
		int to_read = total - received;
		if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);
		int ret = httpd_req_recv(req, buf, to_read);
		if (ret <= 0) {
			if (f) fclose(f);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
			return ESP_FAIL;
		}

		if (!header_parsed) {
			for (int i = 0; i < ret - 3; i++) {
				if (buf[i] == '\r' && buf[i+1] == '\n' &&
				    buf[i+2] == '\r' && buf[i+3] == '\n') {
					buf[i] = '\0';
					char *fn = strstr(buf, "filename=\"");
					if (fn) {
						fn += 10;
						char *end = strchr(fn, '"');
						if (end) {
							int len = end - fn;
							if (len > (int)sizeof(filename) - 1)
								len = sizeof(filename) - 1;
							memcpy(filename, fn, len);
							filename[len] = '\0';
						}
					}

					if (filename[0] == '\0') {
						httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
						return ESP_FAIL;
					}

					char path[300];
					snprintf(path, sizeof(path), STORAGE_BASE "/%s", filename);
					f = fopen(path, "wb");
					if (!f) {
						httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
						return ESP_FAIL;
					}
					ESP_LOGI(TAG, "Uploading: %s (%d bytes)", filename, total);

					header_parsed = true;
					body_start = i + 4;

					int data_len = ret - body_start;
					if (data_len > 0) {
						fwrite(buf + body_start, 1, data_len, f);
					}
					break;
				}
			}
		} else {
			fwrite(buf, 1, ret, f);
		}

		received += ret;
		taskYIELD();
	}

	if (f) {
		fclose(f);

		if (boundary_len && filename[0]) {
			char path[300];
			snprintf(path, sizeof(path), STORAGE_BASE "/%s", filename);
			struct stat st;
			if (stat(path, &st) == 0) {
				long trim = boundary_len + 8;
				if (st.st_size > trim) {
					truncate(path, st.st_size - trim);
				}
			}
		}

		ESP_LOGI(TAG, "Upload complete: %s", filename);
		httpd_resp_sendstr(req, "OK");
	} else {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No file data");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static bool valid_filename(const char *name)
{
	return name[0] != '\0' &&
	       !strchr(name, '/') && !strchr(name, '\\') && !strstr(name, "..");
}

static esp_err_t delete_post_handler(httpd_req_t *req)
{
	char query[128];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
		return ESP_FAIL;
	}

	char filename[64];
	if (httpd_query_key_value(query, "file", filename, sizeof(filename)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file param");
		return ESP_FAIL;
	}

	if (!valid_filename(filename)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_FAIL;
	}

	char path[300];
	snprintf(path, sizeof(path), STORAGE_BASE "/%s", filename);

	if (remove(path) == 0) {
		ESP_LOGI(TAG, "Deleted: %s", filename);
		httpd_resp_sendstr(req, "OK");
	} else {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t rename_post_handler(httpd_req_t *req)
{
	char query[256];
	if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
		return ESP_FAIL;
	}

	char from[64], to[64];
	if (httpd_query_key_value(query, "from", from, sizeof(from)) != ESP_OK ||
	    httpd_query_key_value(query, "to", to, sizeof(to)) != ESP_OK) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing from/to params");
		return ESP_FAIL;
	}

	if (!valid_filename(from) || !valid_filename(to)) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_FAIL;
	}

	char path_from[300], path_to[300];
	snprintf(path_from, sizeof(path_from), STORAGE_BASE "/%s", from);
	snprintf(path_to, sizeof(path_to), STORAGE_BASE "/%s", to);

	if (rename(path_from, path_to) == 0) {
		ESP_LOGI(TAG, "Renamed: %s -> %s", from, to);
		httpd_resp_sendstr(req, "OK");
	} else {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
		return ESP_FAIL;
	}

	return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
	char ssid[33];
	settings_get_wifi(ssid, sizeof(ssid), NULL, 0);

	char buf[512];
	int len = snprintf(buf, sizeof(buf),
		"{\"brightness\":%d,\"volume\":%d,"
		"\"alarm_hour\":%d,\"alarm_minute\":%d,"
		"\"alarm_enabled\":%s,\"alarm_file\":\"%s\","
		"\"display_flip\":%s,\"encoder_invert\":%s,"
		"\"tz_offset\":%d,"
		"\"wifi_ssid\":\"%s\",\"wifi_connected\":%s,"
		"\"time_set\":%s,\"time\":%lld}",
		settings_get_brightness(),
		settings_get_volume(),
		settings_get_alarm_hour(),
		settings_get_alarm_minute(),
		settings_get_alarm_enabled() ? "true" : "false",
		settings_get_alarm_file(),
		settings_get_display_flip() ? "true" : "false",
		settings_get_encoder_invert() ? "true" : "false",
		settings_get_tz_offset(),
		ssid,
		sta_connected ? "true" : "false",
		time_is_set() ? "true" : "false",
		(long long)time_get_epoch());

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_send(req, buf, len);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
	char body[512];
	int len = httpd_req_recv(req, body, sizeof(body) - 1);
	if (len <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
		return ESP_FAIL;
	}
	body[len] = '\0';

	char val[64];

	if (parse_form_value(body, "brightness", val, sizeof(val)))
		settings_set_brightness(atoi(val));

	if (parse_form_value(body, "volume", val, sizeof(val)))
		settings_set_volume(atoi(val));

	if (parse_form_value(body, "alarm_hour", val, sizeof(val))) {
		char val2[8];
		if (parse_form_value(body, "alarm_minute", val2, sizeof(val2)))
			settings_set_alarm_time(atoi(val), atoi(val2));
	}

	if (parse_form_value(body, "alarm_enabled", val, sizeof(val))) {
		settings_set_alarm_enabled(strcmp(val, "1") == 0 || strcmp(val, "true") == 0);
		alarm_reschedule();
	}

	if (parse_form_value(body, "alarm_file", val, sizeof(val)))
		settings_set_alarm_file(val);

	if (parse_form_value(body, "tz_offset", val, sizeof(val)))
		settings_set_tz_offset(atoi(val));

	if (parse_form_value(body, "display_flip", val, sizeof(val)))
		settings_set_display_flip(strcmp(val, "1") == 0 || strcmp(val, "true") == 0);

	if (parse_form_value(body, "encoder_invert", val, sizeof(val)))
		settings_set_encoder_invert(strcmp(val, "1") == 0 || strcmp(val, "true") == 0);

	httpd_resp_sendstr(req, "OK");
	return ESP_OK;
}

static esp_err_t time_post_handler(httpd_req_t *req)
{
	char body[64];
	int len = httpd_req_recv(req, body, sizeof(body) - 1);
	if (len <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
		return ESP_FAIL;
	}
	body[len] = '\0';

	char val[20];

	// Set timezone first (from browser's getTimezoneOffset), persist to NVS
	if (parse_form_value(body, "tz", val, sizeof(val))) {
		settings_set_tz_offset(atoi(val));
	}

	if (parse_form_value(body, "epoch", val, sizeof(val))) {
		int64_t epoch = strtoll(val, NULL, 10);
		if (epoch > 1700000000) {
			time_set_epoch(epoch);
			httpd_resp_sendstr(req, "OK");
			return ESP_OK;
		}
	}

	httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid epoch");
	return ESP_FAIL;
}

// --- WiFi STA ---

static void wifi_sta_event_handler(void *arg, esp_event_base_t base,
                                    int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		sta_connected = false;
		ESP_LOGI(TAG, "STA disconnected");
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		sta_connected = true;
		ESP_LOGI(TAG, "STA connected, starting SNTP");
		esp_sntp_stop();
		esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
		esp_sntp_setservername(0, "pool.ntp.org");
		esp_sntp_init();
	}
}

static void do_sta_connect(const char *ssid, const char *pass)
{
	if (!ssid[0]) return;

	wifi_config_t sta_cfg = { 0 };
	strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
	strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);

	esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
	esp_wifi_connect();
	ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
	char body[256];
	int len = httpd_req_recv(req, body, sizeof(body) - 1);
	if (len <= 0) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
		return ESP_FAIL;
	}
	body[len] = '\0';

	char ssid[33] = "", pass[64] = "";
	parse_form_value(body, "ssid", ssid, sizeof(ssid));
	parse_form_value(body, "pass", pass, sizeof(pass));

	if (!ssid[0]) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
		return ESP_FAIL;
	}

	settings_set_wifi(ssid, pass);
	do_sta_connect(ssid, pass);

	httpd_resp_sendstr(req, "OK");
	return ESP_OK;
}

static esp_err_t alarm_test_post_handler(httpd_req_t *req)
{
	char path[80];
	snprintf(path, sizeof(path), "/storage/%s", settings_get_alarm_file());
	audio_play(path);
	httpd_resp_sendstr(req, "OK");
	return ESP_OK;
}

static esp_err_t alarm_stop_post_handler(httpd_req_t *req)
{
	alarm_dismiss();
	audio_stop();
	httpd_resp_sendstr(req, "OK");
	return ESP_OK;
}

// --- Server setup ---

static void start_httpd(void)
{
	if (server) return;

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.max_uri_handlers = 16;
	config.uri_match_fn = httpd_uri_match_wildcard;
	config.stack_size = 8192;

	if (httpd_start(&server, &config) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to start HTTP server");
		return;
	}

	httpd_uri_t handlers[] = {
		{ "/",            HTTP_GET,  root_get_handler, NULL },
		{ "/files",       HTTP_GET,  files_get_handler, NULL },
		{ "/upload",      HTTP_POST, upload_post_handler, NULL },
		{ "/delete",      HTTP_POST, delete_post_handler, NULL },
		{ "/rename",      HTTP_POST, rename_post_handler, NULL },
		{ "/settings",    HTTP_GET,  settings_get_handler, NULL },
		{ "/settings",    HTTP_POST, settings_post_handler, NULL },
		{ "/time",        HTTP_POST, time_post_handler, NULL },
		{ "/wifi",        HTTP_POST, wifi_post_handler, NULL },
		{ "/alarm/test",  HTTP_POST, alarm_test_post_handler, NULL },
		{ "/alarm/stop",  HTTP_POST, alarm_stop_post_handler, NULL },
	};

	for (int i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
		httpd_register_uri_handler(server, &handlers[i]);
	}

	ESP_LOGI(TAG, "HTTP server started");
}

// --- WiFi AP ---

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
		ESP_LOGI(TAG, "Client connected");
	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		ESP_LOGI(TAG, "Client disconnected");
	}
}

static void wifi_init(void)
{
	esp_netif_init();
	esp_event_loop_create_default();
	ap_netif = esp_netif_create_default_wifi_ap();
	sta_netif = esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_wifi_init(&cfg);

	esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
	                                     &wifi_event_handler, NULL, NULL);
	esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
	                                     &wifi_sta_event_handler, NULL, NULL);
	esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
	                                     &wifi_sta_event_handler, NULL, NULL);

	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_start();

	ESP_LOGI(TAG, "WiFi started in STA mode (AP idle)");
}

void webserver_set_ap_enabled(bool enabled)
{
	if (enabled == ap_enabled) return;
	ap_enabled = enabled;

	if (enabled) {
		// Switching STA->APSTA leaves STA connection intact
		esp_wifi_set_mode(WIFI_MODE_APSTA);
		wifi_config_t ap_cfg = {
			.ap = {
				.ssid = AP_SSID,
				.ssid_len = strlen(AP_SSID),
				.channel = AP_CHANNEL,
				.password = AP_PASS,
				.max_connection = AP_MAX_CONN,
				.authmode = WIFI_AUTH_WPA2_PSK,
			},
		};
		esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
		simple_dns_server_start("192.168.4.1");
		ESP_LOGI(TAG, "SoftAP enabled. SSID: %s, Password: %s", AP_SSID, AP_PASS);
	} else {
		simple_dns_server_stop();
		// APSTA->STA stops only the AP; STA connection survives
		esp_wifi_set_mode(WIFI_MODE_STA);
		ESP_LOGI(TAG, "SoftAP disabled");
	}
}

bool webserver_is_ap_enabled(void)
{
	return ap_enabled;
}

const char *webserver_get_ap_ssid(void)
{
	return AP_SSID;
}

esp_err_t webserver_init(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs_flash_init();
	}

	wifi_init();
	start_httpd();

	return ESP_OK;
}

void webserver_try_sta_connect(void)
{
	char ssid[33], pass[64];
	settings_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
	if (ssid[0]) {
		do_sta_connect(ssid, pass);
	}
}

bool webserver_is_sta_connected(void)
{
	return sta_connected;
}

void webserver_get_sta_ip(char *buf, size_t len)
{
	if (!sta_connected || !sta_netif) {
		snprintf(buf, len, "no wifi");
		return;
	}
	esp_netif_ip_info_t ip_info;
	if (esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
		snprintf(buf, len, "%d.%d.%d.%d",
			(int)((ip_info.ip.addr >>  0) & 0xFF),
			(int)((ip_info.ip.addr >>  8) & 0xFF),
			(int)((ip_info.ip.addr >> 16) & 0xFF),
			(int)((ip_info.ip.addr >> 24) & 0xFF));
	} else {
		snprintf(buf, len, "no wifi");
	}
}
