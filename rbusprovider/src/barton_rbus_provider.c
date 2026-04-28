#include "barton_rbus_provider.h"

#include "barton-core-client.h"
#include "barton-core-device.h"
#include "barton-core-endpoint.h"
#include "barton-core-resource.h"
#include "events/barton-core-device-added-event.h"
#include "events/barton-core-device-removed-event.h"
#include "events/barton-core-resource-updated-event.h"
#include "events/barton-core-discovery-started-event.h"
#include "events/barton-core-discovery-stopped-event.h"

#include <rbus/rbus.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static BartonRbusContext *s_ctx = NULL;

/* ======================================================================
 * Utility: BCoreDevice → JSON string (caller must g_free)
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
        g_autofree gchar *devJson = device_to_json(B_CORE_DEVICE(it->data));
        if (!first) g_string_append(sb, ",");
        g_string_append(sb, devJson);
        first = false;
    }
    g_string_append(sb, "]");
    return g_string_free(sb, FALSE);
}

/* ======================================================================
 * Property GET handler
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
 * Event subscription handler
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
    printf("[BartonRbus] %s: %s\n",
           action == RBUS_EVENT_ACTION_SUBSCRIBE ? "SUBSCRIBE" : "UNSUBSCRIBE",
           eventName);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * Method handlers
 * ====================================================================*/

/* Barton.DiscoverStart()  in: deviceClass(str), timeout(uint32)  out: success(bool) */
static rbusError_t methodDiscoverStart(rbusHandle_t handle,
                                        const char *methodName,
                                        rbusObject_t inParams,
                                        rbusObject_t outParams,
                                        rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vClass   = rbusObject_GetValue(inParams, "deviceClass");
    rbusValue_t vTimeout = rbusObject_GetValue(inParams, "timeout");

    const char *deviceClass = vClass   ? rbusValue_GetString(vClass, NULL) : "matter";
    uint32_t    timeout     = vTimeout ? rbusValue_GetUInt32(vTimeout)      : 60;

    GList *classes = g_list_append(NULL, (gpointer)deviceClass);
    GError *err = NULL;
    gboolean ok = b_core_client_discover_start(s_ctx->client, classes, NULL,
                                               (guint16)timeout, &err);
    g_list_free(classes);
    if (err) g_error_free(err);

    rbusValue_t result;
    rbusValue_Init(&result);
    rbusValue_SetBoolean(result, ok ? true : false);
    rbusObject_SetValue(outParams, "success", result);
    rbusValue_Release(result);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.DiscoverStop()  in: deviceClass(str, optional)  out: success(bool) */
static rbusError_t methodDiscoverStop(rbusHandle_t handle,
                                       const char *methodName,
                                       rbusObject_t inParams,
                                       rbusObject_t outParams,
                                       rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vClass = rbusObject_GetValue(inParams, "deviceClass");
    GList *classes = NULL;
    if (vClass)
        classes = g_list_append(NULL, (gpointer)rbusValue_GetString(vClass, NULL));

    gboolean ok = b_core_client_discover_stop(s_ctx->client, classes);
    if (classes) g_list_free(classes);

    rbusValue_t result;
    rbusValue_Init(&result);
    rbusValue_SetBoolean(result, ok ? true : false);
    rbusObject_SetValue(outParams, "success", result);
    rbusValue_Release(result);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.GetDevices()  out: devices(str, JSON array) */
static rbusError_t methodGetDevices(rbusHandle_t handle,
                                     const char *methodName,
                                     rbusObject_t inParams,
                                     rbusObject_t outParams,
                                     rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)inParams; (void)asyncHandle;

    GList *devices = b_core_client_get_devices(s_ctx->client);
    g_autofree gchar *json = devices_list_to_json(devices);
    g_list_free_full(devices, g_object_unref);

    rbusValue_t val;
    rbusValue_Init(&val);
    rbusValue_SetString(val, json);
    rbusObject_SetValue(outParams, "devices", val);
    rbusValue_Release(val);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.GetDevice()  in: deviceId(str)  out: device(str, JSON) */
static rbusError_t methodGetDevice(rbusHandle_t handle,
                                    const char *methodName,
                                    rbusObject_t inParams,
                                    rbusObject_t outParams,
                                    rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vId = rbusObject_GetValue(inParams, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    BCoreDevice *dev = b_core_client_get_device_by_id(s_ctx->client,
                           rbusValue_GetString(vId, NULL));
    rbusValue_t val;
    rbusValue_Init(&val);
    if (dev)
    {
        g_autofree gchar *json = device_to_json(dev);
        rbusValue_SetString(val, json);
        g_object_unref(dev);
    }
    else
    {
        rbusValue_SetString(val, "{}");
    }
    rbusObject_SetValue(outParams, "device", val);
    rbusValue_Release(val);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.RemoveDevice()  in: deviceId(str)  out: success(bool) */
static rbusError_t methodRemoveDevice(rbusHandle_t handle,
                                       const char *methodName,
                                       rbusObject_t inParams,
                                       rbusObject_t outParams,
                                       rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vId = rbusObject_GetValue(inParams, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_remove_device(s_ctx->client,
                      rbusValue_GetString(vId, NULL));

    rbusValue_t val;
    rbusValue_Init(&val);
    rbusValue_SetBoolean(val, ok ? true : false);
    rbusObject_SetValue(outParams, "success", val);
    rbusValue_Release(val);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.ReadResource()  in: uri(str)  out: value(str) */
static rbusError_t methodReadResource(rbusHandle_t handle,
                                       const char *methodName,
                                       rbusObject_t inParams,
                                       rbusObject_t outParams,
                                       rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vUri = rbusObject_GetValue(inParams, "uri");
    if (!vUri) return RBUS_ERROR_INVALID_INPUT;

    GError *err = NULL;
    gchar *value = b_core_client_read_resource(s_ctx->client,
                       rbusValue_GetString(vUri, NULL), &err);
    if (err) g_error_free(err);

    rbusValue_t val;
    rbusValue_Init(&val);
    rbusValue_SetString(val, value ? value : "");
    rbusObject_SetValue(outParams, "value", val);
    rbusValue_Release(val);
    g_free(value);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.WriteResource()  in: uri(str), value(str)  out: success(bool) */
static rbusError_t methodWriteResource(rbusHandle_t handle,
                                        const char *methodName,
                                        rbusObject_t inParams,
                                        rbusObject_t outParams,
                                        rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vUri   = rbusObject_GetValue(inParams, "uri");
    rbusValue_t vValue = rbusObject_GetValue(inParams, "value");
    if (!vUri || !vValue) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_write_resource(s_ctx->client,
                      rbusValue_GetString(vUri, NULL),
                      rbusValue_GetString(vValue, NULL));

    rbusValue_t val;
    rbusValue_Init(&val);
    rbusValue_SetBoolean(val, ok ? true : false);
    rbusObject_SetValue(outParams, "success", val);
    rbusValue_Release(val);
    return RBUS_ERROR_SUCCESS;
}

/* Barton.CommissionDevice()  in: setupPayload(str), timeout(uint32)  out: success(bool) */
static rbusError_t methodCommissionDevice(rbusHandle_t handle,
                                           const char *methodName,
                                           rbusObject_t inParams,
                                           rbusObject_t outParams,
                                           rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle; (void)methodName; (void)asyncHandle;

    rbusValue_t vPayload = rbusObject_GetValue(inParams, "setupPayload");
    rbusValue_t vTimeout = rbusObject_GetValue(inParams, "timeout");
    if (!vPayload) return RBUS_ERROR_INVALID_INPUT;

    uint32_t timeout = vTimeout ? rbusValue_GetUInt32(vTimeout) : 120;
    GError *err = NULL;
    gboolean ok = b_core_client_commission_device(s_ctx->client,
                      (gchar *)rbusValue_GetString(vPayload, NULL),
                      (guint16)timeout, &err);
    if (err) g_error_free(err);

    rbusValue_t val;
    rbusValue_Init(&val);
    rbusValue_SetBoolean(val, ok ? true : false);
    rbusObject_SetValue(outParams, "success", val);
    rbusValue_Release(val);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * GObject signal handlers → publish rbus events
 * ====================================================================*/

static void onDeviceAdded(BCoreClient *source, BCoreDeviceAddedEvent *event,
                           gpointer userData)
{
    (void)source; (void)userData;

    g_autofree gchar *deviceId    = NULL;
    g_autofree gchar *uri         = NULL;
    g_autofree gchar *deviceClass = NULL;
    guint classVersion = 0;

    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_UUID],
        &deviceId,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_URI],
        &uri,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS],
        &deviceClass,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS_VERSION],
        &classVersion,
        NULL);

    printf("[BartonRbus] DeviceAdded: id=%s class=%s uri=%s\n",
           deviceId ? deviceId : "",
           deviceClass ? deviceClass : "",
           uri ? uri : "");

    rbusObject_t data;
    rbusObject_Init(&data, NULL);

    rbusValue_t vId, vUri, vClass, vVer;
    rbusValue_Init(&vId);    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_Init(&vUri);   rbusValue_SetString(vUri,   uri         ? uri         : "");
    rbusValue_Init(&vClass); rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusValue_Init(&vVer);   rbusValue_SetUInt32(vVer,   classVersion);

    rbusObject_SetValue(data, "deviceId",     vId);
    rbusObject_SetValue(data, "uri",          vUri);
    rbusObject_SetValue(data, "deviceClass",  vClass);
    rbusObject_SetValue(data, "classVersion", vVer);

    rbusValue_Release(vId);
    rbusValue_Release(vUri);
    rbusValue_Release(vClass);
    rbusValue_Release(vVer);

    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DEVICE_ADDED, data);
    rbusObject_Release(data);
}

static void onDeviceRemoved(BCoreClient *source, BCoreDeviceRemovedEvent *event,
                             gpointer userData)
{
    (void)source; (void)userData;

    g_autofree gchar *deviceId    = NULL;
    g_autofree gchar *deviceClass = NULL;

    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_UUID],
        &deviceId,
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_CLASS],
        &deviceClass,
        NULL);

    printf("[BartonRbus] DeviceRemoved: id=%s class=%s\n",
           deviceId ? deviceId : "", deviceClass ? deviceClass : "");

    rbusObject_t data;
    rbusObject_Init(&data, NULL);

    rbusValue_t vId, vClass;
    rbusValue_Init(&vId);    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_Init(&vClass); rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusObject_SetValue(data, "deviceId",    vId);
    rbusObject_SetValue(data, "deviceClass", vClass);
    rbusValue_Release(vId);
    rbusValue_Release(vClass);

    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DEVICE_REMOVED, data);
    rbusObject_Release(data);
}

static void onResourceUpdated(BCoreClient *source, BCoreResourceUpdatedEvent *event,
                               gpointer userData)
{
    (void)source; (void)userData;

    g_autoptr(BCoreResource) resource = NULL;
    g_object_get(G_OBJECT(event),
        B_CORE_RESOURCE_UPDATED_EVENT_PROPERTY_NAMES[B_CORE_RESOURCE_UPDATED_EVENT_PROP_RESOURCE],
        &resource, NULL);
    if (!resource) return;

    g_autofree gchar *uri   = NULL;
    g_autofree gchar *value = NULL;
    g_object_get(G_OBJECT(resource),
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_URI],   &uri,
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_VALUE], &value,
        NULL);

    rbusObject_t data;
    rbusObject_Init(&data, NULL);

    rbusValue_t vUri, vVal;
    rbusValue_Init(&vUri); rbusValue_SetString(vUri, uri   ? uri   : "");
    rbusValue_Init(&vVal); rbusValue_SetString(vVal, value ? value : "");
    rbusObject_SetValue(data, "uri",   vUri);
    rbusObject_SetValue(data, "value", vVal);
    rbusValue_Release(vUri);
    rbusValue_Release(vVal);

    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_RESOURCE_UPDATED, data);
    rbusObject_Release(data);
}

static void onDiscoveryStarted(BCoreClient *source, BCoreDiscoveryStartedEvent *event,
                                gpointer userData)
{
    (void)source; (void)event; (void)userData;
    printf("[BartonRbus] DiscoveryStarted\n");
    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DISC_STARTED, data);
    rbusObject_Release(data);
}

static void onDiscoveryStopped(BCoreClient *source, BCoreDiscoveryStoppedEvent *event,
                                gpointer userData)
{
    (void)source; (void)event; (void)userData;
    printf("[BartonRbus] DiscoveryStopped\n");
    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbus_publishEvent(s_ctx->handle, BARTON_RBUS_EVT_DISC_STOPPED, data);
    rbusObject_Release(data);
}

/* ======================================================================
 * Public API
 * ====================================================================*/

bool barton_rbus_provider_init(BartonRbusContext *ctx, BCoreClient *client)
{
    ctx->client = client;
    s_ctx = ctx;

    rbusError_t rc = rbus_open(&ctx->handle, BARTON_RBUS_COMPONENT_NAME);
    if (rc != RBUS_ERROR_SUCCESS)
    {
        fprintf(stderr, "[BartonRbus] rbus_open failed: %d\n", rc);
        return false;
    }

    rbusDataElement_t elements[] = {
        /* --- Properties --- */
        {BARTON_RBUS_PROP_STATUS,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        {BARTON_RBUS_PROP_DEVICE_COUNT,
         RBUS_ELEMENT_TYPE_PROPERTY,
         {propGetHandler, NULL, NULL, NULL, NULL, NULL}},

        /* --- Methods --- */
        {BARTON_RBUS_METHOD_DISCOVER_START,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodDiscoverStart}},

        {BARTON_RBUS_METHOD_DISCOVER_STOP,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodDiscoverStop}},

        {BARTON_RBUS_METHOD_GET_DEVICES,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodGetDevices}},

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

        {BARTON_RBUS_METHOD_COMMISSION_DEVICE,
         RBUS_ELEMENT_TYPE_METHOD,
         {NULL, NULL, NULL, NULL, NULL, methodCommissionDevice}},

        /* --- Events --- */
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

    int numElements = (int)(sizeof(elements) / sizeof(elements[0]));
    rc = rbus_regDataElements(ctx->handle, numElements, elements);
    if (rc != RBUS_ERROR_SUCCESS)
    {
        fprintf(stderr, "[BartonRbus] rbus_regDataElements failed: %d\n", rc);
        rbus_close(ctx->handle);
        return false;
    }

    /* Connect BartonCore signals → rbus event publishers */
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

    printf("[BartonRbus] Provider initialized. %d elements registered.\n", numElements);
    return true;
}

void barton_rbus_provider_cleanup(BartonRbusContext *ctx)
{
    if (!ctx) return;
    if (ctx->handle)
    {
        rbus_close(ctx->handle);
        ctx->handle = NULL;
    }
    s_ctx = NULL;
    printf("[BartonRbus] Cleaned up.\n");
}
