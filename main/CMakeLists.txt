idf_component_register(
    SRCS
        "main.c"
        "usb_storage.c"
        "wifi_mgr.c"
        "web_server.c"
        "netlog.c"
        "auth.c"
        "ota.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html"
)
