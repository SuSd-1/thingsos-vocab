// SPDX-License-Identifier: Apache-2.0
/* Approved Bluetooth UE. All rendering and physical input stay on the UI task. */
typedef struct {thingsos_bt_status_t status;thingsos_bt_device_t selected;unsigned serial,view,saved_start,found_start;bool confirm_pair,confirm_forget;} bluetooth_ui_t;
static bluetooth_ui_t* bluetooth_ui;
static bool bt_ui_ready(void){if(!bluetooth_ui)bluetooth_ui=thingsos_psram_calloc(1,sizeof(*bluetooth_ui));return bluetooth_ui!=NULL;}
static bool bt_ui_unsaved_current(void){thingsos_bt_status_t* s=&bluetooth_ui->status;if(!s->connected)return false;for(unsigned i=0;i<s->saved_count;++i)if(!strcmp(s->saved[i].address,s->current.address))return false;return true;}
static unsigned bt_ui_saved_count(void){return bluetooth_ui->status.saved_count+(bt_ui_unsaved_current()?1u:0u);}
static thingsos_bt_device_t* bt_ui_saved_at(unsigned i){if(bt_ui_unsaved_current()){if(!i)return &bluetooth_ui->status.current;--i;}return bluetooth_ui->status.saved+i;}
static void bt_ui_state(void){if(bt_ui_ready()){thingsos_bluetooth_status(&bluetooth_ui->status);if(bluetooth_ui->saved_start>=bt_ui_saved_count())bluetooth_ui->saved_start=0;if(bluetooth_ui->found_start>=bluetooth_ui->status.found_count)bluetooth_ui->found_start=0;}}
static const char* bt_ui_summary(void){bt_ui_state();return !bluetooth_ui?"未连接":!bluetooth_ui->status.enabled?"已关闭":bluetooth_ui->status.connected?bluetooth_ui->status.current.name:"未连接";}
static bool bt_ui_idle(void){if(thingsos_audio_suspend_locked())return true;say("请先保存录音或结束音频初始化，再修改蓝牙连接");return false;}
static void bt_ui_result(bool ok){if(!ok)say("蓝牙任务处理中，请稍后重试");app.dirty=true;}
static void bt_ui_enter(void){if(!bt_ui_ready()){say("界面内存不足");return;}bluetooth_ui->view=0;bluetooth_ui->confirm_pair=bluetooth_ui->confirm_forget=false;bt_ui_state();navigate(THINGSOS_UI_BLUETOOTH);}
static void bt_ui_poll(void){
 if(!bluetooth_ui)return;
 thingsos_bluetooth_status(&bluetooth_ui->status);
 if(bluetooth_ui->status.serial!=bluetooth_ui->serial){bluetooth_ui->serial=bluetooth_ui->status.serial;if(app.page==THINGSOS_UI_BLUETOOTH||app.page==THINGSOS_UI_SETTINGS)app.dirty=true;}
}
static void bt_ui_row(const thingsos_bt_device_t* d,int top,bool saved){
 const thingsos_bt_status_t* s=&bluetooth_ui->status;
 label(d->name,48,top+30,28,500);label(s->connected&&!strcmp(s->current.address,d->address)?"已连接 · 音频输出":saved?"已保存":"可配对",48,top+66,22,480);
 label(saved?">":"配对",saved?600:550,top+42,24,80);thingsos_rect(&app.canvas,36,top+86,612,1,170);
}
/* Two-state switch: the left segment is off, the right segment is on. */
static void bt_ui_switch(int x,int y,bool on){
 static const char* const names[]={"关","开"};
 reader_refresh_segments(names,x,y,176,2,on?1:0);
}
static bool bt_ui_draw(void){
 if(app.page!=THINGSOS_UI_BLUETOOTH||!bt_ui_ready())return false;
 bt_ui_state();thingsos_bt_status_t* s=&bluetooth_ui->status;header(bluetooth_ui->view?"设备详情":"蓝牙");
 if(bluetooth_ui->view){
  thingsos_bt_device_t* d=&bluetooth_ui->selected;bool connected=s->connected&&!strcmp(s->current.address,d->address);
  label(d->name,36,285,36,612);label(d->address,36,338,24,612);
  label(s->busy?"正在处理…":connected?"已连接 · 音频输出":"已保存 · 未连接",36,425,28,612);
  reader_control(connected?"断开连接":"连接",36,490,612,!s->busy);reader_control("忘记此设备",36,590,612,false);
  label("断开保留设备信息，忘记后需重新配对",36,720,24,612);
 }else{
  label("蓝牙",36,264,30,400);bt_ui_switch(472,218,s->enabled);
  label(s->enabled?"将耳机或音箱设为配对模式，再搜索设备":"开启后可搜索并连接蓝牙设备",36,318,22,612);
  thingsos_rect(&app.canvas,36,346,612,1,170);
  if(!s->enabled){centered_label("蓝牙已关闭",36,470,32,612);centered_label("已配对设备会保留，开启后可继续使用",36,540,25,612);}
  else{
   label("已配对设备",36,395,25,450);
   if(bt_ui_saved_count()>2)label("下一组 >",520,395,23,128);
   if(!bt_ui_saved_count())label("还没有配对设备",48,468,25,570);
   for(unsigned i=0;i<2&&bluetooth_ui->saved_start+i<bt_ui_saved_count();++i)bt_ui_row(bt_ui_saved_at(bluetooth_ui->saved_start+i),416+(int)i*88,true);
   label("自动重连上次设备",36,650,24,420);bt_ui_switch(472,604,s->automatic);
   label("可用设备",36,731,25,440);reader_control(s->phase==THINGSOS_BT_SCANNING?"停止搜索":"搜索设备",464,684,184,false);
   if(!s->found_count)label(s->phase==THINGSOS_BT_SCANNING?"正在搜索附近蓝牙设备…":"点击“搜索设备”开始",48,825,25,570);
   for(unsigned i=0;i<3&&bluetooth_ui->found_start+i<s->found_count;++i)bt_ui_row(s->found+bluetooth_ui->found_start+i,766+(int)i*88,false);
   if(s->found_count>3)reader_control("上一组",36,1052,285,false),reader_control("下一组",363,1052,285,false);
  }
 }
 if(s->message[0])label(s->message,36,1175,23,612);
 if(bluetooth_ui->confirm_pair||bluetooth_ui->confirm_forget){
  bool pair=bluetooth_ui->confirm_pair;thingsos_rect(&app.canvas,36,355,612,475,0);thingsos_rect(&app.canvas,38,357,608,471,255);
  label(pair?"配对并连接？":"忘记此设备？",66,420,34,552);label(bluetooth_ui->selected.name,66,490,28,552);
  label(pair?"成功后保存设备信息，下次可直接连接":"移除本机连接记录，录音和音乐保留",66,550,24,552);
  reader_control(pair?"配对并连接":"忘记设备",66,610,552,true);reader_control("取消",66,710,552,false);
 }
 return true;
}
static bool bt_ui_key(thingsos_key_action_t action){
 if(app.page!=THINGSOS_UI_BLUETOOTH)return false;
 if(action==THINGSOS_KEY_HOME)return false;
 if(action!=THINGSOS_KEY_PREVIOUS&&action!=THINGSOS_KEY_MENU)return true;
 if(bluetooth_ui->confirm_pair||bluetooth_ui->confirm_forget)bluetooth_ui->confirm_pair=bluetooth_ui->confirm_forget=false;
 else if(bluetooth_ui->view)bluetooth_ui->view=0;else navigate(THINGSOS_UI_SETTINGS);
 app.dirty=true;return true;
}
static bool bt_ui_tap(int x,int y){
 if(app.page!=THINGSOS_UI_BLUETOOTH||!bt_ui_ready())return false;
 if(app.controls_open)return false;
 bt_ui_state();thingsos_bt_status_t* s=&bluetooth_ui->status;
 if(bluetooth_ui->confirm_pair||bluetooth_ui->confirm_forget){
  if(x>=66&&x<618&&y>=710&&y<778)bluetooth_ui->confirm_pair=bluetooth_ui->confirm_forget=false;
  else if(x>=66&&x<618&&y>=610&&y<678&&bt_ui_idle()){
   bool ok=bluetooth_ui->confirm_pair?thingsos_bluetooth_connect(&bluetooth_ui->selected):thingsos_bluetooth_forget(bluetooth_ui->selected.address);
   if(ok){bluetooth_ui->view=bluetooth_ui->confirm_pair?1:0;bluetooth_ui->confirm_pair=bluetooth_ui->confirm_forget=false;}bt_ui_result(ok);
  }app.dirty=true;return true;
 }
 if(x<100&&y>=74&&y<200){bt_ui_key(THINGSOS_KEY_PREVIOUS);return true;}
 if(bluetooth_ui->view){
  bool connected=s->connected&&!strcmp(s->current.address,bluetooth_ui->selected.address);
  if(y>=490&&y<558&&!s->busy&&bt_ui_idle())bt_ui_result(connected?thingsos_bluetooth_disconnect():thingsos_bluetooth_connect(&bluetooth_ui->selected));
  else if(y>=590&&y<658&&!s->busy)bluetooth_ui->confirm_forget=true;
 }else if(y>=214&&y<300&&bt_ui_idle())bt_ui_result(thingsos_bluetooth_power(!s->enabled));
 else if(s->enabled){
  if(y>=596&&y<680)bt_ui_result(thingsos_bluetooth_automatic(!s->automatic));
  else if(x>=464&&x<648&&y>=684&&y<752&&bt_ui_idle()){bluetooth_ui->found_start=0;bt_ui_result(thingsos_bluetooth_scan(s->phase!=THINGSOS_BT_SCANNING));}
  else if(x>=520&&y>=360&&y<410&&bt_ui_saved_count()>2)bluetooth_ui->saved_start=bluetooth_ui->saved_start+2<bt_ui_saved_count()?bluetooth_ui->saved_start+2:0;
  else if(y>=416&&y<592&&!s->busy){unsigned i=bluetooth_ui->saved_start+(unsigned)(y-416)/88;if(i<bt_ui_saved_count()){bluetooth_ui->selected=*bt_ui_saved_at(i);bluetooth_ui->view=1;}}
  else if(y>=766&&y<1030&&!s->busy){unsigned i=bluetooth_ui->found_start+(unsigned)(y-766)/88;if(i<s->found_count){bluetooth_ui->selected=s->found[i];bluetooth_ui->confirm_pair=true;}}
  else if(y>=1052&&y<1120&&s->found_count>3){if(x<342)bluetooth_ui->found_start=bluetooth_ui->found_start>=3?bluetooth_ui->found_start-3:0;else if(bluetooth_ui->found_start+3<s->found_count)bluetooth_ui->found_start+=3;}
 }app.dirty=true;return true;
}
