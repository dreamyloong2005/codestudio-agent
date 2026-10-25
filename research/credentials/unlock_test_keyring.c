/* Private gnome-keyring test fixture only. Never shipped or linked into Agent. */
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *marker = getenv("CODESTUDIO_TEST_PRIVATE_SECRET_SERVICE");
    const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (argc != 2 || !marker || strcmp(marker, "1") || !bus || strncmp(bus, "unix:", 5) ||
        strncmp(argv[1], "/org/freedesktop/secrets/collection/", 35)) return 2;
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection) return 3;
    GVariant *opened = g_dbus_connection_call_sync(connection, "org.freedesktop.secrets",
        "/org/freedesktop/secrets", "org.freedesktop.Secret.Service", "OpenSession",
        g_variant_new("(sv)", "plain", g_variant_new_string("")), G_VARIANT_TYPE("(vo)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!opened) return 4;
    GVariant *path = g_variant_get_child_value(opened, 1);
    const char password[] = "codestudio-test-keyring-password";
    GVariant *secret = g_variant_new("(o@ay@ays)", g_variant_get_string(path, NULL),
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0, 1),
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, password, strlen(password), 1), "text/plain");
    GVariant *unlocked = g_dbus_connection_call_sync(connection, "org.freedesktop.secrets",
        "/org/freedesktop/secrets", "org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface",
        "UnlockWithMasterPassword", g_variant_new("(o@(oayays))", argv[1], secret), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!unlocked) {
        g_printerr("Private test-keyring unlock failed: %s\n", error ? error->message : "unknown");
        return 5;
    }
    g_variant_unref(unlocked);
    g_variant_unref(path);
    g_variant_unref(opened);
    g_object_unref(connection);
    return 0;
}
