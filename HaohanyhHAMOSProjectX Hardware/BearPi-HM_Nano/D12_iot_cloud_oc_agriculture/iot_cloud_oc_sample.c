/*
 * Copyright (c) 2020 Nanjing Xiaoxiongpai Intelligent Technology Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 /* 受Haohanyh Computer Software Products Open Source LICENSE保护 https://git.haohanyh.top:3001/Haohanyh/LICENSE */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "ohos_init.h"
#include "cmsis_os2.h"

#include "wifi_connect.h"
#include <queue.h>
#include <oc_mqtt_al.h>
#include <oc_mqtt_profile.h>
#include "E53_IA1.h"
#include <dtls_al.h>
#include <mqtt_al.h>

#define CONFIG_WIFI_SSID          "233333333"                            //修改为自己的WiFi 热点账号

#define CONFIG_WIFI_PWD           "233333333"                        //修改为自己的WiFi 热点密码

#define CONFIG_APP_SERVERIP       "**********.iot-mqtts.cn-north-4.myhuaweicloud.com"

#define CONFIG_APP_SERVERPORT     "1883"

#define CONFIG_APP_DEVICEID       "**********"       //替换为注册设备后生成的deviceid

#define CONFIG_APP_DEVICEPWD      "**********"                                   //替换为注册设备后生成的密钥

#define CONFIG_APP_LIFETIME       60     ///< seconds

#define CONFIG_QUEUE_TIMEOUT      (5*1000)

#define MSGQUEUE_OBJECTS 16 // number of Message Queue Objects

osMessageQueueId_t mid_MsgQueue; // message queue id
typedef enum
{
    en_msg_cmd = 0,
    en_msg_report,
    en_msg_conn,
    en_msg_disconn,
}en_msg_type_t;

typedef struct
{
    char *request_id;
    char *payload;
} cmd_t;

typedef struct
{
    int lum;
    int temp;
    int hum;
} report_t;

typedef struct
{
    en_msg_type_t msg_type;
    union
    {
        cmd_t cmd;
        report_t report;
    } msg;
} app_msg_t;

typedef struct
{
    queue_t                     *app_msg;
    int                          connected;
    int                          led;
    int                          motor;
}app_cb_t;
static app_cb_t  g_app_cb;

static void deal_report_msg(report_t *report)
{
    oc_mqtt_profile_service_t    service;
    oc_mqtt_profile_kv_t         temperature;
    oc_mqtt_profile_kv_t         humidity;
    oc_mqtt_profile_kv_t         luminance;
    oc_mqtt_profile_kv_t         led;
    oc_mqtt_profile_kv_t         motor;

    if(g_app_cb.connected != 1){
        return;
    }

    service.event_time = NULL;
    service.service_id = "Agriculture";
    service.service_property = &temperature;
    service.nxt = NULL;

    temperature.key = "Temperature";
    temperature.value = &report->temp;
    temperature.type = EN_OC_MQTT_PROFILE_VALUE_INT;
    temperature.nxt = &humidity;

    humidity.key = "Humidity";
    humidity.value = &report->hum;
    humidity.type = EN_OC_MQTT_PROFILE_VALUE_INT;
    humidity.nxt = &luminance;

    luminance.key = "Luminance";
    luminance.value = &report->lum;
    luminance.type = EN_OC_MQTT_PROFILE_VALUE_INT;
    luminance.nxt = &led;

    led.key = "LightStatus";
    led.value = g_app_cb.led?"ON":"OFF";
    led.type = EN_OC_MQTT_PROFILE_VALUE_STRING;
    led.nxt = &motor;

    motor.key = "MotorStatus";
    motor.value = g_app_cb.motor?"ON":"OFF";
    motor.type = EN_OC_MQTT_PROFILE_VALUE_STRING;
    motor.nxt = NULL;

    oc_mqtt_profile_propertyreport(NULL,&service);
    return;
}

//use this function to push all the message to the buffer
static int msg_rcv_callback(oc_mqtt_profile_msgrcv_t *msg)
{
    int    ret = 0;
    char  *buf;
    int    buf_len;
    app_msg_t *app_msg;

    if((NULL == msg)|| (msg->request_id == NULL) || (msg->type != EN_OC_MQTT_PROFILE_MSG_TYPE_DOWN_COMMANDS)){
        return ret;
    }

    buf_len = sizeof(app_msg_t) + strlen(msg->request_id) + 1 + msg->msg_len + 1;
    buf = malloc(buf_len);
    if(NULL == buf){
        return ret;
    }
    app_msg = (app_msg_t *)buf;
    buf += sizeof(app_msg_t);

    app_msg->msg_type = en_msg_cmd;
    app_msg->msg.cmd.request_id = buf;
    buf_len = strlen(msg->request_id);
    buf += buf_len + 1;
    memcpy(app_msg->msg.cmd.request_id, msg->request_id, buf_len);
    app_msg->msg.cmd.request_id[buf_len] = '\0';

    buf_len = msg->msg_len;
    app_msg->msg.cmd.payload = buf;
    memcpy(app_msg->msg.cmd.payload, msg->msg, buf_len);
    app_msg->msg.cmd.payload[buf_len] = '\0';

    ret = queue_push(g_app_cb.app_msg,app_msg,10);
    if(ret != 0){
        free(app_msg);
    }

    return ret;
}

///< COMMAND DEAL
#include <cJSON.h>
static void deal_cmd_msg(cmd_t *cmd)
{
    cJSON *obj_root;
    cJSON *obj_cmdname;
    cJSON *obj_paras;
    cJSON *obj_para;

    int cmdret = 1;
    oc_mqtt_profile_cmdresp_t cmdresp;
    obj_root = cJSON_Parse(cmd->payload);
    if (NULL == obj_root)
    {
        goto EXIT_JSONPARSE;
    }

    obj_cmdname = cJSON_GetObjectItem(obj_root, "command_name");
    if (NULL == obj_cmdname)
    {
        goto EXIT_CMDOBJ;
    }
    if (0 == strcmp(cJSON_GetStringValue(obj_cmdname), "Agriculture_Control_light"))
    {
        obj_paras = cJSON_GetObjectItem(obj_root, "paras");
        if (NULL == obj_paras)
        {
            goto EXIT_OBJPARAS;
        }
        obj_para = cJSON_GetObjectItem(obj_paras, "Light");
        if (NULL == obj_para)
        {
            goto EXIT_OBJPARA;
        }
        ///< operate the LED here
        if (0 == strcmp(cJSON_GetStringValue(obj_para), "ON"))
        {
            g_app_cb.led = 1;
            Light_StatusSet(ON);
            printf("Light On!\r\n");
        }
        else
        {
            g_app_cb.led = 0;
            Light_StatusSet(OFF);
            printf("Light Off!\r\n");
        }
        cmdret = 0;
    }
    else if (0 == strcmp(cJSON_GetStringValue(obj_cmdname), "Agriculture_Control_Motor"))
    {
        obj_paras = cJSON_GetObjectItem(obj_root, "Paras");
        if (NULL == obj_paras)
        {
            goto EXIT_OBJPARAS;
        }
        obj_para = cJSON_GetObjectItem(obj_paras, "Motor");
        if (NULL == obj_para)
        {
            goto EXIT_OBJPARA;
        }
        ///< operate the Motor here
        if (0 == strcmp(cJSON_GetStringValue(obj_para), "ON"))
        {
            g_app_cb.motor = 1;
            Motor_StatusSet(ON);
            printf("Motor On!\r\n");
        }
        else
        {
            g_app_cb.motor = 0;
            Motor_StatusSet(OFF);
            printf("Motor Off!\r\n");
        }
        cmdret = 0;
    }

EXIT_OBJPARA:
EXIT_OBJPARAS:
EXIT_CMDOBJ:
    cJSON_Delete(obj_root);
EXIT_JSONPARSE:
    ///< do the response
    cmdresp.paras = NULL;
    cmdresp.request_id = cmd->request_id;
    cmdresp.ret_code = cmdret;
    cmdresp.ret_name = NULL;
    (void)oc_mqtt_profile_cmdresp(NULL, &cmdresp);
    return;
}


static int task_main_entry(void)
{
    app_msg_t *app_msg;
    uint32_t ret ;
    
    WifiConnect(CONFIG_WIFI_SSID, CONFIG_WIFI_PWD);
    dtls_al_init();
    mqtt_al_init();
    oc_mqtt_init();
    
    g_app_cb.app_msg = queue_create("queue_rcvmsg",10,1);
    if(NULL ==  g_app_cb.app_msg){
        printf("Create receive msg queue failed");
        
    }
    oc_mqtt_profile_connect_t  connect_para;
    (void) memset( &connect_para, 0, sizeof(connect_para));

    connect_para.boostrap =      0;
    connect_para.device_id =     CONFIG_APP_DEVICEID;
    connect_para.device_passwd = CONFIG_APP_DEVICEPWD;
    connect_para.server_addr =   CONFIG_APP_SERVERIP;
    connect_para.server_port =   CONFIG_APP_SERVERPORT;
    connect_para.life_time =     CONFIG_APP_LIFETIME;
    connect_para.rcvfunc =       msg_rcv_callback;
    connect_para.security.type = EN_DTLS_AL_SECURITY_TYPE_NONE;
    ret = oc_mqtt_profile_connect(&connect_para);
    if((ret == (int)en_oc_mqtt_err_ok)){
        g_app_cb.connected = 1;
        printf("oc_mqtt_profile_connect succed!\r\n");
    }
    else
    {
        printf("oc_mqtt_profile_connect faild!\r\n");
    }
    
    while (1)
    {
        app_msg = NULL;
        (void)queue_pop(g_app_cb.app_msg,(void **)&app_msg,0xFFFFFFFF);
        if(NULL != app_msg){
            switch(app_msg->msg_type){
                case en_msg_cmd:
                    deal_cmd_msg(&app_msg->msg.cmd);
                    break;
                case en_msg_report:
                    deal_report_msg(&app_msg->msg.report);
                    break;
                default:
                    break;
            }
            free(app_msg);
        }
    }
    return 0;
}

static int task_sensor_entry(void)
{
    app_msg_t *app_msg;
    E53_IA1_Data_TypeDef data;
    E53_IA1_Init();
    while (1)
    {
        E53_IA1_Read_Data(&data);
        app_msg = malloc(sizeof(app_msg_t));
        printf("SENSOR:lum:%.2f temp:%.2f hum:%.2f\r\n", data.Lux, data.Temperature, data.Humidity);
        if (NULL != app_msg)
        {
            app_msg->msg_type = en_msg_report;
            app_msg->msg.report.hum = (int)data.Humidity;
            app_msg->msg.report.lum = (int)data.Lux;
            app_msg->msg.report.temp = (int)data.Temperature;
            if(0 != queue_push(g_app_cb.app_msg,app_msg,CONFIG_QUEUE_TIMEOUT)){
                free(app_msg);
            }
        }
        sleep(3);
    }
    return 0;
}

static void OC_Demo(void)
{

    osThreadAttr_t attr;

    attr.name = "task_main_entry";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = 10240;
    attr.priority = 24;

    if (osThreadNew((osThreadFunc_t)task_main_entry, NULL, &attr) == NULL)
    {
        printf("Falied to create task_main_entry!\n");
    }
    attr.stack_size = 2048;
    attr.priority = 25;
    attr.name = "task_sensor_entry";
    if (osThreadNew((osThreadFunc_t)task_sensor_entry, NULL, &attr) == NULL)
    {
        printf("Falied to create task_sensor_entry!\n");
    }
}

APP_FEATURE_INIT(OC_Demo);