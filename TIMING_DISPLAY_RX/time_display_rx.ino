//#include <VirtualWire.h>

// include the library code for use by the LCD display we're using for testing the program structure
#include <SPI.h>   // Comes with Arduino IDE
#include <RF24.h>  // Download and Install (See above)
#include <nRF24L01.h>
#include <LiquidCrystal.h>

// initialize the library with the numbers of the interface pins
LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

RF24 myRadio (9, 10);

byte addresses[6] = {"1Node"}; 

char tx_array[63];
int i;
int lstate;
int r_int;
int h_int;
int timeone;
int timetwo;
int timethree;
int timefour;

int radav = 0;

void setup()
{
  //vw_set_ptt_inverted(true); // Required for DR3100
  //vw_set_rx_pin(8);
  //vw_setup(2000);  // Bits per sec
  myRadio.begin();
  myRadio.setChannel(108);
  myRadio.openReadingPipe(1, addresses[0]);
  myRadio.startListening();
  delay(50);
  lcd.begin(16, 2);
  lcd.clear();
  lcd.print("Starting.....");
  delay(5000);
  lcd.clear();void recvlstate(){
  if (myRadio.available())  // While there is data ready
    {
      myRadio.read( &tx_array, sizeof(tx_array) ); // Get the data payload (You must have defined that already!)
      radav = 1;
    }
}
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
    r_int = atoi(strtok(NULL, ","));
    h_int = atoi(strtok(NULL, ","));
    timeone = atoi(strtok(NULL, ","));
    timetwo = atoi(strtok(NULL, ","));
    timethree = atoi(strtok(NULL, ","));
    timefour = atoi(strtok(NULL, "."));
    
    lcd.clear();
    lcd.print("R:");
    lcd.print(r_int);    
    lcd.print(" H:");   
    lcd.print(h_int);
    lcd.setCursor(4,1);
    lcd.print("TIME:");
    lcd.print(timeone);
    lcd.print(timetwo);
    lcd.print(":");
    lcd.print(timethree);
    lcd.print(timefour);
    delay(10);
    
    radav = 0;
  }
}

void recvlstate(){
  if (myRadio.available())  // While there is data ready
    {
      myRadio.read( &tx_array, sizeof(tx_array) ); // Get the data payload (You must have defined that already!)
      radav = 1;
    }
}
