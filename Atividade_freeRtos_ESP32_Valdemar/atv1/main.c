

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

void vMonitorTask( void *pvParameters );

void app_main(void)
{

    xTaskCreate(vMonitorTask, "MonitorTask", 10000, NULL, 5, NULL);
    
}


void vMonitorTask(void *pvParameters)
{
    char buffer[1024]; 

    for(;;)
    {
        TaskStatus_t *pxTaskStatusArray;
        UBaseType_t uxArraySize, x;
        BaseType_t xCoreID; // Get the core ID (0 or 1)
        unsigned long ulTotalRunTime, ulStatsAsPercentage;

        buffer[0] = '\0';

        uxArraySize = uxTaskGetNumberOfTasks();

        pxTaskStatusArray = pvPortMalloc(uxArraySize * sizeof(TaskStatus_t));

        if (pxTaskStatusArray != NULL)
        {
            uxArraySize = uxTaskGetSystemState(
                pxTaskStatusArray,
                uxArraySize,
                &ulTotalRunTime
            );

            ulTotalRunTime /= 100UL;

            if (ulTotalRunTime > 0)
            {
                for (x = 0; x < uxArraySize; x++)
                {
                    ulStatsAsPercentage = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalRunTime;

                    xCoreID = xTaskGetCoreID(pxTaskStatusArray[x].xHandle); // Get the core ID (0 or 1)

                    char *state;

                    switch (pxTaskStatusArray[x].eCurrentState)
                    {
                        case eRunning:
                            state = "Running";
                            break;
                        case eReady:
                            state = "Ready";
                            break;
                        case eBlocked:
                            state = "Blocked";
                            break;
                        case eSuspended:
                            state = "Suspended";
                            break;
                        default:
                            state = "Unknown";
                            break;
                    }

                    char line[128];

                    if (ulStatsAsPercentage > 0)
                    {
                        sprintf(line, "Nome: %s | Estado: %s | Prioridade: %u | Stack: %lu | Core: %d | Tempo de CPU: %lu | Uso de CPU: %lu%%\n",
                            pxTaskStatusArray[x].pcTaskName,
                            state,
                            pxTaskStatusArray[x].uxCurrentPriority,
                            pxTaskStatusArray[x].usStackHighWaterMark,
                            xCoreID,
                            pxTaskStatusArray[x].ulRunTimeCounter,
                            ulStatsAsPercentage);
                    }
                    else
                    {
                        sprintf(line, "Nome: %s | Estado: %s | Prioridade: %u | Stack: %lu | Core: %d | Tempo de CPU: %lu | Uso de CPU: %lu%%\n",
                            pxTaskStatusArray[x].pcTaskName,
                            state,
                            pxTaskStatusArray[x].uxCurrentPriority,
                            pxTaskStatusArray[x].usStackHighWaterMark,
                            xCoreID,
                            pxTaskStatusArray[x].ulRunTimeCounter,
                            ulStatsAsPercentage);
                    }

                    strcat(buffer, line);
                }
            }

            vPortFree(pxTaskStatusArray);
        }

        int32_t size_memory = xPortGetFreeHeapSize();

        ESP_LOGI("MONITOR", "Memória Livre: %ld bytes", (long int)size_memory);
        ESP_LOGI("MONITOR", "================ TASK MONITOR ================");
        ESP_LOGI("MONITOR", "\n%s", buffer);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    vTaskDelete(NULL);
}
