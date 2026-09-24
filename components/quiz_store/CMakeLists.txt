idf_component_register(
    SRCS "quiz_store.c"
    INCLUDE_DIRS "include"
    REQUIRES spiffs json math_generator esp_timer
)
