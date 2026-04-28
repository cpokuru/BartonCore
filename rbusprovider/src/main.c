#include "barton_rbus_provider.h"
#include "rbus-network-credentials-provider.h"   /* <-- our own, no BartonCommon dep */
#include "barton-core-client.h"
#include "barton-core-initialize-params-container.h"
#include "barton-core-properties.h"
#include "provider/barton-core-property-provider.h"

#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_STORAGE_DIR "/tmp/barton-rbus"
#define DEFAULT_WIFI_SSID   "MySSID"
#define DEFAULT_WIFI_PASS   "MyPassword"

static GMainLoop *s_mainLoop = NULL;

static void signalHandler(int sig)
{
    (void)sig;
    printf("\n[BartonRbus] Signal caught, shutting down...\n");
    if (s_mainLoop) g_main_loop_quit(s_mainLoop);
}

static void setDefaultMatterParams(BCoreInitializeParamsContainer *params)
{
    BCorePropertyProvider *pp =
        b_core_initialize_params_container_get_property_provider(params);
    if (!pp) return;

    b_core_property_provider_set_property_string(pp,
        B_CORE_BARTON_MATTER_VENDOR_NAME,   "Barton");
    b_core_property_provider_set_property_uint16(pp,
        B_CORE_BARTON_MATTER_VENDOR_ID,     0xFFF1);
    b_core_property_provider_set_property_string(pp,
        B_CORE_BARTON_MATTER_PRODUCT_NAME,  "Barton Rbus Device");
    b_core_property_provider_set_property_uint16(pp,
        B_CORE_BARTON_MATTER_PRODUCT_ID,    0x8001);
    b_core_property_provider_set_property_uint16(pp,
        B_CORE_BARTON_MATTER_HARDWARE_VERSION, 1);
    b_core_property_provider_set_property_string(pp,
        B_CORE_BARTON_MATTER_HARDWARE_VERSION_STRING, "1.0");
    b_core_property_provider_set_property_string(pp,
        B_CORE_BARTON_MATTER_SERIAL_NUMBER, "SN-RBUS-001");

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char mfgDate[11];
    strftime(mfgDate, sizeof(mfgDate), "%Y-%m-%d", tm_info);
    b_core_property_provider_set_property_string(pp,
        B_CORE_BARTON_MATTER_MANUFACTURING_DATE, mfgDate);

    guint16 disc = b_core_property_provider_get_property_as_uint16(
        pp, B_CORE_BARTON_MATTER_SETUP_DISCRIMINATOR, 0);
    if (disc == 0)
        b_core_property_provider_set_property_uint16(pp,
            B_CORE_BARTON_MATTER_SETUP_DISCRIMINATOR, 3840);

    guint32 pass = b_core_property_provider_get_property_as_uint32(
        pp, B_CORE_BARTON_MATTER_SETUP_PASSCODE, 0);
    if (pass == 0)
        b_core_property_provider_set_property_uint32(pp,
            B_CORE_BARTON_MATTER_SETUP_PASSCODE, 20202021);
}

int main(int argc, char **argv)
{
    const char *storageDir = (argc > 1) ? argv[1] : DEFAULT_STORAGE_DIR;
    const char *wifiSsid   = (argc > 2) ? argv[2] : DEFAULT_WIFI_SSID;
    const char *wifiPass   = (argc > 3) ? argv[3] : DEFAULT_WIFI_PASS;

    printf("[BartonRbus] storageDir=%s wifi=%s\n", storageDir, wifiSsid);

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    /* ---- Init BartonCore ---- */
    g_autoptr(BCoreInitializeParamsContainer) params =
        b_core_initialize_params_container_new();

    b_core_initialize_params_container_set_storage_dir(params, storageDir);
    b_core_initialize_params_container_set_account_id(params, "1");

    g_autofree gchar *matterDir = g_strdup_printf("%s/matter", storageDir);
    g_mkdir_with_parents(matterDir, 0755);
    b_core_initialize_params_container_set_matter_storage_dir(params, matterDir);
    b_core_initialize_params_container_set_matter_attestation_trust_store_dir(
        params, matterDir);

    /* Our own credentials provider - no BartonCommon dependency */
    b_rbus_network_credentials_provider_set_wifi(wifiSsid, wifiPass);
    g_autoptr(BRbusNetworkCredentialsProvider) netCreds =
        b_rbus_network_credentials_provider_new();
    b_core_initialize_params_container_set_network_credentials_provider(
        params, B_CORE_NETWORK_CREDENTIALS_PROVIDER(netCreds));

    BCoreClient *client = b_core_client_new(params);
    setDefaultMatterParams(params);

    /* ---- Init rbus provider (before client start) ---- */
    BartonRbusContext rbusCtx = {0};
    if (!barton_rbus_provider_init(&rbusCtx, client))
    {
        fprintf(stderr, "[BartonRbus] rbus init failed\n");
        g_object_unref(client);
        return 1;
    }

    /* ---- Start BartonCore ---- */
    b_core_client_start(client);
    b_core_client_set_system_property(client, "deviceDescriptorBypass", "true");

    printf("[BartonRbus] Running. Press Ctrl+C to exit.\n");
    printf("[BartonRbus] Test with: rbuscli getv Barton.Status\n");

    s_mainLoop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(s_mainLoop);

    /* ---- Cleanup ---- */
    barton_rbus_provider_cleanup(&rbusCtx);
    b_core_client_stop(client);
    g_object_unref(client);
    g_main_loop_unref(s_mainLoop);

    printf("[BartonRbus] Done.\n");
    return 0;
}
