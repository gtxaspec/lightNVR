#ifndef LIGHTNVR_URL_UTILS_H
#define LIGHTNVR_URL_UTILS_H

#include <stddef.h>

/**
 * Apply username/password to a URL, replacing any embedded credentials.
 *
 * When @p username is NULL or empty the original URL is copied verbatim.
 * When @p password is NULL or empty the username is applied and any existing
 * password is cleared.
 */
int url_apply_credentials(const char *url, const char *username,
                          const char *password, char *out_url,
                          size_t out_size);

/**
 * Remove embedded credentials from a URL.
 */
int url_strip_credentials(const char *url, char *out_url, size_t out_size);

/**
 * Extract embedded credentials from a URL.
 * Missing username/password parts are returned as empty strings.
 */
int url_extract_credentials(const char *url, char *username,
                            size_t username_size, char *password,
                            size_t password_size);

/**
 * Build an ONVIF service URL from an existing stream/device URL.
 *
 * Maps transport schemes to their HTTP equivalent: rtsps → https, everything
 * else (rtsp, onvif, …) → http; existing http/https schemes are preserved.
 * Maps standard transport ports to ONVIF web ports when @p onvif_port is not
 * explicitly set: RTSP 554 → HTTP 80, RTSPS 322 → HTTPS 443.  All other
 * ports are passed through unchanged.  Credentials, query parameters, and
 * fragments are always stripped from the output.
 *
 * @param url          Source URL (rtsp, rtsps, http, https, onvif, …).
 * @param onvif_port   Explicit ONVIF port.  When > 0 it overrides any port
 *                     derived from @p url; when <= 0 the port is derived via
 *                     the mapping table above.
 * @param service_path ONVIF service path (e.g. "/onvif/device_service" or
 *                     "/onvif/ptz_service").  Must begin with '/'.
 *                     Pass NULL or an empty string to return only the
 *                     scheme + host + port (base URL without trailing slash).
 * @param out_url      Output buffer.
 * @param out_size     Size of the output buffer.
 * @return             0 on success, -1 on error.
 */
int url_build_onvif_service_url(const char *url, int onvif_port,
                                const char *service_path,
                                char *out_url, size_t out_size);

/**
 * Build an ONVIF device-service URL from an existing stream/device URL.
 *
 * Convenience wrapper around url_build_onvif_service_url() that uses
 * "/onvif/device_service" as the service path.
 *
 * If @p onvif_port is <= 0 the stripped input URL is returned unchanged
 * (backward-compatible behaviour for callers that pass a URL already
 * containing an ONVIF endpoint).  Otherwise the host is preserved,
 * credentials are removed, the scheme is mapped to http/https, and the
 * path becomes /onvif/device_service.
 */
int url_build_onvif_device_service_url(const char *url, int onvif_port,
                                       char *out_url, size_t out_size);

/**
 * Redact credentials for safe logging.
 */
int url_redact_for_logging(const char *url, char *out_url, size_t out_size);

#endif /* LIGHTNVR_URL_UTILS_H */