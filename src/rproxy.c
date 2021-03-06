/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License. For a copy,
 * see http://www.gnu.org/licenses/gpl-2.0.html.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "config.h"

#ifdef ENABLE_RPROXY

#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "global.h"
#include "rproxy.h"
#ifdef ENABLE_SSL
#include "ssl.h"
#endif
#include "libstr.h"
#include "libfs.h"
#include "polarssl/md5.h"

#define RPROXY_ID_LEN   10 /* Must be smaller than 32 */
#define MAX_SEND_BUFFER  2 * KILOBYTE
#define EXTENSION_SIZE  10

static char   *rproxy_header;
static size_t rproxy_header_len;
static char   *rproxy_id_key = "X-Hiawatha-RProxy-ID:";
static char   rproxy_id[33];

static pthread_mutex_t resolv_mutex;

extern char *hs_forwarded;

typedef struct type_send_buffer {
	char buffer[MAX_SEND_BUFFER];
	int bytes_in_buffer;
} t_send_buffer;

/* Initialize reverse proxy module
 */
int init_rproxy_module(void) {
	unsigned char digest[16];
	char str[50];
	time_t t;
	struct tm *s;
	char *format = "Connection: close\r\n%s %s\r\n";

	time(&t);
	s = localtime(&t);
	str[49] = '\0';
	strftime(str, 49, "%a %d %b %Y %T", s);

	md5((unsigned char*)str, strlen(str), digest);
	md5_bin2hex(digest, rproxy_id);
	rproxy_id[RPROXY_ID_LEN] = '\0';

	if ((rproxy_header = (char*)malloc(strlen(format) - 4 + strlen(rproxy_id_key) + RPROXY_ID_LEN + 1)) == NULL) {
		return -1;
	}
	sprintf(rproxy_header, format, rproxy_id_key, rproxy_id);
	rproxy_header_len = strlen(rproxy_header);

	if (pthread_mutex_init(&resolv_mutex, NULL) != 0) {
		return -1;
	}

	return 0;
}

/* Parse configuration line
 */
t_rproxy *rproxy_setting(char *line) {
	t_rproxy *rproxy;
	size_t len;
	char *path, *port, *timeout;
	int resolved;

	if (split_string(line, &path, &line, ' ') != 0) {
		return NULL;
	} else if ((rproxy = (t_rproxy*)malloc(sizeof(t_rproxy))) == NULL) {
		return NULL;
	}

	split_string(line, &line, &timeout, ' ');

	rproxy->next = NULL;
	rproxy->timeout = 5;

	/* Pattern
	 */
	if (regcomp(&(rproxy->pattern), path, REG_EXTENDED) != 0) {
		free(rproxy);
		return NULL;
	}

	/* Protocol
	 */
	if (strncmp(line, "http://", 7) == 0) {
		line += 7;
#ifdef ENABLE_SSL
		rproxy->use_ssl = false;
	} else if (strncmp(line, "https://", 8) == 0) {
		line += 8;
		rproxy->use_ssl = true;
#endif
	} else {
		free(rproxy);
		return NULL;
	}

	/* Path
	 */
	rproxy->path = NULL;
	rproxy->path_len = 0;
	if ((path = strchr(line, '/')) != NULL) {
		if ((len = strlen(path)) > 1) {
			if (*(path + len - 1) == '/') {
				*(path + len - 1) = '\0';
			}
			if ((rproxy->path = strdup(path)) == NULL) {
				free(rproxy);
				return NULL;
			}
			rproxy->path_len = strlen(rproxy->path);
		}
		*path = '\0';
	}

	/* Port
	 */
#ifdef ENABLE_IPV6
	if (*line == '[') {
		line++;
		if ((port = strchr(line, ']')) == NULL) {
			check_free(rproxy->path);
			free(rproxy);
			return NULL;
		}
		*(port++) = '\0';
		if (*port == '\0') {
			port = NULL;
		} else if (*port != ':') {
			check_free(rproxy->path);
			free(rproxy);
			return NULL;
		}
	} else
#endif
		port = strchr(line, ':');

	if (port != NULL) {
		*(port++) = '\0';
		if ((rproxy->port = str2int(port)) < 1) {
			check_free(rproxy->path);
			free(rproxy);
			return NULL;
		} else if (rproxy->port > 65535) {
			check_free(rproxy->path);
			free(rproxy);
			return NULL;
		}
	} else {
#ifdef ENABLE_SSL
		if (rproxy->use_ssl) {
			rproxy->port = 443;
		} else
#endif
			rproxy->port = 80;
	}

	/* Hostname
	 */
	if (parse_ip(line, &(rproxy->ip_addr)) == -1) {
		if ((rproxy->hostname = strdup(line)) == NULL) {
			check_free(rproxy->path);
			free(rproxy);
			return NULL;
		}
		rproxy->hostname_len = strlen(rproxy->hostname);

		pthread_mutex_lock(&resolv_mutex);
		resolved = hostname_to_ip(line, &(rproxy->ip_addr));
		pthread_mutex_unlock(&resolv_mutex);

		if (resolved == -1) {
			fprintf(stderr, "Can't resolve hostname '%s'\n", line);
			check_free(rproxy->path);
			check_free(rproxy->hostname);
			free(rproxy);
			return NULL;
		}
	} else {
		rproxy->hostname = NULL;
		rproxy->hostname_len = -1;
	}

	/* Timeout
	 */
	if (timeout != NULL) {
		if ((rproxy->timeout = str2int(timeout)) <= 0) {
			check_free(rproxy->path);
			check_free(rproxy->hostname);
			free(rproxy);
			return NULL;
		}
	}

	return rproxy;
}

/* Does URL match with proxy match pattern?
 */
bool rproxy_match(t_rproxy *rproxy, char *uri) {
	if ((rproxy == NULL) || (uri == NULL)) {
		return false;
	}

	return regexec(&(rproxy->pattern), uri, 0, NULL, 0) != REG_NOMATCH;
}

/* Detect reverse proxy loop
 */
bool rproxy_loop_detected(t_http_header *http_headers) {
	char *value;

	if ((value = get_http_header(rproxy_id_key, http_headers)) == NULL) {
		return false;
	}

	if (strcmp(value, rproxy_id) != 0) {
		return false;
	}

	return true;
}

/* Init reverse proxy options record
 */
void init_rproxy_options(t_rproxy_options *options, int socket, t_ip_addr *client_ip,
	                     char *method, char *uri, t_http_header *http_headers,
	                     char *body, int body_length, char *remote_user) {
	options->client_socket = socket;
	options->client_ip = client_ip;
	options->method = method;
	options->uri = uri;
	options->http_headers = http_headers;
	options->body = body;
	options->body_length = body_length;
	options->remote_user = remote_user;
}

/* Init reverse proxy result record
 */
void init_rproxy_result(t_rproxy_result *result) {
	result->bytes_sent = 0;
}

/* Connect to the webserver
 */
int connect_to_webserver(t_rproxy *rproxy) {
	int sock = -1;
	struct sockaddr_in saddr4;
#ifdef ENABLE_IPV6
	struct sockaddr_in6 saddr6;
#endif

	if (rproxy == NULL) {
		return -1;
	}

	if (rproxy->ip_addr.family == AF_INET) {
		/* IPv4
		 */
		if ((sock = socket(AF_INET, SOCK_STREAM, 0)) > 0) {
			memset(&saddr4, 0, sizeof(struct sockaddr_in));
			saddr4.sin_family = AF_INET;
			saddr4.sin_port = htons(rproxy->port);
			memcpy(&saddr4.sin_addr.s_addr, &(rproxy->ip_addr.value), rproxy->ip_addr.size);
			if (connect(sock, (struct sockaddr*)&saddr4, sizeof(struct sockaddr_in)) != 0) {
				close(sock);
				sock = -1;
			}
		}
#ifdef ENABLE_IPV6
	} else if (rproxy->ip_addr.family == AF_INET6) {
		/* IPv6
		 */
		if ((sock = socket(AF_INET6, SOCK_STREAM, 0)) > 0) {
			memset(&saddr6, 0, sizeof(struct sockaddr_in6));
			saddr6.sin6_family = AF_INET6;
			saddr6.sin6_port = htons(rproxy->port);
			memcpy(&saddr6.sin6_addr.s6_addr, &(rproxy->ip_addr.value), rproxy->ip_addr.size);
			if (connect(sock, (struct sockaddr*)&saddr6, sizeof(struct sockaddr_in6)) != 0) {
				close(sock);
				sock = -1;
			}
		}
#endif
	}

	return sock;
}

/* Send output buffer to webserver
 */
static int send_buffer_to_webserver(t_rproxy_webserver *webserver, const char *buffer, int size) {
#ifdef ENABLE_SSL
	if (webserver->use_ssl) {
		return ssl_send_completely(&(webserver->ssl), buffer, size);
	} else
#endif
		return write_buffer(webserver->socket, buffer, size);
}

/* Send buffer to webserver
 */
static int send_to_webserver(t_rproxy_webserver *webserver, t_rproxy_result *result, t_send_buffer *send_buffer, const char *buffer, int size) {
	if (buffer == NULL) {
		if (send_buffer->bytes_in_buffer > 0) {
			if (send_buffer_to_webserver(webserver, send_buffer->buffer, send_buffer->bytes_in_buffer) == -1) {
				return -1;
			}
		}
	} else if (size > MAX_SEND_BUFFER) {
		if (send_buffer_to_webserver(webserver, send_buffer->buffer, send_buffer->bytes_in_buffer) == -1) {
			return -1;
		}
		send_buffer->bytes_in_buffer = 0;

		if (send_buffer_to_webserver(webserver, buffer, size) == -1) {
			return -1;
		}
	} else if (send_buffer->bytes_in_buffer + size > MAX_SEND_BUFFER) {
		if (send_buffer_to_webserver(webserver, send_buffer->buffer, send_buffer->bytes_in_buffer) == -1) {
			return -1;
		}
		memcpy(send_buffer->buffer, buffer, size);
		send_buffer->bytes_in_buffer = size;
	} else {
		memcpy(send_buffer->buffer + send_buffer->bytes_in_buffer, buffer, size);
		send_buffer->bytes_in_buffer += size;
	}

	result->bytes_sent += size;

	return 0;
}

/* Send the request to the webserver
 */
int send_request_to_webserver(t_rproxy_webserver *webserver, t_rproxy_options *options, t_rproxy *rproxy, t_rproxy_result *result) {
	t_http_header *http_header;
	char forwarded_for[20 + MAX_IP_STR_LEN], ip_addr[MAX_IP_STR_LEN];
	bool forwarded_found = false;
	t_send_buffer send_buffer;
#ifdef ENABLE_CACHE
	char extension[EXTENSION_SIZE];
#endif

	send_buffer.bytes_in_buffer = 0;

	if (ip_to_str(ip_addr, options->client_ip, MAX_IP_STR_LEN) == -1) {
		return -1;
	}

	/* Send first line
	 */
	if (send_to_webserver(webserver, result, &send_buffer, options->method, strlen(options->method)) == -1) {
		return -1;
	} else if (send_to_webserver(webserver, result, &send_buffer, " ", 1) == -1) {
		return -1;
	}

	if (rproxy->path != NULL) {
		if (send_to_webserver(webserver, result, &send_buffer, rproxy->path, rproxy->path_len) == -1) {
			return -1;
		}
	}

	if (send_to_webserver(webserver, result, &send_buffer, options->uri, strlen(options->uri)) == -1) {
		return -1;
	} else if (send_to_webserver(webserver, result, &send_buffer, " HTTP/1.1\r\n", 11) == -1) {
		return -1;
	}

	/* Send HTTP headers
	 */
	if (rproxy->hostname != NULL) {
		if (send_to_webserver(webserver, result, &send_buffer, "Host: ", 6) == -1) {
			return -1;
		} else if (send_to_webserver(webserver, result, &send_buffer, rproxy->hostname, rproxy->hostname_len) == -1) {
			return -1;
		} else if (send_to_webserver(webserver, result, &send_buffer, "\r\n", 2) == -1) {
			return -1;
		}
	}

	if (send_to_webserver(webserver, result, &send_buffer, rproxy_header, rproxy_header_len) == -1) {
		return -1;
	}

	for (http_header = options->http_headers; http_header != NULL; http_header = http_header->next) {
		if (rproxy->hostname != NULL) {
			if (strncasecmp(http_header->data, "Host:", 5) == 0) {
				continue;
			}
		}

		if (strncasecmp(http_header->data, "Connection:", 11) == 0) {
			continue;
		}

#ifdef ENABLE_CACHE
		if (strncasecmp(http_header->data, "If-Modified-Since:", 18) == 0) {
			if (extension_from_uri(options->uri, extension, EXTENSION_SIZE)) {
				if (in_charlist(extension, options->cache_extensions)) {
					continue;
				}
			}
		}
#endif

		if (strncasecmp(http_header->data, "X-Forwarded-User:", 17) == 0) {
			continue;
		}


		if (send_to_webserver(webserver, result, &send_buffer, http_header->data, http_header->length) == -1) {
			return -1;
		}

		if (strncasecmp(http_header->data, hs_forwarded, 16) == 0) {
			/* Add IP to X-Forwarded-For header
			 */
			if (sprintf(forwarded_for, ", %s\r\n", ip_addr) == -1) {
				return -1;
			} else if (send_to_webserver(webserver, result, &send_buffer, forwarded_for, strlen(forwarded_for)) == -1) {
				return -1;
			}

			forwarded_found = true;
		} else if (send_to_webserver(webserver, result, &send_buffer, "\r\n", 2) == -1) {
			return -1;
		}
	}

	/* Send X-Forwarded-For
	 */
	if (forwarded_found == false) {
		if (sprintf(forwarded_for, "%s %s\r\n", hs_forwarded, ip_addr) == -1) {
			return -1;
		} else if (send_to_webserver(webserver, result, &send_buffer, forwarded_for, strlen(forwarded_for)) == -1) {
			return -1;
		}
	}

	/* Send X-Forwarded-User
	 */
	if (options->remote_user != NULL) {
		if (send_to_webserver(webserver, result, &send_buffer, "X-Forwarded-User: ", 18) == -1) {
			return -1;
		} else if (send_to_webserver(webserver, result, &send_buffer, options->remote_user, strlen(options->remote_user)) == -1) {
			return -1;
		} else if (send_to_webserver(webserver, result, &send_buffer, "\r\n", 2) == -1) {
			return -1;
		}
	}

	/* Close header
	 */
	if (send_to_webserver(webserver, result, &send_buffer, "\r\n", 2) == -1) {
		return -1;
	}

	/* Send body
	 */
	if (options->body != NULL) {
		if (send_to_webserver(webserver, result, &send_buffer, options->body, options->body_length) == -1) {
			return -1;
		}
	}

	if (send_to_webserver(webserver, result, &send_buffer, NULL, 0) == -1) {
		return -1;
	}

	return 0;
}

#endif
