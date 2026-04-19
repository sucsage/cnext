#ifndef CNEXT_TLS_H
#define CNEXT_TLS_H

// =====================================================================
// tls.h — OpenSSL wrapper for cnext
//
// Runtime cert loading: drop cert.pem + key.pem into certs/ at the
// project root (override with env vars CNEXT_TLS_CERT / CNEXT_TLS_KEY).
// If no cert is found at startup, TLS is silently disabled — the
// plaintext listener keeps working unchanged.
//
// When TLS_ENABLED is 0 the whole module compiles to stubs so the
// library still links without libssl.
// =====================================================================

#include "config.h"

#if TLS_ENABLED
#include <openssl/ssl.h>
#endif

// Initialise the process-wide SSL_CTX. Safe to call multiple times;
// subsequent calls are no-ops. Never aborts — on failure tls_is_ready
// simply reports 0 and the caller should skip the TLS listener.
void tls_init(void);

// Returns 1 iff tls_init() successfully loaded a usable cert+key.
int  tls_is_ready(void);

#if TLS_ENABLED
// Wrap an accepted fd. Returns an SSL* owned by the caller, or NULL on
// failure (in which case the caller should close the fd).
SSL *tls_accept(int fd);

// Drive the handshake to completion on a blocking socket.
// Returns 0 on success, -1 on fatal error.
int  tls_handshake(SSL *ssl);

// Enable kTLS on both BIOs. Call after a successful handshake.
// Returns 0 if kTLS is active on send & recv, -1 otherwise (the caller
// can still use SSL_read/SSL_write as a fallback).
int  tls_enable_ktls(SSL *ssl);

// Inspect the negotiated ALPN protocol. Returns PROTO_H2, PROTO_H1,
// or PROTO_H1 as the default when the peer didn't advertise ALPN.
int  tls_alpn_proto(SSL *ssl);

// Free the SSL handle (caller is responsible for the underlying fd).
void tls_free(SSL *ssl);
#endif // TLS_ENABLED

#endif // CNEXT_TLS_H
