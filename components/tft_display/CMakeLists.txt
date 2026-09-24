idf_component_register(
    SRCS "tft_display.c" "font_cn16.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos math_generator
)
