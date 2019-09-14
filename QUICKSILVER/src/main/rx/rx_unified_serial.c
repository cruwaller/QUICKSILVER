#include <stdbool.h>
#include <stdio.h>
#include "defines.h"
#include "drv_fmc.h"
#include "drv_rx_serial.h"
#include "drv_serial.h"
#include "drv_time.h"
#include "drv_uart.h"
#include "profile.h"
#include "project.h"
#include "util.h"
#include "led.h"
#include "rx.h"
#include "debug.h"

// sbus input ( pin SWCLK after calibration)
// WILL DISABLE PROGRAMMING AFTER GYRO CALIBRATION - 2 - 3 seconds after powerup)

#ifdef RX_UNIFIED_SERIAL

extern debug_type debug;






// global use rx variables
extern float rx[4];
extern char aux[AUX_CHANNEL_MAX];
extern char lastaux[AUX_CHANNEL_MAX];
extern char auxchange[AUX_CHANNEL_MAX];
uint8_t rxusart = 1;
int failsafe = 9001; //OVER 9000
int rxmode = 0;
int rx_ready = 0;

// internal variables
uint8_t RXProtocol = 0;
#define RX_BUFF_SIZE 68
uint8_t rx_buffer[RX_BUFF_SIZE];
uint8_t rx_end = 0;
uint16_t rx_time[RX_BUFF_SIZE];
uint8_t rx_data[RX_BUFF_SIZE]; //A place to put the RX frame so nothing can get overwritten during processing.
uint8_t bytesSinceStart = 0;
int frameStatus = -1;
uint8_t frameStart = 0;
uint8_t frameEnd = 0;
uint8_t telemetryCounter = 0;
uint8_t expectedFrameLength = 10;
uint8_t escapedChars = 0;
uint8_t rx_bind_enable = 0;

unsigned long time_lastrx;
unsigned long time_siglost;
uint8_t last_rx_end = 0;
unsigned long time_lastframe;
int rx_state = 0;
int bind_safety = 0;
int channels[16];
uint16_t CRCByte = 0; //Defined here to allow Debug to see it.
uint8_t protocolToCheck = 1;//Defined here to allow Debug to see it.
uint8_t protocolDetectTimer = 0;//Defined here to allow Debug to see it.


int failsafe_sbus_failsafe = 0;
int failsafe_siglost = 0;
int failsafe_noframes = 0;

uint8_t stat_frames_second;
uint32 ticksStart = 0;
uint32 ticksEnd = 0;
uint32 ticksLongest = 0;
int bobnovas = 0;

//Telemetry variables

//Global values to send as telemetry
bool FPORTDebugTelemetry = false;
extern float vbattfilt;
extern float vbatt_comp;
extern unsigned int lastlooptime;
uint8_t telemetryPacket[10];
extern int current_pid_axis;
extern int current_pid_term;
extern profile_t profile;


uint16_t SbusTelemetryIDs[] = {
  0x0210, //VFAS, use for vbat_comp
  0x0211, //VFAS1, use for vbattfilt
  //Everything past here is only active in FPORT-Debug-Telemetry mode
  0x0900, //A3_FIRST_ID, used for cell count
  0x0400, //T1, used for Axis Identifier
  0x0700, //ACC-X, misused for PID-P
  0x0710, //ACC-X, misused for PID-I
  0x0720, //ACC-X, misused for PID-D
};
uint8_t telemetryPosition = 0; //This iterates through the above, you can only send one sensor per frame.
uint8_t teleCounter = 0;





void RX_USART_ISR(void) {
  rx_buffer[rx_end] = USART_ReceiveData(USART1);
  // calculate timing since last rx
  unsigned long maxticks = SysTick->LOAD;
  unsigned long ticks = SysTick->VAL;
  unsigned long elapsedticks;
  static unsigned long lastticks;
  if (ticks < lastticks)
    elapsedticks = lastticks - ticks;
  else { // overflow ( underflow really)
    elapsedticks = lastticks + (maxticks - ticks);
  }

  if (elapsedticks < 65536) {
    rx_time[rx_end] = elapsedticks; //
  }
  else {
    rx_time[rx_end] = 65535; //ffff
  }

  lastticks = ticks;

  if (USART_GetFlagStatus(USART1, USART_FLAG_ORE)) {
    // overflow means something was lost
    rx_time[rx_end] = 0xFFFe;
    USART_ClearFlag(USART1, USART_FLAG_ORE);
  }

  bytesSinceStart++;
  if (rx_time[rx_end] > 9000) { //Long delay since last byte. Probably a new frame, or we missed some bytes. Either way, start over.
    frameStart = rx_end;
    bytesSinceStart = 1;
    frameStatus = 0; //0 is "frame in progress or first frame not arrived",
    //1 is "frame ready(ish) to be read, or at least checked",
    //2 is "Frame looks complete, CRC it and read controls",
  }                  //3 is "frame already complete and processed, do nothing (or send telemetry if enabled)"
  else if (bytesSinceStart >= expectedFrameLength && frameStatus == 0) { //It's long enough to be a full frame, and we're waiting for a full frame. Check it!
    frameStatus = 1;
  }

  rx_end++;
  rx_end %= (RX_BUFF_SIZE);
  bobnovas += 100;
}

void rx_init(void) {

}

void rx_serial_init(void) {

  //RXProtocol = 4;  //Remove meeeeeeeee

  frameStatus = 0; //Let the uart ISR do its stuff.
  if (RXProtocol == 0) { //No known protocol? Autodetect!
    findprotocol();
  }
  else {
    usart_rx_init(RXProtocol); //There's already a known protocol, we're good.
  }
  switch (RXProtocol)
  {
    case 1: // DSM
      expectedFrameLength = 16;
      break;
    case 2: // SBUS
      expectedFrameLength = 24;
      break;
    case 3: // IBUS
      expectedFrameLength = 32;
      break;
    case 4: // FPORT
      expectedFrameLength = 28; //Minimum.
      break;
    case 5: // CRSF
      expectedFrameLength = 64; //Maybe 65? Not sure where the Sync Byte comes in
      break;

    default:
      break;
  }
}



void checkrx() {

//FAILSAFE! It gets checked every time!     FAILSAFE! It gets checked every time!     FAILSAFE! It gets checked every time!
if (gettime() - time_lastframe > 1000000)
  {
    failsafe_noframes = 1;
  } else
    failsafe_noframes = 0;

  // add the 3 failsafes together
  failsafe = failsafe_noframes || failsafe_siglost || failsafe_sbus_failsafe;


  
  if (frameStatus == 1) { //USART ISR says there's enough frame to look at. Look at it.
    switch (RXProtocol)
    {
      case 1: // DSM
        processDSMX();
        break;
      case 2: // SBUS
        bobnovas++;
        processSBUS();

        break;
      case 3: // IBUS
        processIBUS();
        break;
      case 4: // FPORT
        processFPORT();
        break;
      case 5: // CRSF
        processCRSF();
        break;

      default:
        break;
    }
  }
  else if(frameStatus == 3){
    switch (RXProtocol)
    {
      case 1: // DSM
        // Run DSM Telemetry
        break;
      case 2: // SBUS

        //Run smartport telemetry

        break;
      case 3: // IBUS
        //IBUS Telemetry function call goes here
        break;
      case 4: // FPORT
        sendFPORTTelemetry();
        break;
      case 5: // CRSF
        //CRSF telemetry function call yo
        break;

      default:
        break;
    }
  }
  else if (frameStatus == -1) { //RX/USART not set up.
    rx_serial_init(); //Set it up. This includes autodetecting protocol if necesary
    rxmode = !RXMODE_BIND;
  }
  
}



void processDSMX(void) {

}


void processSBUS(void) {
  for (uint8_t counter = 0; counter < 25; counter++) {                //First up, get therx_data out of the RX buffer and into somewhere safe
    rx_data[counter] = rx_buffer[(counter + frameStart) % RX_BUFF_SIZE]; // This can probably go away, as long as the buffer is large enough
  }

  if (rx_data[23] & (1 << 2)) //RX sets this bit when it knows it missed a frame. Presumably this is a timer in the RX.
  {
    if (!time_siglost)
      time_siglost = gettime();
    if (gettime() - time_siglost > TICK_CLOCK_FREQ_HZ) //8,000,000 ticks on F0, 21M on F4. One second.
    {
      failsafe_siglost = 1;
    }
  } else {
    time_siglost = 0;
    failsafe_siglost = 0;
  }
  if (rx_data[23] & (1 << 3)) {
    failsafe_sbus_failsafe = 1; // Sbus packets have a failsafe bit. This is cool. If you forget to trust it you get programs though.
  } else {
    failsafe_sbus_failsafe = 0;
  }

  //Sbus channels, this could be a struct and a memcpy but eh.
  //All 16 are decoded, even if Quicksilver doesn't want to use them.
  channels[0] = ((rx_data[1] | rx_data[2] << 8) & 0x07FF);
  channels[1] = ((rx_data[2] >> 3 | rx_data[3] << 5) & 0x07FF);
  channels[2] = ((rx_data[3] >> 6 | rx_data[4] << 2 | rx_data[5] << 10) & 0x07FF);
  channels[3] = ((rx_data[5] >> 1 | rx_data[6] << 7) & 0x07FF);
  channels[4] = ((rx_data[6] >> 4 | rx_data[7] << 4) & 0x07FF);
  channels[5] = ((rx_data[7] >> 7 | rx_data[8] << 1 | rx_data[9] << 9) & 0x07FF);
  channels[6] = ((rx_data[9] >> 2 | rx_data[10] << 6) & 0x07FF);
  channels[7] = ((rx_data[10] >> 5 | rx_data[11] << 3) & 0x07FF);
  channels[8] = ((rx_data[12] | rx_data[11] << 8) & 0x07FF);
  channels[9] = ((rx_data[13] >> 3 | rx_data[14] << 5) & 0x07FF); //This is the last channel Silverware previously supported.
  channels[10] = ((rx_data[14] >> 6 | rx_data[15] << 2 | rx_data[16] << 10) & 0x07FF);
  channels[11] = ((rx_data[16] >> 1 | rx_data[17] << 7) & 0x07FF);
  channels[12] = ((rx_data[17] >> 4 | rx_data[18] << 4) & 0x07FF);
  channels[13] = ((rx_data[18] >> 7 | rx_data[19] << 1 | rx_data[20] << 9) & 0x07FF);
  channels[14] = ((rx_data[20] >> 2 | rx_data[21] << 6) & 0x07FF);
  channels[15] = ((rx_data[21] >> 5 | rx_data[22] << 3) & 0x07FF);

  if (rx_state == 0) //Stay in failsafe until we've received a stack of frames AND throttle is under 10% or so
  {
    // wait for valid sbus signal
    static int frame_count = 0;
    failsafe = 1;
    rxmode = RXMODE_BIND;
    // if throttle < 10%
    if (channels[2] < 300)
      frame_count++; //AETR!
    if (frame_count > 130) {
      if (stat_frames_second > 30) {
        rx_state++;
        rxmode = !RXMODE_BIND;
      } else {
        frame_count = 0;
      }
    }
  }

  if (rx_state == 1) {
    // normal rx mode

    // AETR channel order
    channels[0] -= 993;
    channels[1] -= 993;
    channels[3] -= 993;

    rx[0] = channels[0];
    rx[1] = channels[1];
    rx[2] = channels[3];

    for (int i = 0; i < 3; i++) {
      rx[i] *= 0.00122026f;
    }

    channels[2] -= 173;
    rx[3] = 0.000610128f * channels[2];

    if (rx[3] > 1)
      rx[3] = 1;
    if (rx[3] < 0)
      rx[3] = 0;

    rx_apply_expo();

    //Here we have the AUX channels Silverware supports
    aux[AUX_CHANNEL_0] = (channels[4] > 1600) ? 1 : 0;
    aux[AUX_CHANNEL_1] = (channels[5] > 1600) ? 1 : 0;
    aux[AUX_CHANNEL_2] = (channels[6] > 1600) ? 1 : 0;
    aux[AUX_CHANNEL_3] = (channels[7] > 1600) ? 1 : 0;
    aux[AUX_CHANNEL_4] = (channels[8] > 1600) ? 1 : 0;
    aux[AUX_CHANNEL_5] = (channels[9] > 1600) ? 1 : 0;
    //and here we have the rest of the sbus AUX channels/
    /* Currently Silverware only has six AUX channels.
        aux[AUX_CHANNEL_6] = (channels[10] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_7] = (channels[11] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_8] = (channels[12] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_9] = (channels[13] > 1600) ? 1 : 0;
    */

    time_lastframe = gettime();
    if (bind_safety > 141) { //requires one second worth of good frames to come in before rx_ready safety can be toggled to 1
      rx_ready = 1;          // because aux channels initialize low and clear the binding while armed flag before aux updates high
      bind_safety = 142;
    }
  }

  // stats
  static int fps = 0;
  static unsigned long secondtime = 0;

  if (gettime() - secondtime > 1000000) {
    stat_frames_second = fps;
    fps = 0;
    secondtime = gettime();
  }
  fps++;
  frameStatus = 3; //We're done with this frame now.
  bind_safety++;   // It was a good frame, increment the good frame counter.

} // end frame received



void processIBUS(void) {

  uint8_t frameLength = 0;
  bobnovas += 1000;
  for (uint8_t counter = 0; counter < 32; counter++) {                //First up, get the data out of the RX buffer and into somewhere safe
    rx_data[counter] = rx_buffer[(counter + frameStart) % RX_BUFF_SIZE]; // This can probably go away, as long as the buffer is large enough
    frameLength++;                                                    // to accept telemetry requests without overwriting control data
  }

  CRCByte = 0xFFFF;
  for (int x = 0; x < 30; x++) {
    CRCByte = CRCByte - rx_data[x];
  }

  if (CRCByte == rx_data[30] + (rx_data[31] << 8)) { //If the CRC is good, shove it into controls

    //Flysky channels are delightfully straightforward
    channels[0] = rx_data[2] + (rx_data[3] << 8);
    channels[1] = rx_data[4] + (rx_data[5] << 8);
    channels[2] = rx_data[6] + (rx_data[7] << 8);
    channels[3] = rx_data[8] + (rx_data[9] << 8);
    channels[4] = rx_data[10] + (rx_data[11] << 8);
    channels[5] = rx_data[12] + (rx_data[13] << 8);
    channels[6] = rx_data[14] + (rx_data[15] << 8);
    channels[7] = rx_data[16] + (rx_data[17] << 8);
    channels[8] = rx_data[18] + (rx_data[19] << 8);
    channels[9] = rx_data[20] + (rx_data[21] << 8);
    channels[10] = rx_data[22] + (rx_data[23] << 8);
    channels[11] = rx_data[24] + (rx_data[25] << 8);
    channels[12] = rx_data[26] + (rx_data[27] << 8);
    channels[13] = rx_data[28] + (rx_data[29] << 8);

    if (rx_state == 0) //Stay in failsafe until we've received a stack of frames AND throttle is under 10% or so
    {
      // wait for valid ibus signal
      static int frame_count = 0;
      failsafe = 1;
      rxmode = RXMODE_BIND;
      // if throttle < 10%
      if (channels[2] < 1100)
        frame_count++; //AETR!
      if (frame_count > 130) {
        if (stat_frames_second > 30) {
          rx_state++;
          rxmode = !RXMODE_BIND;
        } else {
          frame_count = 0;
        }
      }
    }

    if (rx_state == 1) {
      // normal rx mode

      // AETR channel order
      channels[0] -= 1500;
      channels[1] -= 1500;
      channels[2] -= 1000;
      channels[3] -= 1500;

      rx[0] = channels[0];
      rx[1] = channels[1];
      rx[2] = channels[3];
      rx[3] = channels[2];

      for (int i = 0; i < 3; i++) {
        rx[i] *= 0.002f;
      }
      rx[3] *= 0.001f;

      if (rx[3] > 1)
        rx[3] = 1;
      if (rx[3] < 0)
        rx[3] = 0;

      rx_apply_expo();

      //Here we have the AUX channels Silverware supports
      aux[AUX_CHANNEL_0] = (channels[4] > 1600) ? 1 : 0;
      aux[AUX_CHANNEL_1] = (channels[5] > 1600) ? 1 : 0;
      aux[AUX_CHANNEL_2] = (channels[6] > 1600) ? 1 : 0;
      aux[AUX_CHANNEL_3] = (channels[7] > 1600) ? 1 : 0;
      aux[AUX_CHANNEL_4] = (channels[8] > 1600) ? 1 : 0;
      aux[AUX_CHANNEL_5] = (channels[9] > 1600) ? 1 : 0;
      //and here we have the rest of the iBus AUX channels/
      /* Currently Silverware only has six AUX channels.
        aux[AUX_CHANNEL_6] = (channels[10] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_7] = (channels[11] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_8] = (channels[12] > 1600) ? 1 : 0;
        aux[AUX_CHANNEL_9] = (channels[13] > 1600) ? 1 : 0;
      */

      time_lastframe = gettime();
      if (bind_safety > 141) { //requires one second worth of good frames to come in before rx_ready safety can be toggled to 1
        rx_ready = 1;          // because aux channels initialize low and clear the binding while armed flag before aux updates high
        bind_safety = 142;
      }
    }

    // stats
    static int fps = 0;
    static unsigned long secondtime = 0;

    if (gettime() - secondtime > 1000000) {
      stat_frames_second = fps;
      fps = 0;
      secondtime = gettime();
    }
    fps++;
    frameStatus = 3; //We're done with this frame now.
    bind_safety++;   // It was a good frame, increment the good frame counter.

  } else { // if CRC fails, do this:
    //while(1){} Enable for debugging to lock the FC if CRC fails. In the air we just drop CRC-failed packets
  }
} // end frame received




void processFPORT(void) {
  uint8_t frameLength = 0;
  uint8_t tempEscapedChars = 0;
  for (uint8_t counter = 0; counter <= bytesSinceStart; counter++) {                     //First up, get the data out of the RX buffer and into somewhere safe
    rx_data[counter] = rx_buffer[(counter + tempEscapedChars + frameStart) % RX_BUFF_SIZE]; //We're going to be messing with the data and it wouldn't do to destroy a packet
    frameLength++;

    if (rx_data[counter - 1] == 0x7D) { //0x7D and 0x7E are reserved, 0x7D is the marker for a reserved / escaped character
      if (rx_data[counter] == 0x5E) {   //0x5E in the byte following 0x7D means it was a 0x7E data byte.
        rx_data[counter - 1] = 0x7E;    //So, make it 0x7E.
        escapedChars++;              //Now we have to get rid of the "current" byte, and adjust the incoming frame length.
        tempEscapedChars++;
        counter--;
        frameLength--;
      } else if (rx_data[counter] == 0x5D) {
        //data[counter - 1] = 0x7D;  It already is, this line is a reminder
        escapedChars++;
        tempEscapedChars++;
        counter--;
        frameLength--;
      }
    }

    if (rx_data[counter] == 0x7e && rx_data[counter - 1] == 0x7e && frameLength > 29) { //Looks like a complete frame, check CRC and process controls if it's good.
      frameStatus = 2;
      counter = 200; //Breaks out of the for loop processing the data array.
      //The telemetry request packet is not read, as it never seems to change. Less control lag if we ignore it.
    }
  }

  if (frameStatus == 2) { //If it looks complete, process it further.

    CRCByte = 0;
    for (int x = 1; x < frameLength - 2; x++) {
      CRCByte = CRCByte + rx_data[x];
    }
    CRCByte = CRCByte + (CRCByte >> 8);
    CRCByte = CRCByte << 8;
    CRCByte = CRCByte >> 8;
    if (CRCByte == 0x00FF) { //CRC is good, check Failsafe bit(s) and shove it into controls

      //FPORT uses SBUS style data, but starts further in the packet

      if (rx_data[25] & (1 << 2)) //RX appears to set this bit when it knows it missed a frame. How does it know?
      {
        if (!time_siglost)
          time_siglost = gettime();
        if (gettime() - time_siglost > TICK_CLOCK_FREQ_HZ) //8,000,000 ticks on F0, 21M on F4. One second.
        {
          failsafe_siglost = 1;
        }
      } else {
        time_siglost = 0;
        failsafe_siglost = 0;
      }
      if (rx_data[25] & (1 << 3)) {
        failsafe_sbus_failsafe = 1; // Sbus packets have a failsafe bit. This is cool.
      } else {
        failsafe_sbus_failsafe = 0;
      }

      channels[0] = ( (rx_data[3] | rx_data[4] << 8) & 0x07FF);
      channels[1] = ( (rx_data[4] >> 3 | rx_data[5] << 5) & 0x07FF);
      channels[2] = ( (rx_data[5] >> 6 | rx_data[6] << 2 | rx_data[7] << 10) & 0x07FF);
      channels[3] = ( (rx_data[7] >> 1 | rx_data[8] << 7) & 0x07FF);
      channels[4] = ( (rx_data[8] >> 4 | rx_data[9] << 4) & 0x07FF);
      channels[5] = ( (rx_data[9] >> 7 | rx_data[10] << 1 | rx_data[11] << 9) & 0x07FF);
      channels[6] = ( (rx_data[11] >> 2 | rx_data[12] << 6) & 0x07FF);
      channels[7] = ( (rx_data[12] >> 5 | rx_data[13] << 3) & 0x07FF);
      channels[8] = ( (rx_data[14] | rx_data[15] << 8) & 0x07FF);
      channels[9] = ( (rx_data[15] >> 3 | rx_data[16] << 5) & 0x07FF); //This is the last channel Silverware previously supported.
      channels[10] = ( (rx_data[16] >> 6 | rx_data[17] << 2 | rx_data[18] << 10) & 0x07FF);
      channels[11] = ( (rx_data[18] >> 1 | rx_data[19] << 7) & 0x07FF);
      channels[12] = ( (rx_data[19] >> 4 | rx_data[20] << 4) & 0x07FF);
      channels[13] = ( (rx_data[20] >> 7 | rx_data[21] << 1 | rx_data[22] << 9) & 0x07FF);
      channels[14] = ( (rx_data[22] >> 2 | rx_data[23] << 6) & 0x07FF);
      channels[15] = ( (rx_data[23] >> 5 | rx_data[24] << 3) & 0x07FF);

      if (rx_state == 0) //Stay in failsafe until we've received a stack of frames AND throttle is under 10% or so
      {
        // wait for valid sbus signal
        static int frame_count = 0;
        failsafe = 1;
        rxmode = RXMODE_BIND;
        // if throttle < 10%
        if (channels[2] < 336)
          frame_count++; //AETR!
        if (frame_count > 130) {
          if (stat_frames_second > 30) {
            rx_state++;
            rxmode = !RXMODE_BIND;
          } else {
            frame_count = 0;
          }
        }
      }

      if (rx_state == 1) {
        // normal rx mode

        // AETR channel order
        channels[0] -= 993;
        channels[1] -= 993;
        channels[3] -= 993;

        rx[0] = channels[0];
        rx[1] = channels[1];
        rx[2] = channels[3];
        //I don't know what any of this really does.
        for (int i = 0; i < 3; i++) {
          rx[i] *= 0.00122026f;
        }

        channels[2] -= 173;
        rx[3] = 0.000610128f * channels[2];

        if (rx[3] > 1)
          rx[3] = 1;
        if (rx[3] < 0)
          rx[3] = 0;

        //I also don't know why expo is being applied in RX code.
        rx_apply_expo();

        //Here we have the AUX channels Silverware supports
        aux[AUX_CHANNEL_0] = (channels[4] > 993) ? 1 : 0;
        aux[AUX_CHANNEL_1] = (channels[5] > 993) ? 1 : 0;
        aux[AUX_CHANNEL_2] = (channels[6] > 993) ? 1 : 0;
        aux[AUX_CHANNEL_3] = (channels[7] > 993) ? 1 : 0;
        aux[AUX_CHANNEL_4] = (channels[8] > 993) ? 1 : 0;
        aux[AUX_CHANNEL_5] = (channels[9] > 993) ? 1 : 0;
        //and here we have the rest of the FPORT/SBUS channels/
        /* Currently Silverware only has six AUX channels.
          aux[AUX_CHANNEL_6] = (channels[10] > 993) ? 1 : 0;
          aux[AUX_CHANNEL_7] = (channels[11] > 993) ? 1 : 0;
          aux[AUX_CHANNEL_8] = (channels[12] > 993) ? 1 : 0;
          aux[AUX_CHANNEL_9] = (channels[13] > 993) ? 1 : 0;
          aux[AUX_CHANNEL_9] = (channels[14] > 993) ? 1 : 0;
          aux[AUX_CHANNEL_10] = (channels[15] > 993) ? 1 : 0;
        */
        if (channels[12] > 993) { // Channel 13 is now FPORT Debug Telemetry switch. Integrate this better sometime
          FPORTDebugTelemetry = true;
        } else {
          FPORTDebugTelemetry = false;
        }

        time_lastframe = gettime();

        if (bind_safety > 111) { //requires one second worth of good frames to come in before rx_ready safety can be toggled to 1
          rx_ready = 1;          // because aux channels initialize low and clear the binding while armed flag before aux updates high
          bind_safety = 112;
        }
      }

      // stats
      static int fps = 0;
      static unsigned long secondtime = 0;

      if (gettime() - secondtime > 1000000) {
        stat_frames_second = fps;
        fps = 0;
        secondtime = gettime();
      }
      fps++;
      frameStatus = 3; //We're done with this frame now.
      bind_safety++;   // It was a good frame, increment the good frame counter.
      telemetryCounter++; // Let the telemetry section know it's time to send.
      
    } 
    else 
    { // if CRC fails, do this:
      //while(1){} Enable for debugging to lock the FC if CRC fails.
      frameStatus = 0; //Most likely reason for failed CRC is a frame that isn't fully here yet. No need to check again until a new byte comes in.
    }
  } // end frame received
}

void sendFPORTTelemetry(){

  if (telemetryCounter > 0 && bytesSinceStart >= 40) { // Send telemetry back every other packet. This gives the RX time to send ITS telemetry back
        telemetryCounter = 0;

        uint16_t telemetryIDs[] = {
    0x0210, //VFAS, use for vbat_comp
    0x0211, //VFAS1, use for vbattfilt
    //Everything past here is only active in FPORT-Debug-Telemetry mode
    0x0900, //A3_FIRST_ID, used for cell count
    0x0400, //T1, used for Axis Identifier
    0x0700, //ACC-X, misused for PID-P
    0x0710, //ACC-X, misused for PID-I
    0x0720, //ACC-X, misused for PID-D
  };
  extern float lipo_cell_count; // For telemetry

        //Telemetry time! Let's have some variables
        //uint8_t telemetryPacket[10]; //Redefining this every pass is (probably) faster than zeroing it.
        uint16_t teleCRC = 0; //Really, really want to start with a zerod CRC!

        telemetryPacket[0] = 0x08; //Bytes 0 through 2 are static in this implementation
        telemetryPacket[1] = 0x81;
        telemetryPacket[2] = 0x10;
        //Note: This does not properly escape 0x7E and 0x7D characters. Some telemetry packets will fail CRC on the other end. This will be fixed.
        if (telemetryPosition == 0) {                                //vbat_comp
          telemetryPacket[3] = telemetryIDs[telemetryPosition];      //0x10;
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8; //0x02;
          telemetryPacket[5] = (int)(vbatt_comp * 100);
          telemetryPacket[6] = (int)(vbatt_comp * 100) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        } else if (telemetryPosition == 1) {                         //vbattfilt
          telemetryPacket[3] = telemetryIDs[telemetryPosition];      //x11;
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8; //0x02;
          telemetryPacket[5] = (int)(vbattfilt * 100);
          telemetryPacket[6] = (int)(vbattfilt * 100) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        }

        else if (telemetryPosition == 2) { //Cell count
          telemetryPacket[3] = telemetryIDs[telemetryPosition];
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8;
          telemetryPacket[5] = (int)(lipo_cell_count * 100);
          telemetryPacket[6] = (int)(lipo_cell_count * 100) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        } else if (telemetryPosition == 3) {                         //PID axis(hundreds column) and P/I/D (ones column) being adjusted currently
          uint16_t axisAndPidID = (current_pid_axis + 1) * 100;      //Adding one so there's always a value. 1 for Pitch (or Pitch/roll), 2 for Roll, 3 for Yaw
          axisAndPidID += current_pid_term + 1;                      //Adding one here too, humans don't deal well with counting starting at zero for this sort of thing
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8; // Adding one to the above makes it match the LED flash codes too
          telemetryPacket[5] = (int)(axisAndPidID);
          telemetryPacket[6] = (int)(axisAndPidID) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        } else if (telemetryPosition == 4) { //PID-P
          telemetryPacket[3] = telemetryIDs[telemetryPosition];
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8;
          telemetryPacket[5] = (int)(profile_current_pid_rates()->kp.axis[current_pid_axis] * 10000);
          telemetryPacket[6] = (int)(profile_current_pid_rates()->kp.axis[current_pid_axis] * 10000) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        } else if (telemetryPosition == 5) { //PID-I
          telemetryPacket[3] = telemetryIDs[telemetryPosition];
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8;
          telemetryPacket[5] = (int)(profile_current_pid_rates()->ki.axis[current_pid_axis] * 1000);
          telemetryPacket[6] = (int)(profile_current_pid_rates()->ki.axis[current_pid_axis] * 1000) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        } else if (telemetryPosition == 6) { //PID-D
          telemetryPacket[3] = telemetryIDs[telemetryPosition];
          telemetryPacket[4] = telemetryIDs[telemetryPosition] >> 8;
          telemetryPacket[5] = (int)(profile_current_pid_rates()->kd.axis[current_pid_axis] * 1000);
          telemetryPacket[6] = (int)(profile_current_pid_rates()->kd.axis[current_pid_axis] * 1000) >> 8;
          telemetryPacket[7] = 0x00;
          telemetryPacket[8] = 0x00;
        }

        //Calculate CRC for packet. This function does not support escaped characters.

        for (int x = 0; x < 9; x++) {
          teleCRC = teleCRC + telemetryPacket[x];
        }
        teleCRC = teleCRC + (teleCRC >> 8);
        teleCRC = 0xff - teleCRC;
        teleCRC = teleCRC << 8;
        teleCRC = teleCRC >> 8;
        telemetryPacket[9] = teleCRC;      //0x34;
        for (uint8_t x = 0; x < 10; x++) { //Shove the packet out the UART. This also doesn't support escaped characters
          while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
          USART_SendData(USART1, telemetryPacket[x]);
        } //That's it, telemetry sent
        telemetryPosition++;
        if (FPORTDebugTelemetry) {
          if (telemetryPosition >= sizeof(telemetryIDs) / 2) // 2 byte ints, so this should give the number of entries. It just incremented, which takes care of the count with 0 or 1
          {
            telemetryPosition = 0;
          }
        }
        else {
          if (telemetryPosition == 2) {
            telemetryPosition = 0;
          }
        }

      }
      
}


void processCRSF(void) {

}






//NOTE TO SELF: Put in some double-check code on the detections somehow.



void findprotocol(void) {

  rx_spektrum_bind(); // Light off DSMX binding, it *must* come first.
  protocolToCheck = 1; //Start with DSMX
  //uint8_t protocolToCheck = 1; Moved to global for visibility

  while (RXProtocol == 0) {
    //protocolToCheck = 4;
    //uint8_t protocolDetectTimer = 0;Moved to global for visibility
    usart_rx_init(protocolToCheck); //Configure a protocol!
    delay(500000);
    protocolDetectTimer = 0;

    while ((frameStatus == 0 || frameStatus == 3) && protocolDetectTimer < 1) { // Wait 2 seconds to see if something turns up.
      bobnovas = bobnovas + 10;
      for (int i = 0; i < protocolToCheck; i++) {
        ledon(255);
        delay(20000);
        ledoff(255);
        delay(50000);
      }
      ledoff(255);
      delay(500000 - (protocolToCheck * 70000));
      protocolDetectTimer++;
    }
    if (frameStatus == 1) { //We got something! What is it?
      switch (protocolToCheck)
      {
        case 1: // DSM
          if (rx_buffer[frameStart] == 0x00 && rx_buffer[frameStart + 1] == 0x00 && rx_buffer[frameStart] != 0x00) { // this one obviously needs some work to not trigger on an empty frame
            RXProtocol = protocolToCheck;
          }
          break;
        case 2: // SBUS
          if (rx_buffer[frameStart] == 0x0F) {
            RXProtocol = protocolToCheck;
          }
          break;
        case 3: // IBUS
          if (rx_buffer[frameStart] == 0x20) {
            RXProtocol = protocolToCheck;
          }
          break;
        case 4: // FPORT
          if (rx_buffer[frameStart] == 0x7E) {
            RXProtocol = protocolToCheck;
          }
          break;
        case 5: // CRSF
          if (rx_buffer[frameStart] != 0xFF && 1 == 2) { //Need to look up the expected start value.
            RXProtocol = protocolToCheck;
          }
          break;
        default:
          frameStatus = 3; //Whatever we got, it didn't make sense. Mark the frame as Checked and start over.
          break;
      }
      //if(rx_buffer[frameStart])
    }

    protocolToCheck++;
    if (protocolToCheck > 5) {
      protocolToCheck = 1;
      rxusart = 1;
    }
  }
  frameStatus = 3;//All done!
  debug.max_cpu_loop_number = gettime();
  lastlooptime = gettime();
}



// Send Spektrum bind pulses to a GPIO e.g. TX1
void rx_spektrum_bind(void) {
  //
  rx_bind_enable = fmc_read_float(56);
  if (rx_bind_enable == 0) {
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin = SERIAL_RX_SPEKBIND_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(SERIAL_RX_PORT, &GPIO_InitStructure);

    // RX line, set high
    GPIO_SetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_RX_PIN);
    // Bind window is around 20-140ms after powerup
    delay(60000);

    for (uint8_t i = 0; i < 9; i++) { // 9 pulses for internal dsmx 11ms, 3 pulses for internal dsm2 22ms
      // RX line, drive low for 120us
      GPIO_ResetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_RX_PIN);
      delay(120);

      // RX line, drive high for 120us
      GPIO_SetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_RX_PIN);
      delay(120);
    }
  }
  //#endif
  GPIO_InitTypeDef GPIO_InitStructure;
  GPIO_InitStructure.GPIO_Pin = SERIAL_RX_SPEKBIND_BINDTOOL_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(SERIAL_RX_PORT, &GPIO_InitStructure);

  // RX line, set high
  GPIO_SetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_BINDTOOL_PIN);
  // Bind window is around 20-140ms after powerup
  delay(60000);

  for (uint8_t i = 0; i < 9; i++) { // 9 pulses for internal dsmx 11ms, 3 pulses for internal dsm2 22ms
    // RX line, drive low for 120us
    GPIO_ResetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_BINDTOOL_PIN);
    delay(120);

    // RX line, drive high for 120us
    GPIO_SetBits(SERIAL_RX_PORT, SERIAL_RX_SPEKBIND_BINDTOOL_PIN);
    delay(120);
  }
}



#endif