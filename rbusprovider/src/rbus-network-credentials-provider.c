#include "rbus-network-credentials-provider.h"
#include "provider/barton-core-network-credentials-provider.h"
#include <glib.h>
#include <pthread.h>
#include <stdio.h>

struct _BRbusNetworkCredentialsProvider
{
    GObject parent_instance;
};

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static gchar *s_ssid     = NULL;
static gchar *s_password = NULL;

static void b_rbus_network_credentials_provider_iface_init(
    BCoreNetworkCredentialsProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(BRbusNetworkCredentialsProvider,
                        b_rbus_network_credentials_provider,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(
                            B_CORE_NETWORK_CREDENTIALS_PROVIDER_TYPE,
                            b_rbus_network_credentials_provider_iface_init))

static BCoreWifiNetworkCredentials *
get_wifi_network_credentials(BCoreNetworkCredentialsProvider *self,
                              GError **error)
{
    (void)self;
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    g_autoptr(BCoreWifiNetworkCredentials) creds =
        b_core_wifi_network_credentials_new();

    pthread_mutex_lock(&s_mtx);
    if (s_ssid != NULL && s_password != NULL)
    {
        g_object_set(creds,
            B_CORE_WIFI_NETWORK_CREDENTIALS_PROPERTY_NAMES
                [B_CORE_WIFI_NETWORK_CREDENTIALS_PROP_SSID], s_ssid,
            B_CORE_WIFI_NETWORK_CREDENTIALS_PROPERTY_NAMES
                [B_CORE_WIFI_NETWORK_CREDENTIALS_PROP_PSK],  s_password,
            NULL);
    }
    else
    {
        fprintf(stderr,
            "[BartonRbus] WARNING: No Wi-Fi credentials set. "
            "Call b_rbus_network_credentials_provider_set_wifi() first.\n");
    }
    pthread_mutex_unlock(&s_mtx);

    return g_steal_pointer(&creds);
}

static void b_rbus_network_credentials_provider_iface_init(
    BCoreNetworkCredentialsProviderInterface *iface)
{
    iface->get_wifi_network_credentials = get_wifi_network_credentials;
}

static void b_rbus_network_credentials_provider_init(
    BRbusNetworkCredentialsProvider *self)
{
    (void)self;
}

static void b_rbus_network_credentials_provider_class_init(
    BRbusNetworkCredentialsProviderClass *klass)
{
    (void)klass;
}

void b_rbus_network_credentials_provider_set_wifi(const gchar *ssid,
                                                   const gchar *password)
{
    g_return_if_fail(ssid != NULL);
    g_return_if_fail(password != NULL);

    pthread_mutex_lock(&s_mtx);
    g_free(s_ssid);
    g_free(s_password);
    s_ssid     = g_strdup(ssid);
    s_password = g_strdup(password);
    pthread_mutex_unlock(&s_mtx);
}

BRbusNetworkCredentialsProvider *b_rbus_network_credentials_provider_new(void)
{
    return B_RBUS_NETWORK_CREDENTIALS_PROVIDER(
        g_object_new(B_RBUS_NETWORK_CREDENTIALS_PROVIDER_TYPE, NULL));
}
