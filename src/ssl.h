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

#ifndef _LIBSSL_H
#define _LIBSSL_H

#include "config.h"

#ifdef ENABLE_SSL

#include <stdbool.h>
#include "liblist.h"
#include "polarssl/ssl.h"
#include "polarssl/x509.h"
#include "polarssl/version.h"

typedef struct {
	ssl_context *context;
	int         *client_fd;
	rsa_context *private_key;
	x509_cert   *certificate;
	x509_cert   *ca_certificate;
	x509_crl    *ca_crl;
	int         timeout;
	int         min_ssl_version;
	int         dh_size;
} t_ssl_accept_data;

int  init_ssl_module(char *logfile);
#if POLARSSL_VERSION_NUMBER >= 0x01020000
int  ssl_register_sni(t_charlist *hostname, rsa_context *private_key, x509_cert *certificate,
                  x509_cert *ca_certificate, x509_crl *ca_crl);
#endif
int  ssl_load_key_cert(char *file, rsa_context **private_key, x509_cert **certificate);
int  ssl_load_ca_cert(char *file, x509_cert **ca_certificate);
int  ssl_load_ca_crl(char *file, x509_crl **ca_crl);
int  ssl_accept(t_ssl_accept_data *ssl_accept_data);
int  ssl_pending(ssl_context *ssl);
int  ssl_receive(ssl_context *ssl, char *buffer, unsigned int maxlength);
int  ssl_send(ssl_context *ssl, const char *buffer, unsigned int length);
bool ssl_has_peer_cert(ssl_context *context);
int  get_peer_cert_info(ssl_context *context, char *subject_dn, char *issuer_dn, char *serial_nr, int length);
char *ssl_version_string(ssl_context *context);
void ssl_close(ssl_context *ssl);
void ssl_shutdown(void);
#ifdef ENABLE_RPROXY
int  ssl_connect(ssl_context *ssl, int *sock, char *hostname);
int  ssl_send_completely(ssl_context *ssl, const char *buffer, int size);
#endif

#endif

#endif
