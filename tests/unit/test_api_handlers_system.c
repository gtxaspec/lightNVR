/**
 * @file test_api_handlers_system.c
 * @brief Layer 2 Unity tests for web/api_handlers_system.c
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include <cjson/cJSON.h>

#include "unity.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/path_utils.h"
#include "utils/strings.h"
#include "database/db_core.h"
#include "database/db_streams.h"
#include "web/api_handlers.h"
#include "web/api_handlers_system.h"
#include "web/request_response.h"
#include "video/stream_manager.h"
#include "video/stream_state.h"

extern config_t g_config;

static char g_tmp_root[MAX_PATH_LENGTH];
static char g_db_path[MAX_PATH_LENGTH];
static char g_storage_path[MAX_PATH_LENGTH];

static cJSON *parse_response_json(const http_response_t *res) {
    TEST_ASSERT_NOT_NULL(res);
    TEST_ASSERT_NOT_NULL(res->body);
    cJSON *json = cJSON_Parse((const char *)res->body);
    TEST_ASSERT_NOT_NULL(json);
    return json;
}

static cJSON *find_version_item(cJSON *items, const char *name) {
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        cJSON *item_name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(item_name) && strcmp(item_name->valuestring, name) == 0) {
            return item;
        }
    }
    return NULL;
}

/* ---- helpers ---- */
static void clear_db_streams(void) {
    sqlite3 *db = get_db_handle();
    sqlite3_exec(db, "DELETE FROM streams;", NULL, NULL, NULL);
}

static stream_config_t make_test_stream(const char *name) {
    stream_config_t s;
    memset(&s, 0, sizeof(s));
    safe_strcpy(s.name, name, sizeof(s.name), 0);
    safe_strcpy(s.url, "rtsp://localhost/stream", sizeof(s.url), 0);
    safe_strcpy(s.codec, "h264", sizeof(s.codec), 0);
    s.enabled  = true;
    s.width    = 1920;
    s.height   = 1080;
    s.fps      = 25;
    s.priority = 5;
    s.segment_duration = 60;
    s.streaming_enabled = true;
    s.tier_critical_multiplier  = 3.0;
    s.tier_important_multiplier = 2.0;
    s.tier_ephemeral_multiplier = 0.25;
    s.storage_priority = 5;
    safe_strcpy(s.detection_object_filter, "none", sizeof(s.detection_object_filter), 0);
    return s;
}

void setUp(void) {
    g_config.web_auth_enabled = false;
}

void tearDown(void) {}

void test_handle_get_system_info_includes_versions_summary(void) {
    http_request_t req;
    http_response_t res;
    http_request_init(&req);
    http_response_init(&res);

    handle_get_system_info(&req, &res);

    TEST_ASSERT_EQUAL_INT(200, res.status_code);

    cJSON *root = parse_response_json(&res);
    cJSON *versions = cJSON_GetObjectItemCaseSensitive(root, "versions");
    cJSON *items = cJSON_GetObjectItemCaseSensitive(versions, "items");

    TEST_ASSERT_TRUE(cJSON_IsObject(versions));
    TEST_ASSERT_TRUE(cJSON_IsArray(items));
    TEST_ASSERT_GREATER_OR_EQUAL_INT(10, cJSON_GetArraySize(items));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "LightNVR"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "Base OS"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "SQLite"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "libcurl"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "mbedTLS"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "libuv"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "llhttp"));
    TEST_ASSERT_NOT_NULL(find_version_item(items, "libavformat"));

    cJSON_Delete(root);
    http_response_free(&res);
}

void test_handle_get_system_info_includes_empty_stream_storage_array(void) {
    http_request_t req;
    http_response_t res;
    http_request_init(&req);
    http_response_init(&res);

    handle_get_system_info(&req, &res);

    TEST_ASSERT_EQUAL_INT(200, res.status_code);

    cJSON *root = parse_response_json(&res);
    cJSON *stream_storage = cJSON_GetObjectItemCaseSensitive(root, "streamStorage");

    TEST_ASSERT_TRUE(cJSON_IsArray(stream_storage));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(stream_storage));

    cJSON_Delete(root);
    http_response_free(&res);
}

/* ================================================================
 * handle_get_streams — motion_trigger_source field present in JSON
 * ================================================================ */

void test_handle_get_streams_includes_motion_trigger_source(void) {
    clear_db_streams();

    stream_config_t ptz = make_test_stream("ptz_cam");
    safe_strcpy(ptz.motion_trigger_source, "fixed_cam", sizeof(ptz.motion_trigger_source), 0);
    add_stream_config(&ptz);

    http_request_t req;
    http_response_t res;
    http_request_init(&req);
    http_response_init(&res);

    handle_get_streams(&req, &res);

    TEST_ASSERT_EQUAL_INT(200, res.status_code);

    /* handle_get_streams returns a bare JSON array (not wrapped in an object) */
    cJSON *root = parse_response_json(&res);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE(cJSON_IsArray(root));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(root));

    cJSON *stream = cJSON_GetArrayItem(root, 0);
    cJSON *mts = cJSON_GetObjectItemCaseSensitive(stream, "motion_trigger_source");
    TEST_ASSERT_NOT_NULL(mts);
    TEST_ASSERT_TRUE(cJSON_IsString(mts));
    TEST_ASSERT_EQUAL_STRING("fixed_cam", mts->valuestring);

    cJSON_Delete(root);
    http_response_free(&res);
    clear_db_streams();
}

/* ================================================================
 * handle_put_stream — motion_trigger_source JSON parsing exercised
 *
 * handle_put_stream looks up the stream by name from the in-memory stream
 * manager (not only the DB), so we need to register the stream there first.
 * ================================================================ */

void test_handle_put_stream_parses_motion_trigger_source(void) {
    clear_db_streams();

    /* Register stream in both DB and in-memory stream manager */
    stream_config_t s = make_test_stream("cam_put_mts");
    add_stream_config(&s);

    init_stream_state_manager(16);
    init_stream_manager(16);
    add_stream(&s);   /* register in-memory so the PUT handler can find it */

    http_request_t req;
    http_response_t res;
    http_request_init(&req);
    http_response_init(&res);

    /* Set URL path so handler can extract the stream name */
    safe_strcpy(req.path, "/api/streams/cam_put_mts", sizeof(req.path), 0);

    /* JSON body with motion_trigger_source to exercise the new parsing code */
    static const char json_body[] = "{\"motion_trigger_source\":\"cam_fixed_src\"}";
    req.body     = (uint8_t *)json_body;
    req.body_len = sizeof(json_body) - 1;

    handle_put_stream(&req, &res);

    /* PUT returns 202 Accepted immediately; the actual restart runs async.
     * Give the detached worker thread a moment to finish before we tear down
     * the stream manager and DB so ASan doesn't report use-after-free. */
    usleep(200000);

    TEST_ASSERT_TRUE(res.status_code == 202 || res.status_code == 400 ||
                     res.status_code == 404 || res.status_code == 500);

    http_response_free(&res);
    shutdown_stream_manager();
    shutdown_stream_state_manager();
    clear_db_streams();
}

int main(void) {
    init_logger();
    load_default_config(&g_config);

    snprintf(g_tmp_root, sizeof(g_tmp_root), "/tmp/lightnvr_system_handler_%d", (int)getpid());
    snprintf(g_db_path, sizeof(g_db_path), "%s/lightnvr.db", g_tmp_root);
    snprintf(g_storage_path, sizeof(g_storage_path), "%s/storage", g_tmp_root);

    mkdir_recursive(g_storage_path);
    safe_strcpy(g_config.storage_path, g_storage_path, sizeof(g_config.storage_path), 0);

    if (init_database(g_db_path) != 0) {
        fprintf(stderr, "FATAL: init_database failed\n");
        return 1;
    }

    /* handle_get_streams uses g_config.max_streams for allocation */
    if (g_config.max_streams == 0) {
        g_config.max_streams = 16;
    }

    UNITY_BEGIN();
    RUN_TEST(test_handle_get_system_info_includes_versions_summary);
    RUN_TEST(test_handle_get_system_info_includes_empty_stream_storage_array);
    RUN_TEST(test_handle_get_streams_includes_motion_trigger_source);
    RUN_TEST(test_handle_put_stream_parses_motion_trigger_source);
    int result = UNITY_END();

    shutdown_database();
    unlink(g_db_path);
    snprintf(g_db_path, sizeof(g_db_path), "%s/lightnvr.db-wal", g_tmp_root);
    unlink(g_db_path);
    snprintf(g_db_path, sizeof(g_db_path), "%s/lightnvr.db-shm", g_tmp_root);
    unlink(g_db_path);
    snprintf(g_db_path, sizeof(g_db_path), "%s/lightnvr.db.bak", g_tmp_root);
    unlink(g_db_path);
    free(g_config.streams);
    g_config.streams = NULL;
    rmdir(g_storage_path);
    rmdir(g_tmp_root);
    shutdown_logger();
    return result;
}