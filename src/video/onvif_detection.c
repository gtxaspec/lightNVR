#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <pthread.h>
#include <time.h>

#include "core/logger.h"
#include "core/config.h"
#include "core/curl_init.h"
#include "core/shutdown_coordinator.h"
#include "core/mqtt_client.h"
#include "utils/strings.h"
#include "video/onvif_detection.h"
#include "video/onvif_soap.h"
#include "video/detection_result.h"
#include "video/onvif_motion_recording.h"
#include "video/zone_filter.h"
#include "database/db_detections.h"

// Global variables
static bool initialized = false;
static CURL *curl_handle = NULL;
static pthread_mutex_t curl_mutex = PTHREAD_MUTEX_INITIALIZER;

// Structure to hold memory for curl response
typedef struct {
    char *memory;
    size_t size;
} memory_struct_t;

// Structure to hold ONVIF subscription information
typedef struct {
    char camera_url[512];           // URL of the camera (used as the key for lookup)
    char subscription_address[512]; // Address returned by CreatePullPointSubscription (full URL)
    char username[64];              // Username for authentication
    char password[64];              // Password for authentication
    time_t creation_time;
    time_t expiration_time;
    bool active;
} onvif_subscription_t;

// Hash map to store subscriptions by URL
#define MAX_SUBSCRIPTIONS 100
static onvif_subscription_t subscriptions[MAX_SUBSCRIPTIONS];
static int subscription_count = 0;
static pthread_mutex_t subscription_mutex = PTHREAD_MUTEX_INITIALIZER;

// Callback function for curl to write data
static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    memory_struct_t *mem = (memory_struct_t *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        log_error("Not enough memory for curl response");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Create ONVIF SOAP request with WS-Security (if credentials provided)
static char *create_onvif_request(const char *username, const char *password, const char *request_body) {
    bool has_credentials = (username && strlen(username) > 0 && password && strlen(password) > 0);
    char *security_header = NULL;

    if (!has_credentials) {
        log_info("Creating ONVIF request without authentication (no credentials provided)");
        security_header = strdup("");
    } else {
        log_info("Creating ONVIF request with WS-Security authentication");
        security_header = onvif_create_security_header(username, password);
        if (!security_header) {
            log_error("Failed to create WS-Security header");
            return NULL;
        }
    }

    // Allocate envelope dynamically to accommodate variable-length body and header
    size_t envelope_size = strlen(request_body) + strlen(security_header) + 1024;
    char *soap_request = malloc(envelope_size);
    if (!soap_request) {
        free(security_header);
        return NULL;
    }

    snprintf(soap_request, envelope_size,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\">"
        "<s:Header>%s</s:Header>"
        "<s:Body xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">%s</s:Body>"
        "</s:Envelope>",
        security_header, request_body);

    free(security_header);
    return soap_request;
}

// Send ONVIF request and get response
static char *send_onvif_request(const char *url, const char *username, const char *password, 
                               const char *request_body, const char *service) {
    if (!initialized || !curl_handle) {
        log_error("ONVIF detection system not initialized");
        return NULL;
    }

    pthread_mutex_lock(&curl_mutex);

    // Create full URL
    char full_url[512];
    snprintf(full_url, sizeof(full_url), "%s/onvif/%s", url, service);
    log_info("ONVIF Detection: Sending request to %s", full_url);

    // Create SOAP request
    char *soap_request = create_onvif_request(username, password, request_body);
    if (!soap_request) {
        log_error("Failed to create ONVIF request");
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    // Set up curl
    curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, soap_request);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(soap_request));

    // Set headers
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    // Set up response buffer
    memory_struct_t chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10);

    // Perform request
    CURLcode res = curl_easy_perform(curl_handle);

    // Clean up request
    free(soap_request);
    curl_slist_free_all(headers);

    // Check for errors
    if (res != CURLE_OK) {
        log_error("ONVIF Detection: curl_easy_perform() failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    // Get HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        log_error("ONVIF request failed with HTTP code %ld", http_code);
        if (chunk.size > 0) {
            onvif_log_soap_fault(chunk.memory, chunk.size, "ONVIF Detection");
        }
        free(chunk.memory);
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    pthread_mutex_unlock(&curl_mutex);
    return chunk.memory;
}

// Send ONVIF request to a full URL (bypassing the base URL + /onvif/ + service construction)
static char *send_onvif_request_to_url(const char *full_url, const char *username,
                                       const char *password, const char *request_body) {
    if (!initialized || !curl_handle) {
        log_error("ONVIF detection system not initialized");
        return NULL;
    }

    pthread_mutex_lock(&curl_mutex);
    log_info("ONVIF Detection: Sending request to %s", full_url);

    char *soap_request = create_onvif_request(username, password, request_body);
    if (!soap_request) {
        log_error("Failed to create ONVIF request");
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    curl_easy_setopt(curl_handle, CURLOPT_URL, full_url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, soap_request);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, strlen(soap_request));

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    memory_struct_t chunk;
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10);

    CURLcode res = curl_easy_perform(curl_handle);
    free(soap_request);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        log_error("ONVIF Detection: curl_easy_perform() failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        log_error("ONVIF request to %s failed with HTTP code %ld", full_url, http_code);
        if (chunk.size > 0) {
            onvif_log_soap_fault(chunk.memory, chunk.size, "ONVIF Detection");
        }
        free(chunk.memory);
        pthread_mutex_unlock(&curl_mutex);
        return NULL;
    }

    pthread_mutex_unlock(&curl_mutex);
    return chunk.memory;
}

// Extract subscription address from response
static char *extract_subscription_address(const char *response) {
    if (!response) return NULL;

    // Try different namespace prefixes
    const char *patterns[] = {
        "<wsa:Address>", "</wsa:Address>",
        "<wsa5:Address>", "</wsa5:Address>",
        "<Address>", "</Address>"
    };

    for (int i = 0; i < 3; i++) {
        const char *start = strstr(response, patterns[(ptrdiff_t)i*2]);
        const char *end = strstr(response, patterns[(ptrdiff_t)i*2+1]);
        
        if (start && end) {
            start += strlen(patterns[(ptrdiff_t)i * 2]);
            int length = (int)(end - start);
            
            return strndup(start, length);
        }
    }

    return NULL;
}

// Query GetServices and return the event service URL, or NULL if not found.
// The returned string is heap-allocated; caller must free() it.
static char *discover_event_service_url(const char *url, const char *username, const char *password) {
    const char *request_body =
        "<GetServices xmlns=\"http://www.onvif.org/ver10/device/wsdl\">"
            "<IncludeCapability>false</IncludeCapability>"
        "</GetServices>";

    // GetServices is always sent to the standard device management endpoint
    char *response = send_onvif_request(url, username, password, request_body, "device_service");
    if (!response) {
        log_warn("ONVIF: GetServices to device_service endpoint failed");
        return NULL;
    }

    // Find the events service namespace in the response
    const char *events_ns = "http://www.onvif.org/ver10/events/wsdl";
    const char *ns_pos = strstr(response, events_ns);
    if (!ns_pos) {
        log_warn("ONVIF: Events service namespace not found in GetServices response");
        free(response);
        return NULL;
    }

    // Search forward from the namespace position for XAddr (prefixed and un-prefixed variants)
    const char *xaddr_open[]  = {"<tds:XAddr>", "<XAddr>", NULL};
    const char *xaddr_close[] = {"</tds:XAddr>", "</XAddr>", NULL};
    char *event_url = NULL;

    for (int i = 0; xaddr_open[i] != NULL; i++) {
        const char *tag = strstr(ns_pos, xaddr_open[i]);
        // Only accept the XAddr if it is within the same Service element (~512 bytes window)
        if (tag && (tag - ns_pos) < 512) {
            const char *val_start = tag + strlen(xaddr_open[i]);
            const char *val_end   = strstr(val_start, xaddr_close[i]);
            if (val_end) {
                size_t url_len = (size_t)(val_end - val_start);
                event_url = strndup(val_start, url_len);
                break;
            }
        }
    }

    if (event_url) {
        log_info("ONVIF: Discovered event service URL: %s", event_url);
    } else {
        log_warn("ONVIF: Could not find XAddr for events service in GetServices response");
    }

    free(response);
    return event_url;
}

// Find or create subscription for a camera
static onvif_subscription_t *get_subscription(const char *url, const char *username, const char *password) {
    pthread_mutex_lock(&subscription_mutex);

    // Check if we already have a subscription for this URL
    for (int i = 0; i < subscription_count; i++) {
        if (strcmp(subscriptions[i].camera_url, url) == 0) {
            // Check if subscription is still valid
            time_t now;
            time(&now);
            
            if (subscriptions[i].active && now < subscriptions[i].expiration_time) {
                log_info("Reusing existing ONVIF subscription for %s", url);
                pthread_mutex_unlock(&subscription_mutex);
                return &subscriptions[i];
            } else {
                // Subscription expired, remove it
                log_info("ONVIF subscription for %s expired, creating new one", url);
                subscriptions[i].active = false;
                break;
            }
        }
    }

    log_info("Creating new ONVIF subscription for %s", url);

    // Create a new subscription
    const char *request_body =
        "<CreatePullPointSubscription xmlns=\"http://www.onvif.org/ver10/events/wsdl\">\n"
        "  <InitialTerminationTime>PT1H</InitialTerminationTime>\n"
        "</CreatePullPointSubscription>";

    // Dynamically discover the correct event service URL via GetServices.
    // Different vendors use different paths (e.g. Tapo uses "service", Lorex uses "event_service").
    char *discovered_url = discover_event_service_url(url, username, password);
    char *response = NULL;

    if (discovered_url) {
        log_info("ONVIF: Sending CreatePullPointSubscription to discovered URL: %s", discovered_url);
        response = send_onvif_request_to_url(discovered_url, username, password, request_body);
        free(discovered_url);
    }

    if (!response) {
        // Fall back through common event service path suffixes
        const char *fallback_services[] = {"service", "event_service", NULL};
        for (int i = 0; fallback_services[i] && !response; i++) {
            log_info("ONVIF: Trying fallback event endpoint: onvif/%s", fallback_services[i]);
            response = send_onvif_request(url, username, password, request_body, fallback_services[i]);
        }
    }

    if (!response) {
        log_error("Failed to create subscription on any endpoint");
        pthread_mutex_unlock(&subscription_mutex);
        return NULL;
    }

    char *subscription_address = extract_subscription_address(response);
    free(response);

    if (!subscription_address) {
        log_error("Failed to extract subscription address");
        pthread_mutex_unlock(&subscription_mutex);
        return NULL;
    }

    // Find an empty slot or reuse an inactive one
    int slot = -1;
    for (int i = 0; i < subscription_count; i++) {
        if (!subscriptions[i].active) {
            slot = i;
            break;
        }
    }

    // If no empty slot found, add to the end if there's space
    if (slot == -1 && subscription_count < MAX_SUBSCRIPTIONS) {
        slot = subscription_count++;
    }

    // If we found a slot, use it
    if (slot >= 0) {
        // Store camera URL, username, and password
        safe_strcpy(subscriptions[slot].camera_url, url, sizeof(subscriptions[slot].camera_url), 0);
        
        safe_strcpy(subscriptions[slot].username, username, sizeof(subscriptions[slot].username), 0);
        
        safe_strcpy(subscriptions[slot].password, password, sizeof(subscriptions[slot].password), 0);
        
        // Store subscription address
        safe_strcpy(subscriptions[slot].subscription_address, subscription_address, 
                sizeof(subscriptions[slot].subscription_address), 0);
        
        // Set timestamps
        time(&subscriptions[slot].creation_time);
        subscriptions[slot].expiration_time = subscriptions[slot].creation_time + 3600; // 1 hour
        subscriptions[slot].active = true;
        
        log_info("Successfully created ONVIF subscription for %s", url);
        free(subscription_address);
        pthread_mutex_unlock(&subscription_mutex);
        return &subscriptions[slot];
    }

    log_error("No space for new ONVIF subscription");
    free(subscription_address);
    pthread_mutex_unlock(&subscription_mutex);
    return NULL;
}

// Extract service name from subscription address
static char *extract_service_name(const char *subscription_address) {
    if (!subscription_address) return NULL;

    // Find the last slash
    const char *last_slash = strrchr(subscription_address, '/');
    if (!last_slash) return NULL;

    // Extract the service name
    char *service = strdup(last_slash + 1);
    return service;
}

// Check for motion events in ONVIF response
static bool has_motion_event(const char *response) {
    if (!response) return false;

    // Check for different motion event patterns.
    // Standard ONVIF topics:
    if (strstr(response, "RuleEngine/MotionDetector") ||
        strstr(response, "VideoAnalytics/Motion") ||
        strstr(response, "MotionAlarm")) {
        return true;
    }

    // Tapo / TP-Link specific topics (e.g. C545D firmware):
    //   tns1:RuleEngine/CellMotionDetector/Motion  (IsMotion: true/false)
    //   tns1:RuleEngine/PeopleDetector/People      (IsPeople: true/false)
    if (strstr(response, "CellMotionDetector") ||
        strstr(response, "PeopleDetector") ||
        strstr(response, "IsMotion") ||
        strstr(response, "IsPeople")) {
        return true;
    }

    return false;
}

/**
 * Initialize the ONVIF detection system
 */
int init_onvif_detection_system(void) {
    if (initialized && curl_handle) {
        log_info("ONVIF detection system already initialized");
        return 0;  // Already initialized and curl handle is valid
    }

    // If we have a curl handle but initialized is false, clean it up first
    if (curl_handle) {
        log_warn("ONVIF detection system has a curl handle but is marked as uninitialized, cleaning up");
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }

    // Initialize curl global (thread-safe, idempotent)
    if (curl_init_global() != 0) {
        log_error("Failed to initialize curl global");
        return -1;
    }

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        log_error("Failed to initialize curl handle");
        // Note: Don't call curl_global_cleanup() here - it's managed centrally
        return -1;
    }

    // Initialize subscriptions
    subscription_count = 0;
    memset(subscriptions, 0, sizeof(subscriptions));

    initialized = true;
    log_info("ONVIF detection system initialized successfully");
    return 0;
}

/**
 * Shutdown the ONVIF detection system
 */
void shutdown_onvif_detection_system(void) {
    log_info("Shutting down ONVIF detection system (initialized: %s, curl_handle: %p)",
             initialized ? "yes" : "no", (void*)curl_handle);

    // Cleanup curl handle if it exists
    if (curl_handle) {
        log_info("Cleaning up curl handle");
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }

    // Note: Don't call curl_global_cleanup() here - it's managed centrally in curl_init.c
    // The global cleanup will happen at program shutdown

    initialized = false;
    log_info("ONVIF detection system shutdown complete");
}

/**
 * Detect motion using ONVIF events
 */
int detect_motion_onvif(const char *onvif_url, const char *username, const char *password,
                       detection_result_t *result, const char *stream_name) {
    // Check if we're in shutdown mode
    if (is_shutdown_initiated()) {
        log_info("ONVIF Detection: System shutdown in progress, skipping detection");
        return -1;
    }

    // Thread safety for curl operations
    pthread_mutex_lock(&curl_mutex);
    
    // Initialize result to empty at the beginning to prevent segmentation fault
    if (result) {
        memset(result, 0, sizeof(detection_result_t));
    } else {
        log_error("ONVIF Detection: NULL result pointer provided");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }
    
    log_info("ONVIF Detection: Starting detection with URL: %s", onvif_url ? onvif_url : "NULL");
    log_info("ONVIF Detection: Stream name: %s", stream_name ? stream_name : "NULL");

    if (!initialized || !curl_handle) {
        log_error("ONVIF detection system not initialized");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Validate parameters - allow empty credentials (empty strings) but not NULL pointers
    // cppcheck-suppress knownConditionTrueFalse
    if (!onvif_url || !username || !password || !result) {
        log_error("Invalid parameters for detect_motion_onvif (NULL pointers not allowed)");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Log credential status for debugging
    if (strlen(username) == 0 || strlen(password) == 0) {
        log_info("ONVIF Detection: Using camera without authentication (empty credentials)");
    } else {
        log_info("ONVIF Detection: Using camera with authentication (username: %s)", username);
    }

    // Get or create subscription
    pthread_mutex_unlock(&curl_mutex); // Unlock before calling get_subscription which will lock again
    onvif_subscription_t *subscription = get_subscription(onvif_url, username, password);
    if (!subscription) {
        log_error("Failed to get subscription for %s", onvif_url);
        return -1;
    }

    // Create pull messages request
    const char *request_body =
        "<PullMessages xmlns=\"http://www.onvif.org/ver10/events/wsdl\">\n"
        "  <Timeout>PT5S</Timeout>\n"
        "  <MessageLimit>100</MessageLimit>\n"
        "</PullMessages>";

    // Send PullMessages directly to the subscription address (the full URL returned by
    // CreatePullPointSubscription per ONVIF spec).  Fall back to the legacy path-extraction
    // approach only when the stored address is not an absolute HTTP URL.
    char *response = NULL;
    if (strncmp(subscription->subscription_address, "http://", 7) == 0 ||
        strncmp(subscription->subscription_address, "https://", 8) == 0) {
        log_info("ONVIF Detection: Sending PullMessages to %s", subscription->subscription_address);
        response = send_onvif_request_to_url(subscription->subscription_address,
                                             subscription->username,
                                             subscription->password,
                                             request_body);
    } else {
        // Legacy fallback: extract last path component and re-append under /onvif/
        log_warn("ONVIF Detection: subscription_address is not a full URL ('%s'), using legacy path extraction",
                 subscription->subscription_address);
        char *service = extract_service_name(subscription->subscription_address);
        if (service) {
            response = send_onvif_request(subscription->camera_url,
                                         subscription->username,
                                         subscription->password,
                                         request_body,
                                         service);
            free(service);
        }
    }

    if (!response) {
        log_error("Failed to pull messages from subscription");
        
        // If pulling messages fails, the subscription might be invalid
        // Mark it as inactive so we'll create a new one next time
        pthread_mutex_lock(&subscription_mutex);
        subscription->active = false;
        pthread_mutex_unlock(&subscription_mutex);
        
        return -1;
    }

    // Check for motion events
    bool motion_detected = has_motion_event(response);
    free(response);

    if (motion_detected) {
        log_info("ONVIF Detection: Motion detected for %s", stream_name);

        // Create a single detection that covers the whole frame
        result->count = 1;
        safe_strcpy(result->detections[0].label, "motion", MAX_LABEL_LENGTH, 0);
        result->detections[0].confidence = 1.0f;
        result->detections[0].x = 0.0f;
        result->detections[0].y = 0.0f;
        result->detections[0].width = 1.0f;
        result->detections[0].height = 1.0f;

        // Filter detections by zones before storing
        if (stream_name && stream_name[0] != '\0') {
            log_info("ONVIF Detection: Filtering detections by zones for stream %s", stream_name);
            int filter_ret = filter_detections_by_zones(stream_name, result);
            if (filter_ret != 0) {
                log_warn("Failed to filter detections by zones, storing all detections");
            }

            // Store the detection in the database (no recording_id linkage for ONVIF)
            time_t timestamp = time(NULL);
            store_detections_in_db(stream_name, result, timestamp, 0);

            // Publish to MQTT and trigger motion recording if detections remain after filtering
            if (result->count > 0) {
                mqtt_publish_detection(stream_name, result, timestamp);
                process_motion_event(stream_name, true, timestamp);
            }
        } else {
            log_warn("No stream name provided, skipping database storage");
        }
    } else {
        log_info("ONVIF Detection: No motion detected for %s", stream_name);
        result->count = 0;

        // Notify motion recording that motion has ended
        if (stream_name && stream_name[0] != '\0') {
            process_motion_event(stream_name, false, time(NULL));
        }
    }

    return 0;
}
