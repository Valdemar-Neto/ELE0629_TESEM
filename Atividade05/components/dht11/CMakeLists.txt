idf_component_register(
    SRCS "dht11.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer
)