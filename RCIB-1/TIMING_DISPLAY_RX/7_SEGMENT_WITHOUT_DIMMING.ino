#include <SPI.h>   // Comes with Arduino IDE
#include <RF24.h>  // Download and Install (See above)
#include <nRF24L01.h>

RF24 myRadio (9, 10);

byte addresses[6] = {
  "1Node"}; 

char tx_array[63];
char disp[63];
char disptwo[63];
int i;
int lstate;
int r_int;
int h_int;
int timeone;
int timetwo;
int timethree;
int timefour;

int radav = 0;

// TPIC6B595 pin declarations
////Pin connected to SERIN of TPIC6B595
int dataPin = 2;
//Pin connected to RCK of TPIC6B595
int latchPin = 3;
//Pin connected to SRCK of TPIC6B595
int clockPin = 4;

// digit declaration digits 0-9
byte lights[] = {0b01110111,0b00100100,0b01011101,0b01101101,0b00101110,0b01101011,0b01111011,0b00100101,0b01111111,0b01101111};
byte others[] = {0b00000000,0b00011000,0b00111010};// blank, r, h

void setup()
{
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT); 
  myRadio.begin();
  myRadio.setChannel(108);
  myRadio.setPALevel(RF24_PA_MAX);
  myRadio.setDataRate(RF24_250KBPS);
  myRadio.openReadingPipe(1, addresses[0]);
  myRadio.startListening();
  delay(50);
}
void loop()
{
  recvlstate();
  if (radav == 1) // Non-blocking
  {
    lstate = atoi(strtok((char*)tx_array, ","));
    r_int = atoi(strtok(NULL, ","));
    h_int = atoi(strtok(NULL, ","));
    timeone = atoi(strtok(NULL, ","));
    timetwo = atoi(strtok(NULL, ","));
    timethree = atoi(strtok(NULL, ","));
    timefour = atoi(strtok(NULL, "."));
    radav = 0;
  }
  if (i <= 5000){
    firstfunction();
  } 
  else if (i <= 10000){
    secondfunction();
  }
  else if (i <= 15000){
    thirdfunction();
  } 
  if (i > 15000){
    i = 0;
  }
  i++;
}

void recvlstate(){
  if (myRadio.available())  // While there is data ready
  {
    myRadio.read( &tx_array, sizeof(tx_array) ); // Get the data payload (You must have defined that already!)
    radav = 1;
  }
}

void firstfunction(){// round number
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[(r_int)]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[0]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[0]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[1]);
  digitalWrite(latchPin, HIGH);                           // take the latch pin high so the LEDs will light up:  
}

void secondfunction(){// heat number
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[(h_int)]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[0]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[0]);
  shiftOut(dataPin, clockPin, MSBFIRST, others[2]);
  digitalWrite(latchPin, HIGH);                           // take the latch pin high so the LEDs will light up:
}

void thirdfunction(){
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[timefour]);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[timethree]);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[timetwo]);
  shiftOut(dataPin, clockPin, MSBFIRST, lights[timeone]);
  digitalWrite(latchPin, HIGH);                           // take the latch pin high so the LEDs will light up:
}
