#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#define DURACAO 10000000 // 10 segundos o teste


void task1(void *parameters);
void task2(void *parameters);

volatile int64_t time_task1 = 0;
volatile int64_t time_task2 = 0;
volatile int64_t time_init = 0;
volatile int64_t heap_size = 0;
volatile int64_t total_time =0;
volatile int count = 0;
volatile bool stop = false;

double media, percentual;


void app_main(void)
{
    int64_t heap_size = xPortGetFreeHeapSize();
    time_init = esp_timer_get_time();
    ESP_LOGI("Main", "Free heap size: %d bytes", heap_size);
    xTaskCreatePinnedToCore(task1, "Task 1", 2048, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(task2, "Task 2", 2048, NULL, 2, NULL, 0);
}

void task1(void *parameters)
{
    for (;;)
    {

        if((esp_timer_get_time()-time_init)>DURACAO){
            break;
        }
        time_task1 = esp_timer_get_time();
        ESP_LOGI("Task 1", "Task 1 is running at time: %lld", time_task1);
        vTaskDelay(100/portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void task2(void *parameters)
{
    for (;;)
    {

        if((esp_timer_get_time() - time_init ) > DURACAO){
            break;
        }


        time_task2 = esp_timer_get_time();

        int64_t time_difference =  time_task2 - time_task1;
        total_time += time_difference;
        count++;


        ESP_LOGI("Task 2", "Task 2 is running at time: %lld", time_task2);
        vTaskDelay(100/portTICK_PERIOD_MS);
    }

    media = (double) total_time/count;
    percentual =  ((double) media/DURACAO)*100;

    ESP_LOGI("Task 2", "Trocas: %d", count);
    ESP_LOGI("Task 2", "Tempo medio %2.f us", media);
    ESP_LOGI("Task 2", "Overhead do kernel: %.4f %%", percentual);

    vTaskDelete(NULL);
}
