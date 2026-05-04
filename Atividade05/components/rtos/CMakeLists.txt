idf_component_register(
    SRCS "rtos.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        esp_timer
        esp_system
        spiffs
        esp_adc
        driver
        dht11
)