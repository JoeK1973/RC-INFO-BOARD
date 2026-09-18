int ledPin1 = 2;
int ledPin2 = 3;
int ledPin3 = 4;

unsigned long time;
unsigned long oldtime;
unsigned long r_interval = 3000;
unsigned long h_interval = 6000;
unsigned long t_interval = 12000;

void setup(){
  pinMode(ledPin1, OUTPUT);
  pinMode(ledPin2, OUTPUT);
  pinMode(ledPin3, OUTPUT);
}

void loop(){
  time = millis();
  if ((time - oldtime) <= r_interval){
    lightledone();
  }
  if (((time - oldtime) > r_interval) && ((time - oldtime) <= h_interval)){
    lightledtwo();
  }
  if (((time - oldtime) > h_interval) && ((time - oldtime) <= t_interval)){
    lightledthree();
  }
  if ((time - oldtime) > t_interval){
    oldtime = time;
  }
}

void lightledone(){
  digitalWrite(ledPin1, HIGH);
  digitalWrite(ledPin2, LOW);
  digitalWrite(ledPin3, LOW);
}

void lightledtwo(){
  digitalWrite(ledPin1, LOW);
  digitalWrite(ledPin2, HIGH);
  digitalWrite(ledPin3, LOW);
}

void lightledthree(){
  digitalWrite(ledPin1, LOW);
  digitalWrite(ledPin2, LOW);
  digitalWrite(ledPin3, HIGH);
}
