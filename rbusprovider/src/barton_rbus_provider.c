#include "barton_rbus_provider.h"
#include "barton-core-client.h"
#include "barton-core-device.h"
#include "barton-core-endpoint.h"
#include "barton-core-resource.h"
#include "barton-core-status.h"
#include "events/barton-core-device-added-event.h"
#include "events/barton-core-device-removed-event.h"
#include "events/barton-core-resource-updated-event.h"
#include "events/barton-core-discovery-started-event.h"
#include "events/barton-core-discovery-stopped-event.h"

#include <rbus/rbus.h>
#include <rbus/rbuscore.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static BartonRbusContext *s_ctx = NULL;

/* ======================================================================
 * Utility
 * ====================================================================*/
static gchar *device_to_json(BCoreDevice *device)
{
    g_autofree gchar *uuid        = NULL;
    g_autofree gchar *deviceClass = NULL;
    g_autofree gchar *uri         = NULL;
    g_autofree gchar *driver      = NULL;
    guint classVersion = 0;

    g_object_get(G_OBJECT(device),
        B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_UUID],                   &uuid,
        B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_DEVICE_CLASS],           &deviceClass,
        B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_DEVICE_CLASS_VERSION],   &classVersion,
        B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_URI],                    &uri,
        B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_MANAGING_DEVICE_DRIVER], &driver,
        NULL);

    return g_strdup_printf(
        "{\"uuid\":\"%s\",\"deviceClass\":\"%s\",\"classVersion\":%u,"
        "\"uri\":\"%s\",\"driver\":\"%s\"}",
        uuid        ? uuid        : "",
        deviceClass ? deviceClass : "",
        classVersion,
        uri         ? uri         : "",
        driver      ? driver      : "");
}

static gchar *devices_list_to_json(GList *devices)
{
    GString *sb = g_string_new("[");
    bool first = true;
    for (GList *it = devices; it != NULL; it = it->next)
    {
        g_autofree gchar *j = device_to_json(B_CORE_DEVICE(it->data));
        if (!first) g_string_append(sb, ",");
        g_string_append(sb, j);
        first = false;
    }
    g_string_append(sb, "]");
    return g_string_free(sb, FALSE);
}

/* ======================================================================
 * Property GET  — handles all Device.IoT.* scalar properties
 * ====================================================================*/
static rbusError_t propGetHandler(rbusHandle_t handle,
                                   rbusProperty_t property,
                                   rbusGetHandlerOptions_t *options)
{
    (void)handle; (void)options;
    const char *name = rbusProperty_GetName(property);
    rbusValue_t val;
    rbusValue_Init(&val);
    rbusError_t rc = RBUS_ERROR_SUCCESS;

    if (strcmp(name, BARTON_RBUS_PROP_STATUS) == 0)
    {
        rbusValue_SetString(val, (s_ctx && s_ctx->client) ? "running" : "stopped");
    }
    else if (strcmp(name, BARTON_RBUS_PROP_DEVICE_COUNT) == 0)
    {
        GList *devices = b_core_client_get_devices(s_ctx->client);
        rbusValue_SetUInt32(val, (uint32_t)g_list_length(devices));
        g_list_free_full(devices, g_object_unref);
    }
    else if (strcmp(name, BARTON_RBUS_PROP_DEVICES_JSON) == 0)
    {
        GList *devices = b_core_client_get_devices(s_ctx->client);
        g_autofree gchar *json = devices_list_to_json(devices);
        g_list_free_full(devices, g_object_unref);
        rbusValue_SetString(val, json);
    }
    else if (strcmp(name, BARTON_RBUS_PROP_DISC_ACTIVE) == 0)
    {
        rbusValue_SetBoolean(val, s_ctx->discoveryActive);
    }
    else
    {
        rbusValue_Release(val);
        return RBUS_ERROR_ELEMENT_DOES_NOT_EXIST;
    }

    rbusProperty_SetValue(property, val);
    rbusValue_Release(val);
    return rc;
}

/* ======================================================================
 * Event sub handler
 * ====================================================================*/
static rbusError_t eventSubHandler(rbusHandle_t handle,
                                    rbusEventSubAction_t action,
                                    const char *eventName,
                                    rbusFilter_t filter,
                                    int32_t interval,
                                    bool *autoPublish)
{
    (void)handle; (void)filter; (void)interval;
    *autoPublish = false;
    printf("[IoT] %s: %s\n",
           action == RBUS_EVENT_ACTION_SUBSCRIBE ? "SUBSCRIBE" : "UNSUBSCRIBE",
           eventName);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * Methods
 * ====================================================================*/

/* Device.IoT.Discovery.Start()
 * in:  deviceClass(string), timeout(uint32)
 * out: success(bool)
 */
static rbusError_t methodDiscStart(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vClass   = rbusObject_GetValue(in, "deviceClass");
    rbusValue_t vTimeout = rbusObject_GetValue(in, "timeout");
    const char *dc = vClass   ? rbusValue_GetString(vClass, NULL) : "matter";
    uint32_t    to = vTimeout ? rbusValue_GetUInt32(vTimeout)      : 60;

    GList *classes = g_list_append(NULL, (gpointer)dc);
    GError *err = NULL;
    gboolean ok = b_core_client_discover_start(s_ctx->client, classes, NULL, (guint16)to, &err);
    g_list_free(classes);
    if (err) g_error_free(err);
    if (ok) s_ctx->discoveryActive = true;

    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Discovery.Stop()
 * in:  deviceClass(string, optional)
 * out: success(bool)
 */
static rbusError_t methodDiscStop(rbusHandle_t handle, const char *name,
                                   rbusObject_t in, rbusObject_t out,
                                   rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vClass = rbusObject_GetValue(in, "deviceClass");
    GList *classes = NULL;
    if (vClass) classes = g_list_append(NULL, (gpointer)rbusValue_GetString(vClass, NULL));

    gboolean ok = b_core_client_discover_stop(s_ctx->client, classes);
    if (classes) g_list_free(classes);
    if (ok) s_ctx->discoveryActive = false;

    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Device.Get()
 * in:  deviceId(string)
 * out: device(string JSON)
 */
static rbusError_t methodGetDevice(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vId = rbusObject_GetValue(in, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    BCoreDevice *dev = b_core_client_get_device_by_id(s_ctx->client,
                           rbusValue_GetString(vId, NULL));
    rbusValue_t v; rbusValue_Init(&v);
    if (dev) {
        g_autofree gchar *json = device_to_json(dev);
        rbusValue_SetString(v, json);
        g_object_unref(dev);
    } else {
        rbusValue_SetString(v, "{}");
    }
    rbusObject_SetValue(out, "device", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Device.Remove()
 * in:  deviceId(string)
 * out: success(bool)
 */
static rbusError_t methodRemoveDevice(rbusHandle_t handle, const char *name,
                                       rbusObject_t in, rbusObject_t out,
                                       rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vId = rbusObject_GetValue(in, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_remove_device(s_ctx->client,
                      rbusValue_GetString(vId, NULL));
    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Resource.Read()
 * in:  uri(string)
 * out: value(string)
 */
static rbusError_t methodReadResource(rbusHandle_t handle, const char *name,
                                       rbusObject_t in, rbusObject_t out,
                                       rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vUri = rbusObject_GetValue(in, "uri");
    if (!vUri) return RBUS_ERROR_INVALID_INPUT;

    GError *err = NULL;
    gchar *val = b_core_client_read_resource(s_ctx->client,
                     rbusValue_GetString(vUri, NULL), &err);
    if (err) g_error_free(err);

    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetString(v, val ? val : "");
    rbusObject_SetValue(out, "value", v);
    rbusValue_Release(v);
    g_free(val);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Resource.Write()
 * in:  uri(string), value(string)
 * out: success(bool)
 */
static rbusError_t methodWriteResource(rbusHandle_t handle, const char *name,
                                        rbusObject_t in, rbusObject_t out,
                                        rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vUri = rbusObject_GetValue(in, "uri");
    rbusValue_t vVal = rbusObject_GetValue(in, "value");
    if (!vUri || !vVal) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_write_resource(s_ctx->client,
                      rbusValue_GetString(vUri, NULL),
                      rbusValue_GetString(vVal, NULL));
    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Resource.Execute()
 * in:  uri(string), payload(string, optional)
 * out: response(string), success(bool)
 */
static rbusError_t methodExecResource(rbusHandle_t handle, const char *name,
                                       rbusObject_t in, rbusObject_t out,
                                       rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vUri     = rbusObject_GetValue(in, "uri");
    rbusValue_t vPayload = rbusObject_GetValue(in, "payload");
    if (!vUri) return RBUS_ERROR_INVALID_INPUT;

    const char *payload = vPayload ? rbusValue_GetString(vPayload, NULL) : NULL;
    char *response = NULL;
    gboolean ok = b_core_client_execute_resource(s_ctx->client,
                      rbusValue_GetString(vUri, NULL), payload, &response);

    rbusValue_t vOk, vResp;
    rbusValue_Init(&vOk);   rbusValue_SetBoolean(vOk, ok ? true : false);
    rbusValue_Init(&vResp); rbusValue_SetString(vResp, response ? response : "");
    rbusObject_SetValue(out, "success",  vOk);
    rbusObject_SetValue(out, "response", vResp);
    rbusValue_Release(vOk);
    rbusValue_Release(vResp);
    free(response);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Matter.Commission()
 * in:  setupPayload(string), timeout(uint32)
 * out: success(bool)
 */
static rbusError_t methodCommission(rbusHandle_t handle, const char *name,
                                     rbusObject_t in, rbusObject_t out,
                                     rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vPayload = rbusObject_GetValue(in, "setupPayload");
    rbusValue_t vTimeout = rbusObject_GetValue(in, "timeout");
    if (!vPayload) return RBUS_ERROR_INVALID_INPUT;

    uint32_t to = vTimeout ? rbusValue_GetUInt32(vTimeout) : 120;
    GError *err = NULL;
    gboolean ok = b_core_client_commission_device(s_ctx->client,
                      (gchar *)rbusValue_GetString(vPayload, NULL), (guint16)to, &err);
    if (err) g_error_free(err);

    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.Matter.OpenCommissioningWindow()
 * in:  deviceId(string, "0" for local), timeout(uint32)
 * out: manualCode(string), qrCode(string)
 */
static rbusError_t methodOpenCommWindow(rbusHandle_t handle, const char *name,
                                         rbusObject_t in, rbusObject_t out,
                                         rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vDevId   = rbusObject_GetValue(in, "deviceId");
    rbusValue_t vTimeout = rbusObject_GetValue(in, "timeout");
    const char *deviceId = vDevId   ? rbusValue_GetString(vDevId, NULL) : "0";
    uint32_t    to       = vTimeout ? rbusValue_GetUInt32(vTimeout)     : 180;

    BCoreCommissioningInfo *info =
        b_core_client_open_commissioning_window(s_ctx->client, deviceId, (guint16)to);

    rbusValue_t vManual, vQr;
    rbusValue_Init(&vManual);
    rbusValue_Init(&vQr);

    if (info) {
        g_autofree gchar *manual = NULL;
        g_autofree gchar *qr     = NULL;
        g_object_get(G_OBJECT(info),
            B_CORE_COMMISSIONING_INFO_PROPERTY_NAMES[B_CORE_COMMISSIONING_INFO_PROP_MANUAL_CODE], &manual,
            B_CORE_COMMISSIONING_INFO_PROPERTY_NAMES[B_CORE_COMMISSIONING_INFO_PROP_QR_CODE],     &qr,
            NULL);
        rbusValue_SetString(vManual, manual ? manual : "");
        rbusValue_SetString(vQr,     qr     ? qr     : "");
        g_object_unref(info);
    } else {
        rbusValue_SetString(vManual, "");
        rbusValue_SetString(vQr,     "");
    }
    rbusObject_SetValue(out, "manualCode", vManual);
    rbusObject_SetValue(out, "qrCode",     vQr);
    rbusValue_Release(vManual);
    rbusValue_Release(vQr);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.GetProperty()
 * in:  key(string)
 * out: value(string)
 */
static rbusError_t methodGetProperty(rbusHandle_t handle, const char *name,
                                      rbusObject_t in, rbusObject_t out,
                                      rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vKey = rbusObject_GetValue(in, "key");
    if (!vKey) return RBUS_ERROR_INVALID_INPUT;

    gchar *val = b_core_client_get_system_property(s_ctx->client,
                     rbusValue_GetString(vKey, NULL));
    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetString(v, val ? val : "");
    rbusObject_SetValue(out, "value", v);
    rbusValue_Release(v);
    g_free(val);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.SetProperty()
 * in:  key(string), value(string)
 * out: success(bool)
 */
static rbusError_t methodSetProperty(rbusHandle_t handle, const char *name,
                                      rbusObject_t in, rbusObject_t out,
                                      rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;
    rbusValue_t vKey = rbusObject_GetValue(in, "key");
    rbusValue_t vVal = rbusObject_GetValue(in, "value");
    if (!vKey || !vVal) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_set_system_property(s_ctx->client,
                      rbusValue_GetString(vKey, NULL),
                      rbusValue_GetString(vVal, NULL));
    rbusValue_t v; rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* Device.IoT.GetStatus()
 * in:  (none)
 * out: status(string JSON)
 */


static rbusError_t methodGetStatus(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)in; (void)async;

    /* b_core_client_get_status() returns the same data as 'gs' command.
     * BCoreStatus has a "json" property that is the full JSON string. */
    g_autoptr(BCoreStatus) status = b_core_client_get_status(s_ctx->client);

    g_autofree gchar *json = NULL;

    if (status)
    {
        g_object_get(G_OBJECT(status),
            B_CORE_STATUS_PROPERTY_NAMES[B_CORE_STATUS_PROP_JSON], &json,
            NULL);
    }

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, json ? json : "{}");
    rbusObject_SetValue(out, "status", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * GObject signal → rbus event publishers
 * ====================================================================*/
static void onDeviceAdded(BCoreClient *src, BCoreDeviceAddedEvent *event, gpointer ud)
{
    (void)src; (void)ud;
    g_autofree gchar *deviceId = NULL, *uri = NULL, *deviceClass = NULL;
    guint classVersion = 0;
    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_UUID],                   &deviceId,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_URI],                    &uri,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS],           &deviceClass,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS_VERSION],   &classVersion,
        NULL);

    printf("[IoT] DeviceAdded: id=%s class=%s\n",
           deviceId ? deviceId : "", deviceClass ? deviceClass : "");

    rbusObject_t data; rbusObject_Init(&data, NULL);
    rbusValue_t vId, vUri, vClass, vVer;
    rbusValue_Init(&vId);    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_Init(&vUri);   rbusValue_SetString(vUri,   uri         ? uri         : "");
    rbusValue_Init(&vClass); rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusValue_Init(&vVer);   rbusValue_SetUInt32(vVer,   classVersion);
    rbusObject_SetValue(data, "deviceId",     vId);
    rbusObject_SetValue(data, "uri",          vUri);
    rbusObject_SetValue(data, "deviceClass",  vClass);
    rbusObject_SetValue(data, "classVersion", vVer);
    rbusValue_Release(vId); rbusValue_Release(vUri);
    rbusValue_Release(vClass); rbusValue_Release(vVer);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DEVICE_ADDED, data);
    rbusObject_Release(data);
}

static void onDeviceRemoved(BCoreClient *src, BCoreDeviceRemovedEvent *event, gpointer ud)
{
    (void)src; (void)ud;
    g_autofree gchar *deviceId = NULL, *deviceClass = NULL;
    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_UUID],  &deviceId,
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_CLASS], &deviceClass,
        NULL);
    printf("[IoT] DeviceRemoved: id=%s\n", deviceId ? deviceId : "");

    rbusObject_t data; rbusObject_Init(&data, NULL);
    rbusValue_t vId, vClass;
    rbusValue_Init(&vId);    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_Init(&vClass); rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusObject_SetValue(data, "deviceId",    vId);
    rbusObject_SetValue(data, "deviceClass", vClass);
    rbusValue_Release(vId); rbusValue_Release(vClass);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DEVICE_REMOVED, data);
    rbusObject_Release(data);
}

static void onResourceUpdated(BCoreClient *src, BCoreResourceUpdatedEvent *event, gpointer ud)
{
    (void)src; (void)ud;
    g_autoptr(BCoreResource) resource = NULL;
    g_object_get(G_OBJECT(event),
        B_CORE_RESOURCE_UPDATED_EVENT_PROPERTY_NAMES[B_CORE_RESOURCE_UPDATED_EVENT_PROP_RESOURCE],
        &resource, NULL);
    if (!resource) return;

    g_autofree gchar *uri = NULL, *value = NULL;
    g_object_get(G_OBJECT(resource),
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_URI],   &uri,
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_VALUE], &value,
        NULL);

    rbusObject_t data; rbusObject_Init(&data, NULL);
    rbusValue_t vUri, vVal;
    rbusValue_Init(&vUri); rbusValue_SetString(vUri, uri   ? uri   : "");
    rbusValue_Init(&vVal); rbusValue_SetString(vVal, value ? value : "");
    rbusObject_SetValue(data, "uri",   vUri);
    rbusObject_SetValue(data, "value", vVal);
    rbusValue_Release(vUri); rbusValue_Release(vVal);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_RESOURCE_UPDATED, data);
    rbusObject_Release(data);
}

static void onDiscoveryStarted(BCoreClient *src, BCoreDiscoveryStartedEvent *event, gpointer ud)
{
    (void)src; (void)event; (void)ud;
    s_ctx->discoveryActive = true;
    printf("[IoT] DiscoveryStarted\n");
    rbusObject_t data; rbusObject_Init(&data, NULL);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DISC_STARTED, data);
    rbusObject_Release(data);
}

static void onDiscoveryStopped(BCoreClient *src, BCoreDiscoveryStoppedEvent *event, gpointer ud)
{
    (void)src; (void)event; (void)ud;
    s_ctx->discoveryActive = false;
    printf("[IoT] DiscoveryStopped\n");
    rbusObject_t data; rbusObject_Init(&data, NULL);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DISC_STOPPED, data);
    rbusObject_Release(data);
}

/* ======================================================================
 * Public API
 * ====================================================================*/
bool barton_rbus_provider_init(BartonRbusContext *ctx, BCoreClient *client)
{
    ctx->client          = client;
    ctx->discoveryActive = false;
    s_ctx = ctx;

    rbusError_t rc = rbus_open(&ctx->handle, BARTON_RBUS_COMPONENT_NAME);
    if (rc != RBUS_ERROR_SUCCESS) {
        fprintf(stderr, "[IoT] rbus_open failed: %d\n", rc);
        return false;
    }

    rbusDataElement_t elements[] = {
        /* Properties */
        {BARTON_RBUS_PROP_STATUS,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        {BARTON_RBUS_PROP_DEVICE_COUNT,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        {BARTON_RBUS_PROP_DEVICES_JSON,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        {BARTON_RBUS_PROP_DISC_ACTIVE,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        /* Methods */
        {BARTON_RBUS_METHOD_DISC_START,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodDiscStart}},

        {BARTON_RBUS_METHOD_DISC_STOP,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodDiscStop}},

        {BARTON_RBUS_METHOD_GET_DEVICE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodGetDevice}},

        {BARTON_RBUS_METHOD_REMOVE_DEVICE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodRemoveDevice}},

        {BARTON_RBUS_METHOD_READ_RESOURCE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodReadResource}},

        {BARTON_RBUS_METHOD_WRITE_RESOURCE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodWriteResource}},

        {BARTON_RBUS_METHOD_EXEC_RESOURCE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodExecResource}},

        {BARTON_RBUS_METHOD_COMMISSION,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodCommission}},

        {BARTON_RBUS_METHOD_OPEN_COMM_WIN,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodOpenCommWindow}},

        {BARTON_RBUS_METHOD_GET_STATUS,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodGetStatus}},

        {BARTON_RBUS_METHOD_GET_PROPERTY,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodGetProperty}},

        {BARTON_RBUS_METHOD_SET_PROPERTY,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodSetProperty}},

        /* Events */
        {BARTON_RBUS_EVT_DEVICE_ADDED,
         RBUS_ELEMENT_TYPE_EVENT,
         {NULL, NULL, NULL, NULL, eventSubHandler, NULL}},

        {BARTON_RBUS_EVT_DEVICE_REMOVED,
         RBUS_ELEMENT_TYPE_EVENT,
         {NULL, NULL, NULL, NULL, eventSubHandler, NULL}},

        {BARTON_RBUS_EVT_RESOURCE_UPDATED,
         RBUS_ELEMENT_TYPE_EVENT,
         {NULL, NULL, NULL, NULL, eventSubHandler, NULL}},

        {BARTON_RBUS_EVT_DISC_STARTED,
         RBUS_ELEMENT_TYPE_EVENT,
         {NULL, NULL, NULL, NULL, eventSubHandler, NULL}},

        {BARTON_RBUS_EVT_DISC_STOPPED,
         RBUS_ELEMENT_TYPE_EVENT,
         {NULL, NULL, NULL, NULL, eventSubHandler, NULL}},
    };

    int n = (int)(sizeof(elements) / sizeof(elements[0]));
    rc = rbus_regDataElements(ctx->handle, n, elements);
    if (rc != RBUS_ERROR_SUCCESS) {
        fprintf(stderr, "[IoT] rbus_regDataElements failed: %d\n", rc);
        rbus_close(ctx->handle);
        return false;
    }

    g_signal_connect(client, B_CORE_CLIENT_SIGNAL_NAME_DEVICE_ADDED,
                     G_CALLBACK(onDeviceAdded), NULL);
    g_signal_connect(client, B_CORE_CLIENT_SIGNAL_NAME_DEVICE_REMOVED,
                     G_CALLBACK(onDeviceRemoved), NULL);
    g_signal_connect(client, B_CORE_CLIENT_SIGNAL_NAME_RESOURCE_UPDATED,
                     G_CALLBACK(onResourceUpdated), NULL);
    g_signal_connect(client, B_CORE_CLIENT_SIGNAL_NAME_DISCOVERY_STARTED,
                     G_CALLBACK(onDiscoveryStarted), NULL);
    g_signal_connect(client, B_CORE_CLIENT_SIGNAL_NAME_DISCOVERY_STOPPED,
                     G_CALLBACK(onDiscoveryStopped), NULL);

    printf("[IoT] Provider initialized. Component='%s', %d elements under Device.IoT.*\n",
           BARTON_RBUS_COMPONENT_NAME, n);
    return true;
}

void barton_rbus_provider_cleanup(BartonRbusContext *ctx)
{
    if (!ctx) return;
    if (ctx->handle) { rbus_close(ctx->handle); ctx->handle = NULL; }
    s_ctx = NULL;
    printf("[IoT] Cleaned up.\n");
}
