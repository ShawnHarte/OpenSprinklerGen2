/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Main loop
 * Feb 2015 @ OpenSprinkler.com
 *
 * This file is part of the OpenSprinkler Firmware
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <limits.h>

#include "OpenSprinkler.h"
#include "program.h"
#include "weather.h"

#if defined(ARDUINO)
#include "SdFat.h"
#include "Wire.h"
byte Ethernet::buffer[ETHER_BUFFER_SIZE]; // Ethernet packet buffer
SdFat sd;                                 // SD card object

void reset_all_stations();
unsigned long getNtpTime();
void manual_start_program(byte pid);
#else // header and defs for RPI/BBB
#include <sys/stat.h>
#include "etherport.h"
#include "server.h"
char ether_buffer[ETHER_BUFFER_SIZE];
EthernetServer *m_server = 0;
EthernetClient *m_client = 0;
#endif

// Some perturbations have been added to the timing values below
// to avoid two events happening too clost to each other
// This is because Arduino is not good at handling multiple
// web requests at the same time
#define NTP_SYNC_INTERVAL       86403L  // NYP sync interval, 24 hrs
#define RTC_SYNC_INTERVAL       60      // RTC sync interval, 60 secs
#define CHECK_NETWORK_INTERVAL  601     // Network checking timeout, 10 minutes
#define CHECK_WEATHER_TIMEOUT   3601    // Weather check interval: 1 hour
#define CHECK_WEATHER_SUCCESS_TIMEOUT 86433L // Weather check success interval: 24 hrs
#define LCD_BACKLIGHT_TIMEOUT   15      // LCD backlight timeout: 15 secs
#define PING_TIMEOUT            200     // Ping test timeout: 200 ms

extern char tmp_buffer[];       // scratch buffer
extern const char wtopts_filename[];
BufferFiller bfill;                       // buffer filler

// ====== Object defines ======
OpenSprinkler os; // OpenSprinkler object
ProgramData pd;   // ProgramdData object

#if defined(ARDUINO)

// ====== UI defines ======
static char ui_anim_chars[3] = {'.', 'o', 'O'};

#define UI_STATE_DEFAULT   0
#define UI_STATE_DISP_IP   1
#define UI_STATE_DISP_GW   2
#define UI_STATE_RUNPROG   3

static byte ui_state = UI_STATE_DEFAULT;
static byte ui_state_runprog = 0;

void ui_state_machine() {

  if (!os.button_timeout) {
    os.lcd_set_brightness(0);
    ui_state = UI_STATE_DEFAULT;  // also recover to default state
  }

  // read button, if something is pressed, wait till release
  byte button = os.button_read(BUTTON_WAIT_HOLD);

  if (button & BUTTON_FLAG_DOWN) {   // repond only to button down events
    os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
    os.lcd_set_brightness(1);
  } else {
    return;
  }

  switch(ui_state) {
  case UI_STATE_DEFAULT:
    switch (button & BUTTON_MASK) {
    case BUTTON_1:
      if (button & BUTTON_FLAG_HOLD) {  // holding B1: stop all stations
        if (digitalRead(PIN_BUTTON_3)==0) { // if B3 is pressed while holding B1, run a short test (internal test)
          manual_start_program(255);
        } else if (digitalRead(PIN_BUTTON_2)==0) { // if B2 is pressed while holding B1, display gateway IP
          os.lcd_print_ip(ether.gwip, 0);
          os.lcd.setCursor(0, 1);
          os.lcd_print_pgm(PSTR("(gwip)"));
          ui_state = UI_STATE_DISP_IP;
        } else {
          reset_all_stations();
        }
      } else {  // clicking B1: display device IP and port
        os.lcd_print_ip(ether.myip, 0);
        os.lcd.setCursor(0, 1);
        os.lcd_print_pgm(PSTR(":"));
        os.lcd.print(ether.hisport);
        os.lcd_print_pgm(PSTR(" (osip)"));
        ui_state = UI_STATE_DISP_IP;
      }
      break;
    case BUTTON_2:
      if (button & BUTTON_FLAG_HOLD) {  // holding B2: reboot
        if (digitalRead(PIN_BUTTON_1)==0) { // if B1 is pressed while holding B2, display external IP
          os.lcd_print_ip((byte*)(&os.nvdata.external_ip), 1);
          os.lcd.setCursor(0, 1);
          os.lcd_print_pgm(PSTR("(eip)"));
          ui_state = UI_STATE_DISP_IP;
        } else if (digitalRead(PIN_BUTTON_3)==0) {  // if B3 is pressed while holding B2, display last successful weather call
          os.lcd.clear();
          os.lcd_print_time(os.checkwt_success_lasttime);
          os.lcd.setCursor(0, 1);
          os.lcd_print_pgm(PSTR("(lswc)"));
          ui_state = UI_STATE_DISP_IP;          
        } else { 
          os.reboot_dev();
        }
      } else {  // clicking B2: display MAC and gate way IP
        os.lcd.clear();
        os.lcd_print_mac(ether.mymac);
        ui_state = UI_STATE_DISP_GW;
      }
      break;
    case BUTTON_3:
      if (button & BUTTON_FLAG_HOLD) {  // holding B3: go to main menu
        os.lcd_print_line_clear_pgm(PSTR("Run a Program:"), 0);
        os.lcd_print_line_clear_pgm(PSTR("Click B3 to list"), 1);
        ui_state = UI_STATE_RUNPROG;
      } else {  // clicking B3: switch board display (cycle through master and all extension boards)
        os.status.display_board = (os.status.display_board + 1) % (os.nboards);
      }
      break;
    }
    break;
  case UI_STATE_DISP_IP:
  case UI_STATE_DISP_GW:
    ui_state = UI_STATE_DEFAULT;
    break;
  case UI_STATE_RUNPROG:
    if ((button & BUTTON_MASK)==BUTTON_3) {
      if (button & BUTTON_FLAG_HOLD) {
        // start
        manual_start_program(ui_state_runprog);
        ui_state = UI_STATE_DEFAULT;
      } else {
        ui_state_runprog = (ui_state_runprog+1) % (pd.nprograms+1);
        os.lcd_print_line_clear_pgm(PSTR("Hold B3 to start"), 0);
        if(ui_state_runprog > 0) {
          ProgramStruct prog;
          pd.read(ui_state_runprog-1, &prog);
          os.lcd_print_line_clear_pgm(PSTR(" "), 1);
          os.lcd.setCursor(0, 1);
          os.lcd.print((int)ui_state_runprog);
          os.lcd_print_pgm(PSTR(". "));
          os.lcd.print(prog.name);
        } else {
          os.lcd_print_line_clear_pgm(PSTR("0. Test (1 min)"), 1);
        }
      }
    }
    break;
  }
}

volatile ulong flow_count = 0;
// Flow sensor interrupt service routine
void flow_isr() {
  ulong curr = millis();
  if(curr-os.sensor_lasttime < 50) return;  // debounce threshold: 50ms
  flow_count++;
  os.sensor_lasttime = curr;
}

// ======================
// Setup Function
// ======================
void do_setup() {
  /* Clear WDT reset flag. */
  MCUSR &= ~(1<<WDRF);

  DEBUG_BEGIN(9600);
  DEBUG_PRINTLN("started.");
  os.begin();          // OpenSprinkler init
  os.options_setup();  // Setup options

  pd.init();            // ProgramData init

  setSyncInterval(RTC_SYNC_INTERVAL);  // RTC sync interval
  // if rtc exists, sets it as time sync source
  setSyncProvider(RTC.get);
  delay(500);
  os.lcd_print_time(os.now_tz());  // display time to LCD

  // enable WDT
  /* In order to change WDE or the prescaler, we need to
   * set WDCE (This will allow updates for 4 clock cycles).
   */
  WDTCSR |= (1<<WDCE) | (1<<WDE);
  /* set new watchdog timeout prescaler value */
  WDTCSR = 1<<WDP3 | 1<<WDP0;  // 8.0 seconds
  /* Enable the WD interrupt (note no reset). */
  WDTCSR |= _BV(WDIE);

  // set up flow sensor
  if (os.options[OPTION_SENSOR_TYPE].value==SENSOR_TYPE_FLOW) {
    attachInterrupt(PIN_FLOWSENSOR_INT, flow_isr, FALLING);
  }
  if (os.start_network()) {  // initialize network
    os.status.network_fails = 0;
  } else {
    os.status.network_fails = 1;
  }
  os.status.req_network = 0;
  os.status.req_ntpsync = 1;
  delay(500);

  os.apply_all_station_bits(); // reset station bits

  os.button_timeout = LCD_BACKLIGHT_TIMEOUT;
}

// Arduino software reset function
void(* sysReset) (void) = 0;

volatile byte wdt_timeout = 0;
// WDT interrupt service routine
ISR(WDT_vect)
{
  wdt_timeout += 1;
  // this isr is called every 8 seconds
  if (wdt_timeout > 15) {
    // reset after 120 seconds of timeout
    sysReset();
  }
}

#else
void do_setup() {
  os.begin();          // OpenSprinkler init
  os.options_setup();  // Setup options

  pd.init();            // ProgramData init
  if (os.start_network()) {  // initialize network
    DEBUG_PRINTLN("network established.");
    os.status.network_fails = 0;
  } else {
    DEBUG_PRINTLN("network failed.");
    os.status.network_fails = 1;
  }
  os.status.req_network = 0;
}
#endif

void write_log(byte type, ulong curr_time);
void schedule_all_stations(ulong curr_time);
void turn_off_station(byte sid, ulong curr_time);
void process_dynamic_events(ulong curr_time);
void check_network();
void check_weather();
void perform_ntp_sync();
void delete_log(char *name);
void handle_web_request(char *p);

/** Main Loop */
void do_loop()
{
  static ulong last_time = 0;
  static ulong last_minute = 0;

  byte bid, sid, s, pid, qid, bitvalue;
  ProgramStruct prog;

  os.status.mas = os.options[OPTION_MASTER_STATION].value;
  os.status.mas2= os.options[OPTION_MASTER_STATION_2].value;
  time_t curr_time = os.now_tz();

  // ====== Process Ethernet packets ======
#if defined(ARDUINO)  // Process Ethernet packets for Arduino
  uint16_t pos=ether.packetLoop(ether.packetReceive());
  if (pos>0) {  // packet received
    handle_web_request((char*)Ethernet::buffer+pos);
  }
  wdt_reset();  // reset watchdog timer
  wdt_timeout = 0;

  ui_state_machine();

#else // Process Ethernet packets for RPI/BBB
  EthernetClient client = m_server->available();
  if (client) {
    while(true) {
      int len = client.read((uint8_t*) ether_buffer, ETHER_BUFFER_SIZE);
      if (len <=0) {
        if(!client.connected()) {
          break;
        } else {
          continue;
        }
      } else {
        m_client = &client;
        ether_buffer[len] = 0;  // put a zero at the end of the packet
        handle_web_request(ether_buffer);
        m_client = 0;
        break;
      }
    }
  }
#endif  // Process Ethernet packets

  // if 1 second has passed
  if (last_time != curr_time) {
    last_time = curr_time;
    if (os.button_timeout) os.button_timeout--;
    
#if defined(ARDUINO)
    if (!ui_state)
      os.lcd_print_time(os.now_tz());       // print time
#endif

    // ====== Check raindelay status ======
    if (os.status.rain_delayed) {
      if (curr_time >= os.nvdata.rd_stop_time) {  // rain delay is over
        os.raindelay_stop();
      }
    } else {
      if (os.nvdata.rd_stop_time > curr_time) {   // rain delay starts now
        os.raindelay_start();
      }
    }

    // ====== Check controller status changes and write log ======
    if (os.old_status.rain_delayed != os.status.rain_delayed) {
      if (os.status.rain_delayed) {
        // rain delay started, record time
        os.raindelay_start_time = curr_time;
      } else {
        // rain delay stopped, write log
        write_log(LOGDATA_RAINDELAY, curr_time);
      }
      os.old_status.rain_delayed = os.status.rain_delayed;
    }
    // ====== Check rain sensor status ======
    os.rainsensor_status();
    if (os.old_status.rain_sensed != os.status.rain_sensed) {
      if (os.status.rain_sensed) {
        // rain sensor on, record time
        os.sensor_lasttime = curr_time;
      } else {
        // rain sensor off, write log
        write_log(LOGDATA_RAINSENSE, curr_time);
      }
      os.old_status.rain_sensed = os.status.rain_sensed;
    }

    // ====== Schedule program data ======
    ulong curr_minute = curr_time / 60;
	uint16_t curr_day = curr_time / 86400;
    boolean match_found = false;
    RuntimeQueueStruct *q;
    
	if(curr_day != os.nvdata.last_day){
		for (byte x=0;x<2;x++) {
			os.nvdata.ethist[x] = os.nvdata.water_balance[x];
			os.nvdata.ethist[x+2] = os.nvdata.et_run_today[x];
			os.nvdata.et_run_today[x] = 0;
		}
		os.nvdata.last_day = curr_day;
		os.nvdata_save();
	}
	// since the granularity of start time is minute
    // we only need to check once every minute
    if (curr_minute != last_minute) {
      last_minute = curr_minute;
      // check through all programs
      for(pid=0; pid<pd.nprograms; pid++) {
        pd.read(pid, &prog);
        if(prog.check_match(curr_time)) {
          // program match found
          // process all selected stations
          for(sid=0;sid<os.nstations;sid++) {
            bid=sid>>3;
            s=sid&0x07;
            // skip if the station is a master station (because master cannot be scheduled independently
            if ((os.status.mas==sid+1) || (os.status.mas2==sid+1))
              continue;

            // if station has non-zero water time and the station is not disabled
            if (prog.durations[sid] && !(os.station_attrib_bits_read(ADDR_NVM_STNDISABLE+bid)&(1<<s))) {
              // water time is scaled by watering percentage
              ulong water_time = water_time_resolve(water_time_decode(prog.durations[sid]));
              // if the program is set to use weather scaling
              if (prog.use_weather) {
				  if (prog.use_et) {
					  if ((os.nvdata.water_balance[prog.pheight[sid]]-(os.nvdata.predicted_rain+os.nvdata.et_run_today[prog.pheight[sid]])) >= os.options[OPTION_ET_MIN].value) {
						  water_time = water_time * min((os.nvdata.water_balance[prog.pheight[sid]]-(os.nvdata.predicted_rain+os.nvdata.et_run_today[prog.pheight[sid]])), (os.options[OPTION_ET_MAX].value)) / 10;
					  } else {
						  water_time = 0;
					  }
				  } else {
					  byte wl = os.options[OPTION_WATER_PERCENTAGE].value;
					  water_time = water_time * wl / 100;
					  if (wl < 20 && water_time < 10) { // if water_percentage is less than 20% and water_time is less than 10 seconds
						  // do not water
						  water_time = 0;
					  }
				  }
			  }

              if (water_time) {
                // check if water time is still valid
                // because it may end up being zero after scaling
                q = pd.enqueue();
                if (q) {
                  q->st = 0;
                  q->dur = water_time;
                  q->sid = sid;
                  q->pid = pid+1;
                  match_found = true;
                } else {
                  // queue is full
                }
              }// if water_time
            }// if prog.durations[sid]
          }// for sid
		  if (prog.use_et) {
			  for (byte x=0;x<2;x++){
				  if ((os.nvdata.water_balance[x]-(os.nvdata.predicted_rain+os.nvdata.et_run_today[x])) > (os.options[OPTION_ET_MIN].value)) {
					  os.nvdata.et_run_today[x] += max(min((os.nvdata.water_balance[x]-os.nvdata.predicted_rain), os.options[OPTION_ET_MAX].value),0);
				  }
			  }
			  os.nvdata_save();
			  write_log(LOGDATA_ET, curr_time);
		  }//if use_et
        }// if check_match
      }// for pid

      // calculate start and end time
      if (match_found) {
        schedule_all_stations(curr_time);
        DEBUG_PRINT("en:");
        for(q=pd.queue;q<pd.queue+pd.nqueue;q++) {
          DEBUG_PRINT("[");
          DEBUG_PRINT(q->sid);
          DEBUG_PRINT(",");
          DEBUG_PRINT(q->dur);
          DEBUG_PRINT(",");
          DEBUG_PRINT(q->st);
          DEBUG_PRINT("]");
        }
        DEBUG_PRINTLN("");
      }
    }//if_check_current_minute

    // ====== Run program data ======
    // Check if a program is running currently
    // If so, do station run-time keeping
    if (os.status.program_busy){
      // first, go through run time queue to assign queue elements to stations
      q = pd.queue;
      qid=0;
      for(;q<pd.queue+pd.nqueue;q++,qid++) {
        sid=q->sid;
        byte sqi=pd.station_qid[sid];
        // skip if station is already assigned a queue element
        // and that queue element has an earlier start time
        if(sqi<255 && pd.queue[sqi].st<q->st) continue;
        // otherwise assign the queue element to station
        pd.station_qid[sid]=qid;
      }
      // next, go through the stations and perform time keeping
      for(bid=0;bid<os.nboards; bid++) {
        bitvalue = os.station_bits[bid];
        for(s=0;s<8;s++) {
          byte sid = bid*8+s;

          // skip master station
          if (os.status.mas == sid+1) continue;
          if (os.status.mas2== sid+1) continue;

          q = pd.queue + pd.station_qid[sid];
          // check if this station is scheduled, either running or waiting to run
          if (q->st > 0) {
            // if so, check if we should turn it off
            if (curr_time >= q->st+q->dur) {
              turn_off_station(sid, curr_time);
            }
          }
          // if current station is not running, check if we should turn it on
          if(!((bitvalue>>s)&1)) {
            if (curr_time >= q->st && curr_time < q->st+q->dur) {
              os.set_station_bit(sid, 1);

              // fix me
              // upon turning on station, process RF
              // if the station is a RF station
              if(os.station_attrib_bits_read(ADDR_NVM_STNSPE+bid)&(1<<s)) {
                // send RF on signal
                os.send_rfstation_signal(sid, true);
              }
            } //if curr_time > scheduled_start_time
          } // if current station is not running
        }//end_s
      }//end_bid

      // finally, go through the queue again and clear up elements marked for removal
      int qi;
      for(qi=pd.nqueue-1;qi>=0;qi--) {
        q=pd.queue+qi;
        if(!q->dur || curr_time>=q->st+q->dur)  pd.dequeue(qi);
      }

      // process dynamic events
      process_dynamic_events(curr_time);

      // activate / deactivate valves
      os.apply_all_station_bits();

      // check through runtime queue, calculate the last stop time of sequential stations
      pd.last_seq_stop_time = 0;
      ulong sst;
      q = pd.queue;
      for(;q<pd.queue+pd.nqueue;q++) {
        sid = q->sid;
        bid = sid>>3;
        s = sid&0x07;
        // check if any sequential station has a valid stop time
        // and the stop time must be larger than curr_time
        sst = q->st + q->dur;
        if (sst>curr_time) {
          if (os.station_attrib_bits_read(ADDR_NVM_STNSEQ+bid)&(1<<s)) {   // only need to update last_seq_stop_time for sequential stations
            pd.last_seq_stop_time = (sst>pd.last_seq_stop_time ) ? sst : pd.last_seq_stop_time;
          }
        }
      }

      // if the runtime queue is empty
      // reset all stations
      if (!pd.nqueue) {
        // turn off all stations
        os.clear_all_station_bits();
        os.apply_all_station_bits();
        // reset runtime
        pd.reset_runtime();
        // reset program busy bit
        os.status.program_busy = 0;

        // in case some options have changed while executing the program
        os.status.mas = os.options[OPTION_MASTER_STATION].value; // update master station
        os.status.mas2= os.options[OPTION_MASTER_STATION_2].value; // update master2 station
      }
    }//if_some_program_is_running

    // handle master
    if (os.status.mas>0) {
      byte mas_on_adj = os.options[OPTION_MASTER_ON_ADJ].value;
      byte mas_off_adj= os.options[OPTION_MASTER_OFF_ADJ].value;
      byte masbit = 0;
      os.station_attrib_bits_load(ADDR_NVM_MAS_OP, (byte*)tmp_buffer);  // tmp_buffer now stores masop_bits
      for(sid=0;sid<os.nstations;sid++) {
        // skip if this is the master station
        if (os.status.mas == sid+1) continue;
        bid = sid>>3;
        s = sid&0x07;
        // if this station is running and is set to activate master
        if ((os.station_bits[bid]&(1<<s)) && (tmp_buffer[bid]&(1<<s))) {
          q=pd.queue+pd.station_qid[sid];
          // check if timing is within the acceptable range
          if (curr_time >= q->st + mas_on_adj &&
              curr_time <= q->st + q->dur + mas_off_adj - 60) {
            masbit = 1;
            break;
          }
        }
      }
      os.set_station_bit(os.status.mas-1, masbit);
    }
    // handle master2
    if (os.status.mas2>0) {
      byte mas_on_adj_2 = os.options[OPTION_MASTER_ON_ADJ_2].value;
      byte mas_off_adj_2= os.options[OPTION_MASTER_OFF_ADJ_2].value;
      byte masbit2 = 0;
      os.station_attrib_bits_load(ADDR_NVM_MAS_OP_2, (byte*)tmp_buffer);  // tmp_buffer now stores masop2_bits
      for(sid=0;sid<os.nstations;sid++) {
        // skip if this is the master station
        if (os.status.mas2 == sid+1) continue;
        bid = sid>>3;
        s = sid&0x07;
        // if this station is running and is set to activate master
        if ((os.station_bits[bid]&(1<<s)) && (tmp_buffer[bid]&(1<<s))) {
          q=pd.queue+pd.station_qid[sid];
          // check if timing is within the acceptable range
          if (curr_time >= q->st + mas_on_adj_2 &&
              curr_time <= q->st + q->dur + mas_off_adj_2 - 60) {
            masbit2 = 1;
            break;
          }
        }
      }
      os.set_station_bit(os.status.mas2-1, masbit2);
    }    

    // process dynamic events
    process_dynamic_events(curr_time);

    // activate/deactivate valves
    os.apply_all_station_bits();

#if defined(ARDUINO)
    // process LCD display
    if (!ui_state)
      os.lcd_print_station(1, ui_anim_chars[curr_time%3]);
    
    // check safe_reboot condition
    if (os.status.safe_reboot) {
      // if no program is running at the moment
      if (!os.status.program_busy) {
        // and if no program is scheduled to run in the next minute
        bool willrun = false;
        for(pid=0; pid<pd.nprograms; pid++) {
          pd.read(pid, &prog);
          if(prog.check_match(curr_time+60)) {
            willrun = true;
            break;
          }
        }
        if (!willrun) {
          os.reboot_dev();
        }
      }
    }
#endif

    // perform ntp sync
    if (curr_time % NTP_SYNC_INTERVAL == 0)
      os.status.req_ntpsync = 1;
    perform_ntp_sync();

    // check network connection
    if (curr_time % CHECK_NETWORK_INTERVAL==0)
      os.status.req_network = 1;
    check_network();

    // check weather
    check_weather();

  }

  #if !defined(ARDUINO)
    usleep(1000);
  #endif
}

void check_weather() {
  // do not check weather if the Use Weather option is disabled, or if network is not available, or if a program is running
  if (os.status.network_fails>0 || os.status.program_busy) return;

  ulong ntz = os.now_tz();
  if (os.checkwt_success_lasttime && (ntz > os.checkwt_success_lasttime + CHECK_WEATHER_SUCCESS_TIMEOUT)) {
    // if weather check has failed to return for too long, restart network
    os.checkwt_success_lasttime = 0;
    // mark for safe restart
    os.status.safe_reboot = 1;
    return;
  }
  if (!os.checkwt_lasttime || (ntz > os.checkwt_lasttime + CHECK_WEATHER_TIMEOUT)) {
    os.checkwt_lasttime = ntz;
    GetWeather();
	write_log(LOGDATA_ET, ntz);
    write_log(LOGDATA_WATERLEVEL, ntz); // warning: water level may update a few seconds after getweather is called
  }
}

void turn_off_station(byte sid, ulong curr_time) {
  os.set_station_bit(sid, 0);

  byte qid = pd.station_qid[sid];
  // ignore if we are turning off a station that's not running or scheduled to run
  if (qid>=pd.nqueue)  return;

  RuntimeQueueStruct *q = pd.queue+qid;

  // check if the current time is past the scheduled start time,
  // because we may be turning off a station that hasn't started yet
  if (curr_time > q->st) {
    // record lastrun log (only for non-master stations)
    if(os.status.mas!=(sid+1) && os.status.mas2!=(sid+1)) {
      pd.lastrun.station = sid;
      pd.lastrun.program = q->pid;
      pd.lastrun.duration = curr_time - q->st;
      pd.lastrun.endtime = curr_time;
      write_log(LOGDATA_STATION, curr_time);
    }

    // upon turning off station, process RF station
    // if the station is a RF station
    // fix me
    byte bid = sid>>3;
    byte s = sid&0x07;
    if(os.station_attrib_bits_read(ADDR_NVM_STNSPE+bid)&(1<<s)) {
      // turn off station
      os.send_rfstation_signal(sid, false);
    }
  }

  // dequeue the element
  pd.dequeue(qid);
  pd.station_qid[sid] = 0xFF;
}

void process_dynamic_events(ulong curr_time) {
  // check if rain is detected
  bool rain = false;
  if (os.status.rain_delayed || (os.status.rain_sensed && os.options[OPTION_SENSOR_TYPE].value == SENSOR_TYPE_RAIN)) {
    rain = true;
  }

  byte sid, s, bid, rbits, sbits;
  for(bid=0;bid<os.nboards;bid++) {
    rbits = os.station_attrib_bits_read(ADDR_NVM_IGNRAIN+bid);
    sbits = os.station_bits[bid];
    for(s=0;s<8;s++) {
      sid=bid*8+s;

      // ignore master stations because they are handled separately      
      if (os.status.mas == sid+1) continue;
      if (os.status.mas2== sid+1) continue;      
      // If this is a normal program (not a run-once or test program)
      // and either the controller is disabled, or
      // if raining and ignore rain bit is cleared
      // fix me
      /*
      if ((pd.scheduled_program_index[sid]<99) &&
          (!en || (rain && !(rbits&(1<<s)))) ) {
        if (sbits&(1<<s)) { // if station is currently running
          turn_off_station(sid, curr_time);

        } else if (pd.scheduled_program_index[sid] > 0) { // if station is currently not running but is waiting to run

          // reset program data variables
          pd.scheduled_start_time[sid] = 0;
          pd.scheduled_stop_time[sid] = 0;
          pd.scheduled_program_index[sid] = 0;
        }
      }*/
    }
  }
}

void schedule_all_stations(ulong curr_time) {
  ulong con_start_time = curr_time + 1;   // concurrent start time
  ulong seq_start_time = con_start_time;  // sequential start time

  int16_t station_delay = water_time_decode_signed(os.options[OPTION_STATION_DELAY_TIME].value);
  // if the sequential queue has stations running
  if (pd.last_seq_stop_time > curr_time) {
    seq_start_time = pd.last_seq_stop_time + station_delay;
  }

  RuntimeQueueStruct *q = pd.queue;
  // go through runtime queue and calculate start time of each station
  for(;q<pd.queue+pd.nqueue;q++) {
    if(q->st) continue; // if this queue element has already been scheduled, skip
    if(!q->dur) continue; // if the element has been marked to reset, skip
    byte sid=q->sid;
    byte bid=sid>>3;
    byte s=sid&0x07;

    // check if this is a sequential station
    if (os.station_attrib_bits_read(ADDR_NVM_STNSEQ+bid)&(1<<s)) {
      // sequential scheduling
      q->st = seq_start_time;
      seq_start_time += q->dur;
      seq_start_time += station_delay; // add station delay time
    } else {
      // concurrent scheduling
      q->st = con_start_time;
    }
    DEBUG_PRINT("[");
    DEBUG_PRINT(sid);
    DEBUG_PRINT(":");
    DEBUG_PRINT(q->st);
    DEBUG_PRINT(",");
    DEBUG_PRINT(q->dur);
    DEBUG_PRINTLN("]");
    os.status.program_busy = 1;  // set program busy bit
  }
}

void reset_all_stations_immediate() {
  os.clear_all_station_bits();
  os.apply_all_station_bits();
  pd.reset_runtime();
}

void reset_all_stations() {
  RuntimeQueueStruct *q = pd.queue;
  // go through runtime queue and assign water time to 0
  for(;q<pd.queue+pd.nqueue;q++) {
    q->dur = 0;
  }
}


// Manually start a program
// If pid==0, this is a test program (1 minute per station)
// If pid==255, this is a short test program (2 second per station)
// If pid > 0. run program pid-1
void manual_start_program(byte pid) {
  boolean match_found = false;
  reset_all_stations_immediate();
  ProgramStruct prog;
  ulong dur;
  byte sid, bid, s;
  if ((pid>0)&&(pid<255)) {
    pd.read(pid-1, &prog);
  }
  for(sid=0;sid<os.nstations;sid++) {
    bid=sid>>3;
    s=sid&0x07;
    dur = 60;
    if(pid==255)  dur=2;
    else if(pid>0)
      dur = water_time_resolve(water_time_decode(prog.durations[sid]));
    RuntimeQueueStruct *q = pd.enqueue();
    if (q && dur>0 && !(os.station_attrib_bits_read(ADDR_NVM_STNDISABLE+bid)&(1<<s))) {
      q->st = 0;
      q->dur = dur;
      q->sid = sid;
      q->pid = 254;
      match_found = true;
    }
  }
  if(match_found) {
    schedule_all_stations(os.now_tz());
  }
}

// ================================
// ====== LOGGING FUNCTIONS =======
// ================================
// Log files will be named /logs/xxxxx.txt
#if defined(ARDUINO)
char LOG_PREFIX[] = "/logs/";
#else
char LOG_PREFIX[] = "./logs/";
#endif

void make_logfile_name(char *name) {
#if defined(ARDUINO)
  sd.chdir("/");
#endif
  strcpy(tmp_buffer+TMP_BUFFER_SIZE-10, name);
  strcpy(tmp_buffer, LOG_PREFIX);
  strcat(tmp_buffer, tmp_buffer+TMP_BUFFER_SIZE-10);
  strcat_P(tmp_buffer, PSTR(".txt"));
}

const char *log_type_names[] = {
  "",
  "rs",
  "rd",
  "wl",
  "et"
};

// write run record to log on SD card
void write_log(byte type, ulong curr_time) {
  if (!os.options[OPTION_ENABLE_LOGGING].value) return;

  // file name will be logs/xxxxx.tx where xxxxx is the day in epoch time
  ultoa(curr_time / 86400, tmp_buffer, 10);
  make_logfile_name(tmp_buffer);

#if defined(ARDUINO) // prepare log folder for Arduino
  if (!os.status.has_sd)  return;

  sd.chdir("/");
  if (sd.chdir(LOG_PREFIX) == false) {
    // create dir if it doesn't exist yet
    if (sd.mkdir(LOG_PREFIX) == false) {
      return;
    }
  }
  SdFile file;
  int ret = file.open(tmp_buffer, O_CREAT | O_WRITE );
  file.seekEnd();
  if(!ret) {
    return;
  }
#else // prepare log folder for RPI/BBB
  struct stat st;
  if(stat(LOG_PREFIX, &st)) {
    if(mkdir(LOG_PREFIX, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH)) {
      return;
    }
  }
  FILE *file;
  file = fopen(tmp_buffer, "rb+");
  if(!file) {
    file = fopen(tmp_buffer, "wb");
    if (!file)  return;
  }
  fseek(file, 0, SEEK_END);
#endif  // prepare log folder

  strcpy_P(tmp_buffer, PSTR("["));

  if(type == LOGDATA_STATION) {
    itoa(pd.lastrun.program, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    itoa(pd.lastrun.station, tmp_buffer+strlen(tmp_buffer), 10);
    strcat_P(tmp_buffer, PSTR(","));
    itoa(pd.lastrun.duration, tmp_buffer+strlen(tmp_buffer), 10);
  } else {
    strcat_P(tmp_buffer, PSTR("0,\""));
    strcat(tmp_buffer, log_type_names[type]);
    strcat_P(tmp_buffer, PSTR("\","));
    switch(type) {
      case LOGDATA_RAINSENSE:
        ultoa((curr_time - os.sensor_lasttime), tmp_buffer+strlen(tmp_buffer), 10);
        break;
      case LOGDATA_RAINDELAY:
        ultoa((curr_time - os.raindelay_start_time), tmp_buffer+strlen(tmp_buffer), 10);
        break;
      case LOGDATA_WATERLEVEL:
        itoa(os.options[OPTION_WATER_PERCENTAGE].value, tmp_buffer+strlen(tmp_buffer), 10);
        break;
	  case LOGDATA_ET:
	    itoa(os.nvdata.water_balance[0], tmp_buffer+strlen(tmp_buffer), 10);
		strcat_P(tmp_buffer, PSTR(","));
		itoa(os.nvdata.water_balance[1], tmp_buffer+strlen(tmp_buffer), 10);
		strcat_P(tmp_buffer, PSTR(","));
		itoa(os.nvdata.et_run_today[0], tmp_buffer+strlen(tmp_buffer), 10);
		strcat_P(tmp_buffer, PSTR(","));
		itoa(os.nvdata.et_run_today[1], tmp_buffer+strlen(tmp_buffer), 10);
		break;
		
    }
  }
  strcat_P(tmp_buffer, PSTR(","));
  ultoa(curr_time, tmp_buffer+strlen(tmp_buffer), 10);
  strcat_P(tmp_buffer, PSTR("]\r\n"));

#if defined(ARDUINO)
  file.write(tmp_buffer);
  file.close();
#else
  fwrite(tmp_buffer, 1, strlen(tmp_buffer), file);
  fclose(file);
#endif
}


// delete log file
// if name is 'all', delete all logs
void delete_log(char *name) {
  if (!os.options[OPTION_ENABLE_LOGGING].value) return;
#if defined(ARDUINO)
  if (!os.status.has_sd) return;

  if (strncmp(name, "all", 3) == 0) {
    // delete the log folder
    SdFile file;

    if (sd.chdir(LOG_PREFIX)) {
      // delete the whole log folder
      sd.vwd()->rmRfStar();
    }
    return;
  } else {
    make_logfile_name(name);
    if (!sd.exists(tmp_buffer))  return;
    sd.remove(tmp_buffer);
  }
#else // delete_log implementation for RPI/BBB
  if (strncmp(name, "all", 3) == 0) {
    // delete the log folder
    rmdir(LOG_PREFIX);
    return;
  } else {
    make_logfile_name(name);
    remove(tmp_buffer);
  }
#endif
}

void check_network() {
#if defined(ARDUINO)
  // do not perform network checking if the controller has just started, or if a program is running
  if (os.status.program_busy) {return;}

  // check network condition periodically
  if (os.status.req_network) {
    os.status.req_network = 0;
    // change LCD icon to indicate it's checking network
    if (!ui_state) {
      os.lcd.setCursor(15, 1);
      os.lcd.write(4);
    }

    // ping gateway ip
    ether.clientIcmpRequest(ether.gwip);

    ulong start = millis();
    boolean failed = true;
    // wait at most PING_TIMEOUT milliseconds for ping result
    do {
      ether.packetLoop(ether.packetReceive());
      if (ether.packetLoopIcmpCheckReply(ether.gwip)) {
        failed = false;
        break;
      }
    } while(millis() - start < PING_TIMEOUT);
    if (failed)  {
      os.status.network_fails++;
      // clamp it to 6
      if (os.status.network_fails > 6) os.status.network_fails = 6;
    }
    else os.status.network_fails=0;
    // if failed more than 6 times, restart
    if (os.status.network_fails>=6) {
      // mark for safe restart
      os.status.safe_reboot = 1;
    } else if (os.status.network_fails>2) {
      // if failed more than twice, try to reconnect    
      if (os.start_network())
        os.status.network_fails=0;
    }
  }
#else
  // nothing to do here
  // Linux will do this for you
#endif
}

void perform_ntp_sync() {
#if defined(ARDUINO)
  // do not perform sync if this option is disabled, or if network is not available, or if a program is running
  if (!os.options[OPTION_USE_NTP].value || os.status.network_fails>0 || os.status.program_busy) return;

  if (os.status.req_ntpsync) {
    os.status.req_ntpsync = 0;
    if (!ui_state) {
      os.lcd_print_line_clear_pgm(PSTR("NTP Syncing..."),1);
    }
    ulong t = getNtpTime();
    if (t>0) {
      setTime(t);
      RTC.set(t);
    }
  }
#else
  // nothing to do here
  // Linux will do this for you
#endif
}

#if !defined(ARDUINO) // main function for RPI/BBB
int main(int argc, char *argv[]) {
  do_setup();

  while(true) {
    do_loop();
  }
  return 0;
}
#endif
