// SPDX-License-Identifier: Apache-2.0
#define THINGSOS_BT_HOST_TEST 1
#include "../firmware/components/thingsos_bluetooth/bluetooth.c"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"Bluetooth service line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 thingsos_bluetooth_init();CHECK(initialized&&!state.enabled&&state.automatic);
 thingsos_bt_device_t device={"001122334455","测试耳机"};
 event(BT_LINE_CONNECTED,&device);CHECK(!thingsos_bluetooth_connected()&&!host_bt_wireless);
 host_bt_ack=false;state.enabled=true;CHECK(!transmitter());CHECK(state.phase==THINGSOS_BT_ERROR&&!state.connected&&!host_bt_wireless);
 CHECK(!thingsos_bluetooth_audio_ready(false)&&!thingsos_bluetooth_audio_ready(true));
 host_bt_ack=true;CHECK(connect_device(&device));CHECK(state.phase==THINGSOS_BT_CONNECTING&&!state.connected&&!host_bt_wireless);
 CHECK(!strcmp(host_bt_last_command,"AT+CONNECT=001122334455\r\n"));
 CHECK(!state.saved_count);event(BT_LINE_CONNECTED,&device);
 CHECK(thingsos_bluetooth_connected()&&host_bt_wireless&&state.saved_count==1&&host_bt_store_bytes==sizeof(stored_t));
 CHECK(thingsos_bluetooth_audio_ready(false)&&!thingsos_bluetooth_audio_ready(true));
 event(BT_LINE_DISCONNECTED,&device);CHECK(!state.connected&&!host_bt_wireless&&thingsos_bluetooth_audio_ready(false)&&thingsos_bluetooth_audio_ready(true)&&state.saved_count==1);
 /* A headset that powers off never sends a disconnect line; the audio path sees
    the I2S master stop clocking and the service applies that on its own task. */
 CHECK(connect_device(&device));event(BT_LINE_CONNECTED,&device);CHECK(state.connected&&host_bt_wireless);
 thingsos_bluetooth_link_lost();CHECK(state.connected&&host_bt_wireless);
 apply_link_lost();CHECK(!state.connected&&!host_bt_wireless&&strstr(state.message,"连接断开")&&thingsos_bluetooth_audio_ready(false));
 thingsos_bluetooth_link_lost();apply_link_lost();CHECK(!state.connected&&!host_bt_wireless);
 memset(&state,0,sizeof(state));load();CHECK(state.saved_count==1&&state.enabled&&state.automatic&&!strcmp(state.last,device.address));
 host_bt_store[30]^=1;memset(&state,0,sizeof(state));load();CHECK(!state.saved_count&&!state.enabled);host_bt_store[30]^=1;
 memset(&state,0,sizeof(state));load();publish(THINGSOS_BT_READY,"");
 CHECK(thingsos_bluetooth_audio_ready(true)&&thingsos_bluetooth_audio_ready(false)); /* enabled-idle is local playback */
 thingsos_bt_device_t second={"AABBCCDDEEFF","另一副耳机"};CHECK(connect_device(&second));host_bt_nvs_fail=true;event(BT_LINE_CONNECTED,&second);
 CHECK(state.connected&&state.saved_count==1&&!strcmp(state.last,device.address));CHECK(strstr(state.message,"保存失败"));host_bt_nvs_fail=false;
 host_bt_audio_active=true;host_bt_audio_suspended=false;CHECK(!thingsos_bluetooth_power(false)&&!uxQueueMessagesWaiting(commands)&&!atomic_load(&route_requests));
 host_bt_audio_suspended=true;CHECK(thingsos_bluetooth_power(false));CHECK(atomic_load(&route_requests)==1&&!thingsos_bluetooth_audio_ready(false));
 worker(NULL);CHECK(!state.enabled&&!task&&!host_bt_wireless&&!atomic_load(&route_requests));CHECK(thingsos_bluetooth_audio_ready(false)&&thingsos_bluetooth_audio_ready(true));
 host_bt_audio_active=false;host_bt_audio_suspended=false;host_bt_task_fail=true;
 CHECK(!thingsos_bluetooth_power(true));CHECK(!atomic_load(&route_requests)&&!uxQueueMessagesWaiting(commands));host_bt_task_fail=false;
 state.enabled=false;publish(THINGSOS_BT_OFF,"");CHECK(thingsos_bluetooth_forget(device.address));worker(NULL);
 CHECK(!state.saved_count&&!state.last[0]);memset(&state,0,sizeof(state));load();CHECK(!state.saved_count&&!state.last[0]);
 puts("PASS: production BH service: mode acknowledgements, confirmed links only, durable records/CRC, save rollback, audio route gating, link loss, task failure and forget persistence");
 free(commands);return 0;
}
