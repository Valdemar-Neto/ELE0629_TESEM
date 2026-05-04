/** @file main.c
 *  @brief Ponto de entrada do DataLogger ESP32.
 *
 *  A main() é responsável apenas por chamar rtos_init(),
 *  que inicializa periféricos, cria tarefas e arma o esp_timer.
 *  Toda a lógica pertence ao componente RTOS.
 */
 
#include "esp_log.h"
#include "rtos.h"
 
static const char *TAG = "MAIN";
 
void app_main(void)
{
    ESP_LOGI(TAG, "========== DataLogger ESP32 ==========");
    ESP_LOGI(TAG, "ELE0629 — Topicos Especiais em Sistemas Embarcados");
    ESP_LOGI(TAG, "Inicializando sistema...");
 
    esp_err_t err = rtos_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rtos_init falhou: %s — sistema nao iniciado", esp_err_to_name(err));
        return;
    }
 
    ESP_LOGI(TAG, "Sistema iniciado com sucesso.");
    /* app_main retorna — FreeRTOS assume o controle das tarefas */
}
 