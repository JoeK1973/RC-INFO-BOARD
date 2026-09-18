#include <SPI.h>   // Comes with Arduino IDE
#include <RF24.h>  // Download and Install (See above)
#include <nRF24L01.h>
#include <HT1632.h>
#include <font_8x4.h>
#include <images.h> 

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

void setup()
{
  HT1632.begin(7, 6, 5);
  myRadio.begin();
  myRadio.setChannel(108);
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
  if (i <= 500){
    firstfunction();
  } 
  else {
    secondfunction();
  } 
  if (i >= 1000){
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

void firstfunction(){
  sprintf(disp, "%d %d : %d %d",timeone, timetwo, timethree, timefour);
  HT1632.clear();
  HT1632.drawText(disp, 0, 0, FONT_8X4, FONT_8X4_END, FONT_8X4_HEIGHT);
  HT1632.render();
}

void secondfunction(){
  sprintf(disptwo, "R %d : H %d",r_int, h_int);
  HT1632.clear();
  HT1632.drawText(disptwo, 0, 0, FONT_8X4, FONT_8X4_END, FONT_8X4_HEIGHT);
  HT1632.render();
}

