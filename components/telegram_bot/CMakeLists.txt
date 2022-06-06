idf_component_register(
	SRCS bot.c
    INCLUDE_DIRS "." ${CMAKE_SOURCE_DIR}/jsmn
	EMBED_TXTFILES api_telegram_org_root_cert.pem
    REQUIRES nvs_flash esp-tls esp_http_client
)
