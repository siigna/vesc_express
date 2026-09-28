#include "comm_http.h"
#include "log.h"
#include "main.h"
#include "commands.h"
#include "terminal.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>

static const char *TAG = "comm_http";
static httpd_handle_t m_server = NULL;

/* The viewer is embedded pre-compressed: it is ~122 kB of HTML plus an inlined
 * charting library, which does not fit the app partition uncompressed but
 * gzips to ~42 kB. Browsers inflate it transparently via Content-Encoding. */
extern const char vesclog_gz_start[] asm("_binary_vesclog_html_gz_start");
extern const char vesclog_gz_end[]   asm("_binary_vesclog_html_gz_end");

static void url_decode(const char *src, char *dst, size_t dst_len) {
	size_t i = 0, j = 0;
	while (src[i] && j < dst_len - 1) {
		if (src[i] == '%' && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
			char hex[3] = {src[i+1], src[i+2], '\0'};
			dst[j++] = (char)strtol(hex, NULL, 16);
			i += 3;
		} else if (src[i] == '+') {
			dst[j++] = ' ';
			i++;
		} else {
			dst[j++] = src[i++];
		}
	}
	dst[j] = '\0';
}

/* Stream a file from the card. Returns false if it is not there, in which case
 * nothing has been sent yet and the caller can fall through to another source. */
static bool send_sd_file(httpd_req_t *req, const char *name, bool gzipped) {
	char path[80];
	snprintf(path, sizeof(path), "%s%s", file_basepath, name);

	FILE *f = fopen(path, "r");
	if (!f) return false;

	if (gzipped) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

	static char buf[2048];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		httpd_resp_send_chunk(req, buf, (ssize_t)n);
	fclose(f);
	httpd_resp_send_chunk(req, NULL, 0);
	return true;
}

static void send_embedded(httpd_req_t *req) {
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	httpd_resp_send(req, vesclog_gz_start,
	                (ssize_t)(vesclog_gz_end - vesclog_gz_start));
}

static esp_err_t root_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	/* A copy on the card always wins, so the viewer can be replaced in the
	 * field without reflashing. Compressed first, then plain. */
	if (send_sd_file(req, "vesclog.html.gz", true))  return ESP_OK;
	if (send_sd_file(req, "vesclog.html",    false)) return ESP_OK;

	/* Nothing there yet — seed the card so /update has something to replace */
	char path[80];
	snprintf(path, sizeof(path), "%svesclog.html.gz", file_basepath);
	FILE *wf = fopen(path, "w");
	if (wf) {
		fwrite(vesclog_gz_start, 1,
		       (size_t)(vesclog_gz_end - vesclog_gz_start), wf);
		fclose(wf);
	}
	send_embedded(req);
	return ESP_OK;
}

static esp_err_t embedded_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	send_embedded(req);
	return ESP_OK;
}

static esp_err_t export_handler(httpd_req_t *req) {
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"vesclog.html\"");

	/* Content-Encoding makes the browser inflate on the way in, so the user
	 * gets a usable .html no matter how the viewer happens to be stored. */
	if (send_sd_file(req, "vesclog.html.gz", true))  return ESP_OK;
	if (send_sd_file(req, "vesclog.html",    false)) return ESP_OK;
	send_embedded(req);
	return ESP_OK;
}

static esp_err_t update_handler(httpd_req_t *req) {
	if (req->content_len == 0 || req->content_len > 1024 * 1024) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
		return ESP_OK;
	}

	char buf[512];
	int remaining = (int)req->content_len;

	/* Read the first chunk up front so the gzip magic tells us which name to
	 * store it under — the uploader may hand us either form. */
	int n = httpd_req_recv(req, buf, MIN((int)sizeof(buf), remaining));
	if (n <= 0) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
		return ESP_OK;
	}
	bool is_gz = (n >= 2 && (unsigned char)buf[0] == 0x1f
	                     && (unsigned char)buf[1] == 0x8b);

	char keep[80], drop[80];
	snprintf(keep, sizeof(keep), "%svesclog.html%s", file_basepath, is_gz ? ".gz" : "");
	snprintf(drop, sizeof(drop), "%svesclog.html%s", file_basepath, is_gz ? "" : ".gz");

	FILE *f = fopen(keep, "w");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write to SD card");
		return ESP_OK;
	}
	fwrite(buf, 1, (size_t)n, f);
	remaining -= n;

	while (remaining > 0) {
		n = httpd_req_recv(req, buf, MIN((int)sizeof(buf), remaining));
		if (n <= 0) {
			fclose(f);
			httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
			return ESP_OK;
		}
		fwrite(buf, 1, (size_t)n, f);
		remaining -= n;
	}
	fclose(f);
	unlink(drop);   /* exactly one copy, so precedence stays unambiguous */

	char resp[96];
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"gzip\":%s}", is_gz ? "true" : "false");
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

static esp_err_t api_logs_handler(httpd_req_t *req) {
	char dir_path[80];
	snprintf(dir_path, sizeof(dir_path), "%slog_can", file_basepath);

	DIR *dir = opendir(dir_path);
	if (!dir) {
		httpd_resp_set_type(req, "application/json");
		httpd_resp_sendstr(req, "[]");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
	httpd_resp_sendstr_chunk(req, "[");

	struct dirent *entry;
	bool first = true;
	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_type != DT_REG) continue;
		size_t nlen = strlen(entry->d_name);
		if (nlen < 4 || strcasecmp(entry->d_name + nlen - 4, ".csv") != 0) continue;

		char full_path[192];
		snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
		struct stat st;
		long size = stat(full_path, &st) == 0 ? (long)st.st_size : -1;

		/* Escape double quotes in filename (shouldn't occur but be safe) */
		char safe_name[128];
		size_t si = 0, di = 0;
		while (entry->d_name[si] && di < sizeof(safe_name) - 2) {
			if (entry->d_name[si] == '"') safe_name[di++] = '\\';
			safe_name[di++] = entry->d_name[si++];
		}
		safe_name[di] = '\0';

		char chunk[192];
		snprintf(chunk, sizeof(chunk),
			"%s{\"name\":\"%s\",\"size\":%ld}",
			first ? "" : ",", safe_name, size);
		httpd_resp_sendstr_chunk(req, chunk);
		first = false;
	}
	closedir(dir);

	httpd_resp_sendstr_chunk(req, "]");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

static esp_err_t api_log_file_handler(httpd_req_t *req) {
	/* URI is /api/logs/<encoded-filename> */
	const char *encoded = req->uri + strlen("/api/logs/");
	if (!*encoded) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
		return ESP_OK;
	}

	char filename[128];
	url_decode(encoded, filename, sizeof(filename));

	/* Prevent path traversal */
	if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_OK;
	}

	char path[256];
	snprintf(path, sizeof(path), "%slog_can/%s", file_basepath, filename);

	FILE *f = fopen(path, "r");
	if (!f) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
		return ESP_OK;
	}

	httpd_resp_set_type(req, "text/csv; charset=utf-8");

	char disp[160];
	snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
	httpd_resp_set_hdr(req, "Content-Disposition", disp);
	httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

	static char buf[4096];
	size_t n;
	esp_err_t ret = ESP_OK;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
			ret = ESP_FAIL;
			break;
		}
	}
	fclose(f);

	if (ret == ESP_OK) {
		httpd_resp_send_chunk(req, NULL, 0);
	}
	return ret;
}

static esp_err_t api_delete_handler(httpd_req_t *req) {
	const char *encoded = req->uri + strlen("/api/delete/");
	if (!*encoded) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename"); return ESP_OK; }
	char filename[128];
	url_decode(encoded, filename, sizeof(filename));
	if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_OK;
	}
	char path[256];
	snprintf(path, sizeof(path), "%slog_can/%s", file_basepath, filename);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, unlink(path) == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
	return ESP_OK;
}

static esp_err_t api_trim_handler(httpd_req_t *req) {
	const char *encoded = req->uri + strlen("/api/trim/");
	if (!*encoded) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename"); return ESP_OK; }
	char filename[128];
	url_decode(encoded, filename, sizeof(filename));
	if (strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
		httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
		return ESP_OK;
	}
	char src_path[256], tmp_path[260];
	snprintf(src_path, sizeof(src_path), "%slog_can/%s", file_basepath, filename);
	snprintf(tmp_path, sizeof(tmp_path), "%slog_can/.trimtmp", file_basepath);

	FILE *src = fopen(src_path, "r");
	if (!src) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found"); return ESP_OK; }
	FILE *dst = fopen(tmp_path, "w");
	if (!dst) { fclose(src); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No space"); return ESP_OK; }

	static char line[1024];
	int spd_col = -1, before = 0, after = 0;

	/* Row 0: header — find kmh_vesc or gnss_h_vel column index */
	if (fgets(line, sizeof(line), src)) {
		fputs(line, dst);
		char *p = line; int col = 0;
		while (*p) {
			int klen = 0;
			char *c = p;
			while (*c && *c != ':' && *c != ';') { klen++; c++; }
			if (*c == ':') {
				if ((klen == 8  && strncmp(p, "kmh_vesc",   8)  == 0) ||
				    (klen == 10 && strncmp(p, "gnss_h_vel", 10) == 0)) {
					spd_col = col; break;
				}
			}
			char *s = strchr(p, ';'); if (!s) break;
			p = s + 1; col++;
		}
	}
	/* Row 1: startup partial record — keep as-is */
	if (fgets(line, sizeof(line), src)) fputs(line, dst);

	/* Remaining rows: keep only if speed >= 0.5 km/h */
	while (fgets(line, sizeof(line), src)) {
		before++;
		bool keep = (spd_col < 0);
		if (spd_col >= 0) {
			char *p = line;
			for (int c = 0; c < spd_col; c++) { char *s = strchr(p, ';'); if (!s) break; p = s + 1; }
			float v = strtof(p, NULL);
			keep = (fabsf(v) >= backup.config.vesclog_trim_speed_x10 / 10.0f);
		}
		if (keep) { fputs(line, dst); after++; }
	}
	fclose(src); fclose(dst);
	unlink(src_path);
	rename(tmp_path, src_path);

	char resp[128];
	snprintf(resp, sizeof(resp), "{\"ok\":true,\"before\":%d,\"after\":%d}", before, after);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_sendstr(req, resp);
	return ESP_OK;
}

void comm_http_start(void) {
	if (!backup.config.vesclog_http_en) return;
	if (m_server != NULL) {
		STORED_LOGF("comm_http_start: already running");
		return;
	}

	STORED_LOGF("comm_http_start: free heap %lu", (unsigned long)esp_get_free_heap_size());

	httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
	cfg.server_port      = backup.config.vesclog_http_port;
	cfg.uri_match_fn     = httpd_uri_match_wildcard;
	cfg.max_uri_handlers = 9;

	esp_err_t err = httpd_start(&m_server, &cfg);
	if (err != ESP_OK) {
		STORED_LOGF("comm_http_start: httpd_start failed: %d", err);
		m_server = NULL;
		return;
	}

	STORED_LOGF("comm_http_start: HTTP server up on port 80");

	static const httpd_uri_t root_uri = {
		.uri = "/", .method = HTTP_GET, .handler = root_handler,
	};
	static const httpd_uri_t embedded_uri = {
		.uri = "/embedded", .method = HTTP_GET, .handler = embedded_handler,
	};
	static const httpd_uri_t export_uri = {
		.uri = "/export", .method = HTTP_GET, .handler = export_handler,
	};
	static const httpd_uri_t update_uri = {
		.uri = "/update", .method = HTTP_POST, .handler = update_handler,
	};
	static const httpd_uri_t api_logs_uri = {
		.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler,
	};
	static const httpd_uri_t api_log_file_uri = {
		.uri = "/api/logs/*", .method = HTTP_GET, .handler = api_log_file_handler,
	};

	static const httpd_uri_t api_delete_uri = {
		.uri = "/api/delete/*", .method = HTTP_POST, .handler = api_delete_handler,
	};
	static const httpd_uri_t api_trim_uri = {
		.uri = "/api/trim/*", .method = HTTP_POST, .handler = api_trim_handler,
	};

	httpd_register_uri_handler(m_server, &root_uri);
	httpd_register_uri_handler(m_server, &embedded_uri);
	httpd_register_uri_handler(m_server, &export_uri);
	httpd_register_uri_handler(m_server, &update_uri);
	httpd_register_uri_handler(m_server, &api_logs_uri);
	httpd_register_uri_handler(m_server, &api_log_file_uri);
	httpd_register_uri_handler(m_server, &api_delete_uri);
	httpd_register_uri_handler(m_server, &api_trim_uri);

	ESP_LOGI(TAG, "HTTP server started on port 80");
}

void comm_http_stop(void) {
	if (m_server) {
		httpd_stop(m_server);
		m_server = NULL;
	}
}

static void terminal_http_status(int argc, const char **argv) {
	(void)argc; (void)argv;
	commands_printf("http server: %s", m_server ? "RUNNING" : "NOT running");
	commands_printf("free heap:   %lu bytes", (unsigned long)esp_get_free_heap_size());

	char dir_path[80];
	snprintf(dir_path, sizeof(dir_path), "%slog_can", file_basepath);
	commands_printf("log dir:     %s", dir_path);
	DIR *d = opendir(dir_path);
	if (!d) {
		commands_printf("opendir failed — no SD card / wrong path?");
	} else {
		int n = 0;
		struct dirent *e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_type == DT_REG) { commands_printf("  %s", e->d_name); n++; }
		}
		closedir(d);
		commands_printf("total files: %d", n);
	}

	if (!m_server) {
		commands_printf("attempting start...");
		httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
		cfg.server_port      = backup.config.vesclog_http_port;
		cfg.uri_match_fn     = httpd_uri_match_wildcard;
		cfg.max_uri_handlers = 4;
		esp_err_t err = httpd_start(&m_server, &cfg);
		if (err != ESP_OK) {
			commands_printf("httpd_start failed: 0x%x", (unsigned)err);
			m_server = NULL;
			return;
		}
		static const httpd_uri_t root_uri = {
			.uri = "/", .method = HTTP_GET, .handler = root_handler,
		};
		static const httpd_uri_t api_logs_uri = {
			.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler,
		};
		static const httpd_uri_t api_log_file_uri = {
			.uri = "/api/logs/*", .method = HTTP_GET, .handler = api_log_file_handler,
		};
		httpd_register_uri_handler(m_server, &root_uri);
		httpd_register_uri_handler(m_server, &api_logs_uri);
		httpd_register_uri_handler(m_server, &api_log_file_uri);
		commands_printf("started OK — try http://device-ip/");
	}
}

void comm_http_register_commands(void) {
	terminal_register_command_callback(
		"http_status",
		"Show HTTP server status and start it if not running",
		0,
		terminal_http_status);
}
