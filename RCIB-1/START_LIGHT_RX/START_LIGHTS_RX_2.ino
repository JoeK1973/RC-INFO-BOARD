
#include <SPI.h>   // Comes with Arduino IDE
#include <RF24.h>  // Download and Install (See above)
#include <nRF24L01.h>

RF24 myRadio (9, 10);

byte addresses[6] = {"1Node"}; 

// initialize the library with the numbers of the interface pins
//LiquidCrystal lcd(12, 9, 5, 4, 3, 2);

char tx_array[63];
int radav = 0;
int lstate;

// TPIC6B595 pin declarations
//Pin connected to ST_CP of 74HC595
int latchPin = 3;
//Pin connected to SH_CP of 74HC595
int clockPin = 4;
////Pin connected to DS of 74HC595
int dataPin = 2;

// digit declaration digits 0-9
byte lights[] = {0b11111111,0b00000000};

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
  //vw_set_ptt_inverted(true); // Required for DR3100
  //vw_set_rx_pin(8);
  //vw_setup(2000);  // Bits per sec
  //lcd.begin(16, 2);
  //lcd.clear();
  //vw_rx_start();       // Start the receiver PLL running
}
void loop()
{
  //uint8_t buf[VW_MAX_MESSAGE_LEN];
  //uint8_t buflen = VW_MAX_MESSAGE_LEN;
  recvlstate();
  if (radav == 1) // Non-blocking
  {
    lstate = atoi(strtok((char*)tx_array, ","));
    switch(lstate){
      case 0:
        digitalWrite(latchPin, LOW);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[1]);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[1]);  // shift out the bits:
        digitalWrite(latchPin, HIGH);                           // take the latch pin high so the LEDs will light up:
        //digitalWrite(redlatchPin, LOW);
        //shiftOut(reddataPin, redclockPin, MSBFIRST, lights[1]);      // shift out the bits:
        //digitalWrite(redlatchPin, HIGH);                             // take the latch pin high so the LEDs will light up:
      break;
      case 1:
        digitalWrite(latchPin, LOW);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[1]);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[0]);  // shift out the bits:
        digitalWrite(latchPin, HIGH);                             // take the latch pin high so the LEDs will light up:
      break;
      case 2:
        digitalWrite(latchPin, LOW);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[0]);
        shiftOut(dataPin, clockPin, MSBFIRST, lights[1]);  // shift out the bits:
        digitalWrite(latchPin, HIGH);                             // take the latch pin high so the LEDs will light up:
      break;
    }
    radav = 0;
    // lcd.clear();     stuff here if it doesn't work in the switch statement

  }
}

void recvlstate(){
  if (myRadio.available())  // While there is data ready
    {
      myRadio.read( &tx_array, sizeof(tx_array) ); // Get the data payload (You must have defined that already!)
      radav = 1;
    }
}
