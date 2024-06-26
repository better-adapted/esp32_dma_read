/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/adc.h"

#define TIMES              512
#define GET_UNIT(x)        ((x>>3) & 0x1)

#if CONFIG_IDF_TARGET_ESP32
#define ADC_RESULT_BYTE                 2
#define ADC_CONV_LIMIT_EN               1                       //For ESP32, this should always be set to 1
#define ADC_OUTPUT_TYPE                 ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define EXAMPLE_ADC_USE_OUTPUT_TYPE1    1
#define ADC_CONV_MODE                   ADC_CONV_SINGLE_UNIT_1
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_RESULT_BYTE                 2
#define ADC_CONV_LIMIT_EN               0
#define ADC_OUTPUT_TYPE                 ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define ADC_CONV_MODE                   ADC_CONV_BOTH_UNIT
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32S3
#define ADC_RESULT_BYTE                 4
#define ADC_CONV_LIMIT_EN               0
#define ADC_OUTPUT_TYPE                 ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define ADC_CONV_MODE                   ADC_CONV_SINGLE_UNIT_1
#endif

#if CONFIG_IDF_TARGET_ESP32
static uint16_t adc1_chan_mask = BIT(0) | BIT(3) | BIT(6);
static uint16_t adc2_chan_mask = 0;
static adc_channel_t channel[3] = {ADC1_CHANNEL_0,ADC1_CHANNEL_3,ADC1_CHANNEL_6};

#elif CONFIG_IDF_TARGET_ESP32S2
static uint16_t adc1_chan_mask = BIT(2) | BIT(3);
static uint16_t adc2_chan_mask = BIT(0);
static adc_channel_t channel[3] = {ADC1_CHANNEL_2, ADC1_CHANNEL_3, (ADC2_CHANNEL_0 | 1 << 3)};

#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32H2
static uint16_t adc1_chan_mask = BIT(2) | BIT(3);
static uint16_t adc2_chan_mask = 0;
static adc_channel_t channel[2] = {ADC1_CHANNEL_2, ADC1_CHANNEL_3};
#endif

static const char *TAG = "ADC DMA";

static void continuous_adc_init(uint16_t adc1_chan_mask, uint16_t adc2_chan_mask, adc_channel_t *channel, uint8_t channel_num)
{
    adc_digi_init_config_t adc_dma_config = {
        .max_store_buf_size = 1024,
        .conv_num_each_intr = TIMES,
        .adc1_chan_mask = adc1_chan_mask,
        .adc2_chan_mask = adc2_chan_mask,
    };
    ESP_ERROR_CHECK(adc_digi_initialize(&adc_dma_config));

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en = ADC_CONV_LIMIT_EN,
        .conv_limit_num = 250,
        .sample_freq_hz = 20480,
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = GET_UNIT(channel[i]);
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_6;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        //ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        //ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        //ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_digi_controller_configure(&dig_cfg));
}

static bool check_valid_data(const adc_digi_output_data_t *data)
{
#if EXAMPLE_ADC_USE_OUTPUT_TYPE1
    if (data->type1.channel >= SOC_ADC_CHANNEL_NUM(ADC_UNIT_1)) {
        return false;
    }
#else
    int unit = data->type2.unit;
    if (unit >= ADC_UNIT_2) {
        return false;
    }
    if (data->type2.channel >= SOC_ADC_CHANNEL_NUM(unit)) {
        return false;
    }
#endif

    return true;
}

typedef struct
{
	uint16_t buffer[TIMES/2];
	uint16_t min;
	uint16_t max;
	uint16_t range;
	uint16_t index;
}CT_chan_t;

typedef struct
{
	CT_chan_t ct_1;
	CT_chan_t ct_2;
	CT_chan_t ct_3;
}CT_results_t;

static CT_results_t CTS_Raw;

void store_ct_raw(CT_chan_t* chan, uint16_t pRaw)
{
	if(chan->index>=TIMES/2)
	{
		return;
	}

	if(chan->index==0)
	{
		chan->max=0;
		chan->min=0xFFFF;
	}

	chan->buffer[chan->index]=pRaw;

	if(chan->min > pRaw)
	{
		chan->min = pRaw;
	}

	if(chan->max < pRaw)
	{
		chan->max = pRaw;
	}

	chan->range = chan->max-chan->min;

	chan->index++;
}

void results_ct_raw(CT_chan_t* chan,char *id)
{
	char msg[200];
	sprintf(msg,"ID=%s,min=%d,max=%d,range=%d",id,chan->min,chan->max,chan->range);
	ESP_LOGI(TAG, "%s",msg);
}

void app_main(void)
{
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[TIMES] = {0};
    memset(result, 0xcc, TIMES);

    continuous_adc_init(adc1_chan_mask, adc2_chan_mask, channel, sizeof(channel) / sizeof(adc_channel_t));
    adc_digi_start();

    int64_t start_time = esp_timer_get_time();
    int block_counter=0;
    int block_temp=0;

    while(1) {
        ret = adc_digi_read_bytes(result, TIMES, &ret_num, ADC_MAX_DELAY);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            if (ret == ESP_ERR_INVALID_STATE) {
                /**
                 * @note 1
                 * Issue:
                 * As an example, we simply print the result out, which is super slow. Therefore the conversion is too
                 * fast for the task to handle. In this condition, some conversion results lost.
                 *
                 * Reason:
                 * When this error occurs, you will usually see the task watchdog timeout issue also.
                 * Because the conversion is too fast, whereas the task calling `adc_digi_read_bytes` is slow.
                 * So `adc_digi_read_bytes` will hardly block. Therefore Idle Task hardly has chance to run. In this
                 * example, we add a `vTaskDelay(1)` below, to prevent the task watchdog timeout.
                 *
                 * Solution:
                 * Either decrease the conversion speed, or increase the frequency you call `adc_digi_read_bytes`
                 */
//            	continue;
            }

            if((esp_timer_get_time() - start_time)>1000000)
			{
            	block_counter=block_temp;
        		ESP_LOGI(TAG, "Blocks %d",block_counter);

            	start_time = esp_timer_get_time();
        		block_temp=0;
			}
            else
            {
            	block_temp+=ret_num;
            }

            if(block_temp==0)
            {
            	ESP_LOGI("TASK:", "ret is %x, ret_num is %d", ret, ret_num);
            	{
            		results_ct_raw(&CTS_Raw.ct_1,"ct_1");
            		results_ct_raw(&CTS_Raw.ct_2,"ct_2");
            		results_ct_raw(&CTS_Raw.ct_3,"ct_3");

            		memset(&CTS_Raw,0,sizeof(CTS_Raw));
            	}

                for (int i = 0; i < ret_num; i += ADC_RESULT_BYTE)
                {
                    adc_digi_output_data_t *p = (void*)&result[i];
                    if (check_valid_data(p))
                    {
                    	// store the raw results to the buffer..
                    	switch(p->type1.channel)
                    	{
                    	case 0:
                    		store_ct_raw(&CTS_Raw.ct_1,p->type1.data);
                    		break;
                    	case 3:
                    		store_ct_raw(&CTS_Raw.ct_2,p->type1.data);
                    		break;
                    	case 6:
                    		store_ct_raw(&CTS_Raw.ct_3,p->type1.data);
                    		break;
                    	}

                #if EXAMPLE_ADC_USE_OUTPUT_TYPE1
                    	if(i<12)
                    		ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %d", 1, p->type1.channel, p->type1.data);
                #else
                        ESP_LOGI(TAG, "Unit: %d,_Channel: %d, Value: %x", p->type2.unit + 1, p->type2.channel, p->type2.data);
                #endif
                    }
                    else
                    {
                        ESP_LOGI(TAG, "Invalid data");
                    }
                }
            }

            //See `note 1`
            vTaskDelay(1);
        } else if (ret == ESP_ERR_TIMEOUT) {
            /**
             * ``ESP_ERR_TIMEOUT``: If ADC conversion is not finished until Timeout, you'll get this return error.
             * Here we set Timeout ``portMAX_DELAY``, so you'll never reach this branch.
             */
            ESP_LOGW(TAG, "No data, increase timeout or reduce conv_num_each_intr");
            vTaskDelay(1000);
        }

    }

    adc_digi_stop();
    ret = adc_digi_deinitialize();
    assert(ret == ESP_OK);
}
