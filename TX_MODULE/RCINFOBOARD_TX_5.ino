/*
RC Information Board Project

This project reads in RC car race information output from RC-Timing by Dale Burr
(race information such as heat number, round number, time elapsed etc) and also
gantry/start lights information.

Race information is read in via the normal Arduino serial port (USB on an Uno
or pins 0 and 1 on an Uno or Atmega 328P-Pu IC) whilst gantry/start lights
information is read in via 2 Arduino pins after having been converted from RS232
by a MAX232 or similar.

The Arduino then processes the information and transforms it into 7 seperate
integer variables which are then joined using the sprintf function and 
transmitted using the VirtualWire library from a 433Mhz MX-FS-03V transmitter
connected to the Arduino

For more information on this project, visit:

https://rcinfoboard.wordpress.com/

Created by Joe Keaveney
February 2016
*/

//#include <LiquidCrystal.h> // Library required for LCD 16x2 display for testing
//#include <VirtualWire.h> // Library required for data transmission over 433Mhz
#include <SoftwareSerial.h>
#include <SPI.h>   // Comes with Arduino IDE
#include <RF24.h>  // Download and Install (See above)
#include <nRF24L01.h>

RF24 myRadio (7, 8);

byte addresses[6] = {"1Node"}; 

SoftwareSerial mySerial(5, 6);

//LiquidCrystal lcd(12, 9, 5, 4, 3, 2); // tell Arduino the LCD pin configuration 

char tx_array[63]; // The array of variables that is transmitted
//char inputChar[50]; // The string (char array) that we search through for data
char *i;
char *rnum; // round number character 
char *hnum; // heat number character
char *mnum; // etc
char *snum; // etc
int lstate = 0; // start/gantry lights state integer
int r_int; // round number integer
int h_int; // etc
int m_int; // etc
int s_int; // etc
int timeone; //integer that represents the first number of a 4-digit display
int timetwo; // second number
int timethree; // third number
int timefour; // fourth number

const int dtrPin = 2;			// DTR input pin from MAX232/USB
const int rtsPin = 3;			// RTS input pin from MAX232/USB
int dtrState = 0;				// stores the state of DTR pin when read
int rtsState = 0;				// stores the state of RTS pin when read

//String inputString = "";         // a string to hold incoming data
//String rideString = "";
const byte numChars = 64;
char receivedChars[numChars];
char rideChars[numChars];
boolean stringComplete = false;  // whether the string is complete

void setup() { // setup Arduino in it's initial state & start services
  pinMode(dtrPin, INPUT); // set input pins for reading RS232 pin states
  pinMode(rtsPin, INPUT);
  //lcd.begin(16, 2); // initialize LCD display and tell Arduino how big it is
  Serial.begin(9600); // initialize serial so we can read in race information
  //rideString.reserve(200);
  //inputString.reserve(200); // reserve 200 bytes for the inputString:
  mySerial.begin(9600);
  myRadio.begin();
  myRadio.setChannel(108);
  myRadio.setPALevel(RF24_PA_MAX);
  myRadio.setDataRate(RF24_250KBPS);
  myRadio.openWritingPipe(addresses[0]);
  delay(50);
  //mySerial.println("Starting");
  //vw_set_ptt_inverted(true); // setup 433Mhz hardware - need this for MX-FS-03V
  //vw_set_tx_pin(8); // set Tx pin number
  //vw_setup(2000);// speed of data transfer Kbps - 2000 is fine & stable
}

void loop() {
  
  dtrState = digitalRead(dtrPin); // read the state of the DTR and RTS inputs
  rtsState = digitalRead(rtsPin);
  //dtrState = !dtrState ; // Inverts the inputs, necessary due to TTL conversion
  //rtsState = !rtsState ; 
  
  if (dtrState == LOW && rtsState == LOW) {     
    // DTR HIGH and RTS HIGH mean Green Light On    
    lstate = 2; 
  } else if (dtrState == LOW && rtsState == HIGH) {     
    // DTR HIGH and RTS LOW means Red Light On   
    lstate = 1; 
  }  
  else {
    // Any other combination means both lights Off
    lstate = 0; 
  }
  
  if (stringComplete) {
    Serial.print(rideChars);
    mySerial.println(rideChars);
//    inputString.toCharArray(inputChar, 50); // convert the input string to char
    rnum = strtok_r(receivedChars,":",&i); // start breaking down the input info
    hnum = strtok_r(NULL,":",&i); // from RC-Timing into the data we need
    mnum = strtok_r(NULL,":",&i);
    snum = strtok_r(NULL,":",&i);
    r_int = atoi(rnum); // convert the info into integers
    h_int = atoi(hnum);
    m_int = atoi(mnum);
    s_int = atoi(snum);
    if (m_int <= 9) { // break the time info into two integers for use on a
        timeone = 0;  // 4-digit display
        timetwo = m_int;
    //    lcd.print(timeone);
    //    lcd.print(timetwo);
    } 
    else {
        timeone = m_int / 10;
        timetwo = m_int % 10;
    //    lcd.print(timeone);
    //    lcd.print(timetwo); 
    }
    // lcd.print(":");
    if (s_int <= 9) {
      timethree = 0;
      timefour = s_int;
    //  lcd.print(timethree);
    //  lcd.print(timefour);
    } 
    else {
      timethree = s_int / 10;
      timefour = s_int % 10;
    //  lcd.print(timethree);
    //  lcd.print(timefour); 
    }
    
    //lcd.clear();			//this section output the data to an LCD display
    //lcd.print("R:");		//in the finished device, this will be removed
    //lcd.print(r_int);    
    //lcd.print(" H:");   
    //lcd.print(h_int);
    //lcd.setCursor(4,1);
    //lcd.print("TIME:");
    //lcd.print(timeone);
    //lcd.print(timetwo);
    //lcd.print(":");
    //lcd.print(timethree);
    //lcd.print(timefour);
    
    //inputString = "";		// clear the string and flag ready to listen again
    //rideString = "";
    stringComplete = false;
  }
  // the sprintf function consolidates all the integer variables together
  // and then vw_send and vw_wait send the info out on the 433Mhz Tx
  sprintf(tx_array, "%d,%d,%d,%d,%d,%d,%d.",lstate,r_int,h_int,timeone,timetwo,timethree,timefour);
  myRadio.write(&tx_array, sizeof(tx_array));
  //vw_send((uint8_t*)tx_array, strlen(tx_array));
  //vw_wait_tx();
}

/*
  SerialEvent occurs whenever a new data comes in the
 hardware serial RX (USB or Arduino pins 0 and 1).  
 This routine is run between each
 time loop() runs.
 */
void serialEvent(){
    static boolean recvInProgress = false;
    static byte ndx = 0;
    char startMarker = '[';
    char endMarker = ']';
    char rc;
 
 // if (Serial.available() > 0) {
    while (Serial.available() > 0 && stringComplete == false) {
        rc = Serial.read();

        if (recvInProgress == true) {
            if (rc != endMarker) {
                receivedChars[ndx] = rc;
                rideChars[(ndx + 1)] = rc;
                ndx++;
                if (ndx >= numChars) {
                    ndx = numChars - 1;
                }
            }
            else {
                receivedChars[ndx] = '\0'; // terminate the string
                rideChars[(ndx + 1)] = rc;
                rideChars[(ndx + 2)] = '\0';
                recvInProgress = false;
                ndx = 0;
                stringComplete = true;
            }
        }

        else if (rc == startMarker) {
            recvInProgress = true;
            rideChars[ndx] = rc;
        }
    }
}


