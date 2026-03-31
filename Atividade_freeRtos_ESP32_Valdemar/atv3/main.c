#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "semaphore.h"

#define BUFFER_SIZE 10

/*Escrita*/
void vEscrita(void *parameters);


/*Leitura*/
void TaskLeitura(void *parameters);

/*Recurso compartilhado*/

char buffer[BUFFER_SIZE][20];
int writeIndex = 0;
int readIndex = 0;
int count = 0;

int espacos_ativos = 5;

/*Inicializacoa Mutex*/

SemaphoreHandle_t mutex;
SemaphoreHandle_t posicoesLivres; /* quantas posições livres existem*/
SemaphoreHandle_t posicoesOcupadas; /* quantas posicoes ocupadas existem*/


void app_main(void)
{

    mutex = xSemaphoreCreateMutex();

    posicoesLivres = xSemaphoreCreateCounting(BUFFER_SIZE, BUFFER_SIZE);
    posicoesOcupadas = xSemaphoreCreateCounting(BUFFER_SIZE, 0);

    int32_t size_heap = xPortGetFreeHeapSize();
    ESP_LOGI("MAIN", "Total size in memory: %d", size_heap);

    /*Task de escritas*/
    xTaskCreate(vEscrita, "Temperatura", 2000, "Temperatura", 2, NULL );
    xTaskCreate(vEscrita, "Umidade", 2000, "Umidade", 2, NULL );
    xTaskCreate(vEscrita, "Velocidade", 2000, "Velocidade", 2, NULL );
    xTaskCreate(vEscrita, "Peso", 2000, "Peso", 2, NULL );
    xTaskCreate(vEscrita, "Distancia", 2000, "Distancia", 2, NULL );

    /*Taskes de Leituras*/
    xTaskCreate(TaskLeitura, "TaskLeitura1", 2000, "TaskLeitura1: ", 2, NULL );
    xTaskCreate(TaskLeitura, "TaskLeitura2", 2000, "TaskLeitura2:", 2, NULL );
}


void vEscrita(void *parameters){

    char *nome = (char *) parameters;
    int contador = 0;

    for(;;){

        if(contador >= 5){
            break;
        }

        xSemaphoreTake(posicoesLivres, portMAX_DELAY);

        
        xSemaphoreTake(mutex, portMAX_DELAY);

        strcpy(buffer[writeIndex], nome);
        writeIndex = (writeIndex + 1) % BUFFER_SIZE;
        contador++;

        xSemaphoreGive(mutex);

        xSemaphoreGive(posicoesOcupadas);

        vTaskDelay(500/portTICK_PERIOD_MS);
    }

    xSemaphoreTake(mutex, portMAX_DELAY);
    espacos_ativos--;
    xSemaphoreGive(mutex);
    printf("%s: Escrita Finalizada.\n", nome);
    vTaskDelete(NULL);
}

void TaskLeitura(void *param)
{
    char *nome = (char *) param;
    char dado[20];

    for (;;){
        
        if (xSemaphoreTake(posicoesOcupadas, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            xSemaphoreTake(mutex, portMAX_DELAY);

            strcpy(dado, buffer[readIndex]);
            readIndex = (readIndex + 1) % BUFFER_SIZE;

            xSemaphoreGive(mutex);

            
            xSemaphoreGive(posicoesLivres);

            printf("%s: %s\n", nome, dado);
        }
        else
        {
            xSemaphoreTake(mutex, portMAX_DELAY);
            int acabou = (espacos_ativos == 0);
            xSemaphoreGive(mutex);
            
            if (espacos_ativos == 0)
            {
                break;
            }
        }
    }

    printf("%s: Leitura finalizada!\n", nome);

    vTaskDelete(NULL);
}
