#include "barton_rbus_provider.h"
#include "barton-core-client.h"
#include "barton-core-device.h"
#include "barton-core-endpoint.h"
#include "barton-core-resource.h"
#include "barton-core-metadata.h"
#include "barton-core-status.h"
#include "barton-core-commissioning-info.h"
#include "events/barton-core-device-added-event.h"
#include "events/barton-core-device-removed-event.h"
#include "events/barton-core-resource-updated-event.h"
#include "events/barton-core-discovery-started-event.h"
#include "events/barton-core-discovery-stopped-event.h"

#include <rbus/rbus.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static BartonRbusContext *s_ctx = NULL;

/* ======================================================================
 * Utilities
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
 * Property GET
 * ====================================================================*/
static rbusError_t propGetHandler(rbusHandle_t handle,
                                   rbusProperty_t property,
                                   rbusGetHandlerOptions_t *options)
{
    (void)handle; (void)options;
    const char *name = rbusProperty_GetName(property);
    rbusValue_t val;
    rbusValue_Init(&val);

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
    return RBUS_ERROR_SUCCESS;
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
    printf("[IoT] %s: %s\n",
           action == RBUS_EVENT_ACTION_SUBSCRIBE ? "SUBSCRIBE" : "UNSUBSCRIBE",
           eventName);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.GetStatus()
 * Maps to: gs
 * in:  dummy(string) - required by rbuscli for no-arg methods
 * out: status(string) - full JSON identical to 'gs' output
 * test: rbuscli method_values "Device.IoT.GetStatus()" dummy string x
 * ====================================================================*/
static rbusError_t methodGetStatus(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)in; (void)async;

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
 * METHOD: Device.IoT.Discovery.Start()
 * Maps to: dstart <deviceClass> [setupCode]
 * in:  deviceClass(string), timeout(uint32), setupCode(string, optional)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Discovery.Start()" deviceClass string matter timeout uint32 60
 * ====================================================================*/
static rbusError_t methodDiscStart(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vClass     = rbusObject_GetValue(in, "deviceClass");
    rbusValue_t vTimeout   = rbusObject_GetValue(in, "timeout");
    rbusValue_t vSetupCode = rbusObject_GetValue(in, "setupCode");

    const char *dc        = vClass     ? rbusValue_GetString(vClass, NULL)    : "matter";
    uint32_t    to        = vTimeout   ? rbusValue_GetUInt32(vTimeout)         : 60;
    const char *setupCode = vSetupCode ? rbusValue_GetString(vSetupCode, NULL) : NULL;

    GList *classes = g_list_append(NULL, (gpointer)dc);

    /* setupCode goes into filters list if provided */
    GList *filters = NULL;
    if (setupCode && strlen(setupCode) > 0)
        filters = g_list_append(NULL, (gpointer)setupCode);

    GError *err = NULL;
    gboolean ok = b_core_client_discover_start(s_ctx->client,
                                               classes,
                                               filters,
                                               (guint16)to,
                                               &err);
    g_list_free(classes);
    if (filters) g_list_free(filters);
    if (err) g_error_free(err);
    if (ok) s_ctx->discoveryActive = true;

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Discovery.Stop()
 * Maps to: dstop
 * in:  dummy(string)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Discovery.Stop()" dummy string x
 * ====================================================================*/
static rbusError_t methodDiscStop(rbusHandle_t handle, const char *name,
                                   rbusObject_t in, rbusObject_t out,
                                   rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)in; (void)async;

    gboolean ok = b_core_client_discover_stop(s_ctx->client, NULL);
    if (ok) s_ctx->discoveryActive = false;

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Device.List()
 * Maps to: list / listDevices
 * in:  dummy(string) OR deviceClass(string, optional filter)
 * out: devices(string JSON array)
 * test: rbuscli method_values "Device.IoT.Device.List()" dummy string x
 * test: rbuscli method_values "Device.IoT.Device.List()" deviceClass string light
 * ====================================================================*/
static rbusError_t methodListDevices(rbusHandle_t handle, const char *name,
                                      rbusObject_t in, rbusObject_t out,
                                      rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vClass = rbusObject_GetValue(in, "deviceClass");
    const char *dc = vClass ? rbusValue_GetString(vClass, NULL) : NULL;

    GList *devices = b_core_client_get_devices(s_ctx->client);

    /* Filter by class if provided and not the dummy value */
    if (dc && strlen(dc) > 0 && strcmp(dc, "x") != 0)
    {
        GList *filtered = NULL;
        for (GList *it = devices; it != NULL; it = it->next)
        {
            g_autofree gchar *cls = NULL;
            g_object_get(G_OBJECT(it->data),
                B_CORE_DEVICE_PROPERTY_NAMES[B_CORE_DEVICE_PROP_DEVICE_CLASS], &cls, NULL);
            if (cls && strcmp(cls, dc) == 0)
                filtered = g_list_append(filtered, g_object_ref(it->data));
        }
        g_list_free_full(devices, g_object_unref);
        devices = filtered;
    }

    g_autofree gchar *json = devices_list_to_json(devices);
    g_list_free_full(devices, g_object_unref);

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, json);
    rbusObject_SetValue(out, "devices", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Device.Get()
 * Maps to: pd <uuid>
 * in:  deviceId(string)
 * out: device(string JSON)
 * test: rbuscli method_values "Device.IoT.Device.Get()" deviceId string 5b9021971ce8c157
 * ====================================================================*/
static rbusError_t methodGetDevice(rbusHandle_t handle, const char *name,
                                    rbusObject_t in, rbusObject_t out,
                                    rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vId = rbusObject_GetValue(in, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    BCoreDevice *dev = b_core_client_get_device_by_id(s_ctx->client,
                           rbusValue_GetString(vId, NULL));
    rbusValue_t v;
    rbusValue_Init(&v);
    if (dev)
    {
        g_autofree gchar *json = device_to_json(dev);
        rbusValue_SetString(v, json);
        g_object_unref(dev);
    }
    else
    {
        rbusValue_SetString(v, "{}");
    }
    rbusObject_SetValue(out, "device", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Device.Remove()
 * Maps to: rd <uuid>
 * in:  deviceId(string)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Device.Remove()" deviceId string 5b9021971ce8c157
 * ====================================================================*/
static rbusError_t methodRemoveDevice(rbusHandle_t handle, const char *name,
                                       rbusObject_t in, rbusObject_t out,
                                       rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vId = rbusObject_GetValue(in, "deviceId");
    if (!vId) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_remove_device(s_ctx->client,
                      rbusValue_GetString(vId, NULL));
    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Resource.Read()
 * Maps to: rr <uri>
 * in:  uri(string)
 * out: value(string)
 * test: rbuscli method_values "Device.IoT.Resource.Read()" uri string /5b9021971ce8c157/ep/1/r/isOn
 * ====================================================================*/
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

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, val ? val : "");
    rbusObject_SetValue(out, "value", v);
    rbusValue_Release(v);
    g_free(val);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Resource.Write()
 * Maps to: wr <uri> <value>
 * in:  uri(string), value(string)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Resource.Write()" uri string /9490bea86d5276b1/ep/1/r/isOn value string true
 * ====================================================================*/
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
    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Resource.Execute()
 * Maps to: er <uri> [payload]
 * in:  uri(string), payload(string, optional)
 * out: success(bool), response(string)
 * test: rbuscli method_values "Device.IoT.Resource.Execute()" uri string /5b9021971ce8c157/ep/1/r/identify payload string ""
 * ====================================================================*/
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
                      rbusValue_GetString(vUri, NULL),
                      payload,
                      &response);

    rbusValue_t vOk, vResp;
    rbusValue_Init(&vOk);
    rbusValue_Init(&vResp);
    rbusValue_SetBoolean(vOk,  ok ? true : false);
    rbusValue_SetString(vResp, response ? response : "");
    rbusObject_SetValue(out, "success",  vOk);
    rbusObject_SetValue(out, "response", vResp);
    rbusValue_Release(vOk);
    rbusValue_Release(vResp);
    free(response);
    return RBUS_ERROR_SUCCESS;
}

// ======================================================================
 // METHOD: Device.IoT.Resource.Query()
 // Maps to: qr <pattern>
 // in:  pattern(string) - e.g. DEVICEID/ep/1/r/isOn or use  wildcard
 // out: resources(string JSON array of {uri, value})
 // test: rbuscli method_values "Device.IoT.Resource.Query()" pattern string  isOn" 
//  ====================================================================
static rbusError_t methodQueryResources(rbusHandle_t handle, const char *name,
                                         rbusObject_t in, rbusObject_t out,
                                         rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vPattern = rbusObject_GetValue(in, "pattern");
    if (!vPattern) return RBUS_ERROR_INVALID_INPUT;

    GList *resources = b_core_client_query_resources_by_uri(s_ctx->client,
                           rbusValue_GetString(vPattern, NULL));

    GString *sb = g_string_new("[");
    bool first = true;
    for (GList *it = resources; it != NULL; it = it->next)
    {
        BCoreResource *res = B_CORE_RESOURCE(it->data);
        g_autofree gchar *uri   = NULL;
        g_autofree gchar *value = NULL;
        g_object_get(G_OBJECT(res),
            B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_URI],   &uri,
            B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_VALUE], &value,
            NULL);
        if (!first) g_string_append(sb, ",");
        g_string_append_printf(sb, "{\"uri\":\"%s\",\"value\":\"%s\"}",
                               uri   ? uri   : "",
                               value ? value : "");
        first = false;
    }
    g_string_append(sb, "]");
    g_autofree gchar *json = g_string_free(sb, FALSE);
    g_list_free_full(resources, g_object_unref);

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, json);
    rbusObject_SetValue(out, "resources", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Metadata.Read()
 * Maps to: rm <uri>
 * in:  uri(string)
 * out: value(string)
 * test: rbuscli method_values "Device.IoT.Metadata.Read()" uri string /5b9021971ce8c157/m/lpmPolicy
 * ====================================================================*/
static rbusError_t methodReadMetadata(rbusHandle_t handle, const char *name,
                                       rbusObject_t in, rbusObject_t out,
                                       rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vUri = rbusObject_GetValue(in, "uri");
    if (!vUri) return RBUS_ERROR_INVALID_INPUT;

    GError *merr = NULL;
    gchar *val = b_core_client_read_metadata(s_ctx->client,
                     rbusValue_GetString(vUri, NULL), &merr);
    if (merr) g_error_free(merr);

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, val ? val : "");
    rbusObject_SetValue(out, "value", v);
    rbusValue_Release(v);
    g_free(val);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Metadata.Write()
 * Maps to: wm <uri> <value>
 * in:  uri(string), value(string)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Metadata.Write()" uri string /5b9021971ce8c157/m/lpmPolicy value string never
 * ====================================================================*/
static rbusError_t methodWriteMetadata(rbusHandle_t handle, const char *name,
                                        rbusObject_t in, rbusObject_t out,
                                        rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vUri = rbusObject_GetValue(in, "uri");
    rbusValue_t vVal = rbusObject_GetValue(in, "value");
    if (!vUri || !vVal) return RBUS_ERROR_INVALID_INPUT;

    gboolean ok = b_core_client_write_metadata(s_ctx->client,
                      rbusValue_GetString(vUri, NULL),
                      rbusValue_GetString(vVal, NULL));
    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.GetProperty()
 * Maps to: gp <key>
 * in:  key(string)
 * out: value(string)
 * test: rbuscli method_values "Device.IoT.GetProperty()" key string deviceDescriptorBypass
 * ====================================================================*/
static rbusError_t methodGetProperty(rbusHandle_t handle, const char *name,
                                      rbusObject_t in, rbusObject_t out,
                                      rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vKey = rbusObject_GetValue(in, "key");
    if (!vKey) return RBUS_ERROR_INVALID_INPUT;

    gchar *val = b_core_client_get_system_property(s_ctx->client,
                     rbusValue_GetString(vKey, NULL));
    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetString(v, val ? val : "");
    rbusObject_SetValue(out, "value", v);
    rbusValue_Release(v);
    g_free(val);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.SetProperty()
 * Maps to: sp <key> <value>
 * in:  key(string), value(string)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.SetProperty()" key string deviceDescriptorBypass value string true
 * ====================================================================*/
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
    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Matter.Commission()
 * Maps to: cd <setupPayload>
 * in:  setupPayload(string), timeout(uint32)
 * out: success(bool)
 * test: rbuscli method_values "Device.IoT.Matter.Commission()" setupPayload string MT:XXXXX timeout uint32 120
 * ====================================================================*/
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
                      (gchar *)rbusValue_GetString(vPayload, NULL),
                      (guint16)to, &err);
    if (err) g_error_free(err);

    rbusValue_t v;
    rbusValue_Init(&v);
    rbusValue_SetBoolean(v, ok ? true : false);
    rbusObject_SetValue(out, "success", v);
    rbusValue_Release(v);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * METHOD: Device.IoT.Matter.OpenCommissioningWindow()
 * Maps to: ocw <nodeId> [timeout]
 * in:  nodeId(string, use "0" for local), timeout(uint32)
 * out: manualCode(string), qrCode(string)
 * test: rbuscli method_values "Device.IoT.Matter.OpenCommissioningWindow()" nodeId string 0 timeout uint32 180
 * ====================================================================*/
static rbusError_t methodOpenCommWindow(rbusHandle_t handle, const char *name,
                                         rbusObject_t in, rbusObject_t out,
                                         rbusMethodAsyncHandle_t async)
{
    (void)handle; (void)name; (void)async;

    rbusValue_t vNodeId  = rbusObject_GetValue(in, "nodeId");
    rbusValue_t vTimeout = rbusObject_GetValue(in, "timeout");
    const char *nodeId = vNodeId  ? rbusValue_GetString(vNodeId, NULL) : "0";
    uint32_t    to     = vTimeout ? rbusValue_GetUInt32(vTimeout)      : 180;

    BCoreCommissioningInfo *info =
        b_core_client_open_commissioning_window(s_ctx->client, nodeId, (guint16)to);

    rbusValue_t vManual, vQr;
    rbusValue_Init(&vManual);
    rbusValue_Init(&vQr);

    if (info)
    {
        g_autofree gchar *manual = NULL;
        g_autofree gchar *qr     = NULL;
        g_object_get(G_OBJECT(info),
            B_CORE_COMMISSIONING_INFO_PROPERTY_NAMES[B_CORE_COMMISSIONING_INFO_PROP_MANUAL_CODE], &manual,
            B_CORE_COMMISSIONING_INFO_PROPERTY_NAMES[B_CORE_COMMISSIONING_INFO_PROP_QR_CODE],     &qr,
            NULL);
        rbusValue_SetString(vManual, manual ? manual : "");
        rbusValue_SetString(vQr,     qr     ? qr     : "");
        g_object_unref(info);
    }
    else
    {
        rbusValue_SetString(vManual, "");
        rbusValue_SetString(vQr,     "");
    }
    rbusObject_SetValue(out, "manualCode", vManual);
    rbusObject_SetValue(out, "qrCode",     vQr);
    rbusValue_Release(vManual);
    rbusValue_Release(vQr);
    return RBUS_ERROR_SUCCESS;
}

/* ======================================================================
 * GObject signal -> rbus event publishers
 * ====================================================================*/
static void onDeviceAdded(BCoreClient *src, BCoreDeviceAddedEvent *event, gpointer ud)
{
    (void)src; (void)ud;
    g_autofree gchar *deviceId    = NULL;
    g_autofree gchar *uri         = NULL;
    g_autofree gchar *deviceClass = NULL;
    guint classVersion = 0;

    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_UUID],                 &deviceId,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_URI],                  &uri,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS],         &deviceClass,
        B_CORE_DEVICE_ADDED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_ADDED_EVENT_PROP_DEVICE_CLASS_VERSION], &classVersion,
        NULL);

    printf("[IoT] DeviceAdded: id=%s class=%s\n",
           deviceId ? deviceId : "", deviceClass ? deviceClass : "");

    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbusValue_t vId, vUri, vClass, vVer;
    rbusValue_Init(&vId);
    rbusValue_Init(&vUri);
    rbusValue_Init(&vClass);
    rbusValue_Init(&vVer);
    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_SetString(vUri,   uri         ? uri         : "");
    rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusValue_SetUInt32(vVer,   classVersion);
    rbusObject_SetValue(data, "deviceId",     vId);
    rbusObject_SetValue(data, "uri",          vUri);
    rbusObject_SetValue(data, "deviceClass",  vClass);
    rbusObject_SetValue(data, "classVersion", vVer);
    rbusValue_Release(vId);
    rbusValue_Release(vUri);
    rbusValue_Release(vClass);
    rbusValue_Release(vVer);
    rbusEvent_t ev_barton_rbus_evt_device_added = {0};
    ev_barton_rbus_evt_device_added.name = BARTON_RBUS_EVT_DEVICE_ADDED;
    ev_barton_rbus_evt_device_added.data = data;
    ev_barton_rbus_evt_device_added.type = RBUS_EVENT_GENERAL;
    rbusEvent_Publish(s_ctx->handle, &ev_barton_rbus_evt_device_added);
    rbusObject_Release(data);
}

static void onDeviceRemoved(BCoreClient *src, BCoreDeviceRemovedEvent *event, gpointer ud)
{
    (void)src; (void)ud;
    g_autofree gchar *deviceId    = NULL;
    g_autofree gchar *deviceClass = NULL;

    g_object_get(G_OBJECT(event),
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_UUID],  &deviceId,
        B_CORE_DEVICE_REMOVED_EVENT_PROPERTY_NAMES[B_CORE_DEVICE_REMOVED_EVENT_PROP_DEVICE_CLASS], &deviceClass,
        NULL);

    printf("[IoT] DeviceRemoved: id=%s\n", deviceId ? deviceId : "");

    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbusValue_t vId, vClass;
    rbusValue_Init(&vId);
    rbusValue_Init(&vClass);
    rbusValue_SetString(vId,    deviceId    ? deviceId    : "");
    rbusValue_SetString(vClass, deviceClass ? deviceClass : "");
    rbusObject_SetValue(data, "deviceId",    vId);
    rbusObject_SetValue(data, "deviceClass", vClass);
    rbusValue_Release(vId);
    rbusValue_Release(vClass);
    rbusEvent_t ev_barton_rbus_evt_device_removed = {0};
    ev_barton_rbus_evt_device_removed.name = BARTON_RBUS_EVT_DEVICE_REMOVED;
    ev_barton_rbus_evt_device_removed.data = data;
    ev_barton_rbus_evt_device_removed.type = RBUS_EVENT_GENERAL;
    rbusEvent_Publish(s_ctx->handle, &ev_barton_rbus_evt_device_removed);
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

    g_autofree gchar *uri   = NULL;
    g_autofree gchar *value = NULL;
    g_object_get(G_OBJECT(resource),
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_URI],   &uri,
        B_CORE_RESOURCE_PROPERTY_NAMES[B_CORE_RESOURCE_PROP_VALUE], &value,
        NULL);

    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbusValue_t vUri, vVal;
    rbusValue_Init(&vUri);
    rbusValue_Init(&vVal);
    rbusValue_SetString(vUri, uri   ? uri   : "");
    rbusValue_SetString(vVal, value ? value : "");
    rbusObject_SetValue(data, "uri",   vUri);
    rbusObject_SetValue(data, "value", vVal);
    rbusValue_Release(vUri);
    rbusValue_Release(vVal);
    rbusEvent_t ev_barton_rbus_evt_resource_updated = {0};
    ev_barton_rbus_evt_resource_updated.name = BARTON_RBUS_EVT_RESOURCE_UPDATED;
    ev_barton_rbus_evt_resource_updated.data = data;
    ev_barton_rbus_evt_resource_updated.type = RBUS_EVENT_GENERAL;
    rbusEvent_Publish(s_ctx->handle, &ev_barton_rbus_evt_resource_updated);
    rbusObject_Release(data);
}

static void onDiscoveryStarted(BCoreClient *src, BCoreDiscoveryStartedEvent *event, gpointer ud)
{
    (void)src; (void)event; (void)ud;
    s_ctx->discoveryActive = true;
    printf("[IoT] DiscoveryStarted\n");
    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbusEvent_t ev_barton_rbus_evt_disc_started = {0};
    ev_barton_rbus_evt_disc_started.name = BARTON_RBUS_EVT_DISC_STARTED;
    ev_barton_rbus_evt_disc_started.data = data;
    ev_barton_rbus_evt_disc_started.type = RBUS_EVENT_GENERAL;
    rbusEvent_Publish(s_ctx->handle, &ev_barton_rbus_evt_disc_started);
    rbusObject_Release(data);
}

static void onDiscoveryStopped(BCoreClient *src, BCoreDiscoveryStoppedEvent *event, gpointer ud)
{
    (void)src; (void)event; (void)ud;
    s_ctx->discoveryActive = false;
    printf("[IoT] DiscoveryStopped\n");
    rbusObject_t data;
    rbusObject_Init(&data, NULL);
    rbusEvent_t ev_barton_rbus_evt_disc_stopped = {0};
    ev_barton_rbus_evt_disc_stopped.name = BARTON_RBUS_EVT_DISC_STOPPED;
    ev_barton_rbus_evt_disc_stopped.data = data;
    ev_barton_rbus_evt_disc_stopped.type = RBUS_EVENT_GENERAL;
    rbusEvent_Publish(s_ctx->handle, &ev_barton_rbus_evt_disc_stopped);
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
    if (rc != RBUS_ERROR_SUCCESS)
    {
        fprintf(stderr, "[IoT] rbus_open failed: %d\n", rc);
        return false;
    }

    rbusDataElement_t elements[] = {
        /* Properties */
        {BARTON_RBUS_PROP_STATUS,       RBUS_ELEMENT_TYPE_PROPERTY, {propGetHandler,NULL,NULL,NULL,NULL,NULL}},
        {BARTON_RBUS_PROP_DEVICE_COUNT, RBUS_ELEMENT_TYPE_PROPERTY, {propGetHandler,NULL,NULL,NULL,NULL,NULL}},
        {BARTON_RBUS_PROP_DEVICES_JSON, RBUS_ELEMENT_TYPE_PROPERTY, {propGetHandler,NULL,NULL,NULL,NULL,NULL}},
        {BARTON_RBUS_PROP_DISC_ACTIVE,  RBUS_ELEMENT_TYPE_PROPERTY, {propGetHandler,NULL,NULL,NULL,NULL,NULL}},

        /* Methods */
        {BARTON_RBUS_METHOD_GET_STATUS,      RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodGetStatus}},
        {BARTON_RBUS_METHOD_DISC_START,      RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodDiscStart}},
        {BARTON_RBUS_METHOD_DISC_STOP,       RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodDiscStop}},
        {BARTON_RBUS_METHOD_LIST_DEVICES,    RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodListDevices}},
        {BARTON_RBUS_METHOD_GET_DEVICE,      RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodGetDevice}},
        {BARTON_RBUS_METHOD_REMOVE_DEVICE,   RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodRemoveDevice}},
        {BARTON_RBUS_METHOD_READ_RESOURCE,   RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodReadResource}},
        {BARTON_RBUS_METHOD_WRITE_RESOURCE,  RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodWriteResource}},
        {BARTON_RBUS_METHOD_EXEC_RESOURCE,   RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodExecResource}},
        {BARTON_RBUS_METHOD_QUERY_RESOURCES, RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodQueryResources}},
        {BARTON_RBUS_METHOD_READ_METADATA,   RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodReadMetadata}},
        {BARTON_RBUS_METHOD_WRITE_METADATA,  RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodWriteMetadata}},
        {BARTON_RBUS_METHOD_GET_PROPERTY,    RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodGetProperty}},
        {BARTON_RBUS_METHOD_SET_PROPERTY,    RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodSetProperty}},
        {BARTON_RBUS_METHOD_COMMISSION,      RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodCommission}},
        {BARTON_RBUS_METHOD_OPEN_COMM_WIN,   RBUS_ELEMENT_TYPE_METHOD, {NULL,NULL,NULL,NULL,NULL,methodOpenCommWindow}},

        /* Events */
        {BARTON_RBUS_EVT_DEVICE_ADDED,     RBUS_ELEMENT_TYPE_EVENT, {NULL,NULL,NULL,NULL,eventSubHandler,NULL}},
        {BARTON_RBUS_EVT_DEVICE_REMOVED,   RBUS_ELEMENT_TYPE_EVENT, {NULL,NULL,NULL,NULL,eventSubHandler,NULL}},
        {BARTON_RBUS_EVT_RESOURCE_UPDATED, RBUS_ELEMENT_TYPE_EVENT, {NULL,NULL,NULL,NULL,eventSubHandler,NULL}},
        {BARTON_RBUS_EVT_DISC_STARTED,     RBUS_ELEMENT_TYPE_EVENT, {NULL,NULL,NULL,NULL,eventSubHandler,NULL}},
        {BARTON_RBUS_EVT_DISC_STOPPED,     RBUS_ELEMENT_TYPE_EVENT, {NULL,NULL,NULL,NULL,eventSubHandler,NULL}},
    };

    int n = (int)(sizeof(elements) / sizeof(elements[0]));
    rc = rbus_regDataElements(ctx->handle, n, elements);
    if (rc != RBUS_ERROR_SUCCESS)
    {
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

    printf("[IoT] Provider initialized. %d elements registered under Device.IoT.*\n", n);
    return true;
}

void barton_rbus_provider_cleanup(BartonRbusContext *ctx)
{
    if (!ctx) return;
    if (ctx->handle) { rbus_close(ctx->handle); ctx->handle = NULL; }
    s_ctx = NULL;
    printf("[IoT] Cleaned up.\n");
}
