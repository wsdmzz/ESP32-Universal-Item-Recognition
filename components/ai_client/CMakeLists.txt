idf_component_register(
    SRCS "ai_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json math_generator esp_timer
)
