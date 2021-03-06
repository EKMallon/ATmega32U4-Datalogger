/* A basic datalogger script from the Cave Pearl Project: https://thecavepearlproject.org/
that sleeps the datalogger and wakes from DS3231 RTC alarms*/

// NOTE:
//Any code which sleeps the 32U processor presents a real pain in the backside when DEBUGGING
//SLEEPING the processor - disables the USB & Uart - which kills your serial output
//this  also makes it impossible to upload new code...because there is no automatic reset like you get with the Pro Mini
//the only way to wake the MCU up is to cycle the power, or to do a "double tap" reset and
//try to get blink into the Pro Micro's processor before the old code goes to sleep again.

#include <Wire.h>
#include <avr/sleep.h>  // used in sleepNwait4D3Interrupt & sleepNwait4RTC
#include <RTClib.h>     // library from https://github.com/MrAlvin/RTClib  Note: there are many other DS3231 libs availiable
//assumes you have a DS3231 connected

#include <SPI.h>
#include <SdFat.h>      // needs 512 byte ram buffer! see https://github.com/greiman/SdFat
SdFat sd; /*Create the objects to talk to the SD card*/
SdFile file;
const int chipSelect = 10;    //CS moved to pin 10 on the arduino
//assumes you have a micro SD card on the SPI lines

#define SampleIntervalMinutes 1  // Whole numbers 1-30 only, must be a divisor of 60
// this is the number of minutes the loggers sleeps between each sensor reading

//#define ECHO_TO_SERIAL // this define to enable debugging output to the serial monitor
//comment out this define in for field deployments where you have no USB cable connected
//if you let the processor sleep it's almost impossible to get debugging output...

//uncomment ONLY ONE OF THESE TWO! - depending on how you are powering your logger
#define voltageRegulated  // if you connect the power supply through the Raw & GND pins which uses the system regulator
// note you must have a voltage divider monitoring the battery on one of the analog inputs
//#define unregulatedOperation  // if you've remvoved the regulator and are running from 2XAA lithium batteries

RTC_DS3231 RTC; // creates an RTC object in the code
// variables for reading the RTC time & handling the INT(0) interrupt it generates
#define DS3231_I2C_ADDRESS 0x68
#define RTC_INTERRUPT_PIN 7 // RTc SQW alarm connected on pin D7
byte Alarmhour;
byte Alarmminute;
byte Alarmday;
char CycleTimeStamp[ ] = "0000/00/00,00:00"; //16 ascii characters (without seconds)
volatile boolean clockInterrupt = false;  //this flag is set to true when the RTC interrupt handler is executed
//variables for reading the DS3231 RTC temperature register
float rtc_TEMP_degC;
byte tMSB = 0;
byte tLSB = 0;

char FileName[12] = "data000.csv"; 
const char codebuild[] PROGMEM = __FILE__;  // loads the compiled source code directory & filename into a varaible
const char compileDate[] PROGMEM = __DATE__; 
const char compileTime[] PROGMEM = __TIME__;
const char compilerVersion[] PROGMEM = __VERSION__; // https://forum.arduino.cc/index.php?topic=158014.0
const char dataCollumnLabels[] PROGMEM = "Time Stamp,Battery(mV),RTC temp(C),Rail(mV),AnalogRead(Raw),"; //gets written to second line of datafiles

//track the rail voltage using the internal 1.1v bandgap trick
uint16_t VccBGap = 9999; 
uint16_t systemShutdownVoltage = 2850; 
//if running of of 2x AA cells (with no regulator) the input cutoff voltage should be 2850 mV (or higher)
//if running a unit with the voltage regulator the input cutoff voltage should be 3400 mV

//example variables for analog pin reading
int analogPin = A0;
int AnalogReading = 0;
int BatteryPin = A3;
int BatteryReading = 9999;
float batteryVoltage = 9999.9;
int preSDsaveBatterycheck = 0;
//Global variables
//******************
byte bytebuffer1 =0;
//byte bytebuffer2 =0;
//int intbuffer=0;  //not used yet

int RXLED = 17;  // The YELLOW RX LED has a defined Arduino pin
// The GREEN TX LED was not so lucky, we'll need to use pre-defined
// macros (TXLED1, TXLED0) to control that.
// (We could use the same macros for the RX LED too -- RXLED1,
//  and RXLED0.)

//======================================================================================================================
//  *  *   *   *   *   *   SETUP   *   *   *   *   *
//======================================================================================================================

void setup() {
  pinMode(RXLED, OUTPUT);  // Set RX LED as an output
  // TX LED is set as an output behind the scenes

  #ifdef unregulatedOperation
  systemShutdownVoltage = 2850; // minimum Battery voltage when running from 2xAA's
  #endif

  #ifdef voltageRegulated
  systemShutdownVoltage = 3500; // minimum Battery voltage when running from 3 or 4 AA's supplying power to the Mic5205 regulator
  #endif
  
  // Setting the SPI pins high helps some sd cards go into sleep mode 
  // the following pullup resistors only needs to be enabled for the ProMini builds - not the UNO loggers
  pinMode(chipSelect, OUTPUT); digitalWrite(chipSelect, HIGH); //ALWAYS pullup the ChipSelect pin with the SD library
  //and you may need to pullup MOSI/MISO, usually MOSIpin=11, and MISOpin=12 if you do not already have hardware pulls
  pinMode(11, OUTPUT);digitalWrite(11, HIGH); //pullup the MOSI pin on the SD card module
  pinMode(12, INPUT_PULLUP); //pullup the MISO pin on the SD card module
  // NOTE: In Mode (0), the SPI interface holds the CLK line low when the bus is inactive, so DO NOT put a pullup on it.
  // NOTE: when the SPI interface is active, digitalWrite() cannot affect MISO,MOSI,CS or CLK

  // VERY LONG 30 second time delay here to compile & upload new code because once the processor sleeps - the USB is off!
  // this delay also stabilizes system after power connection - the cap on the main battery voltage divider needs>2s to charge up 
  for (int CNTR = 0; CNTR < 22; CNTR++) {
   
  TXLED0; //GREEN LED OFF
  digitalWrite(RXLED, LOW);//RX YELLOW LED ON
  delay(500);
  //digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
  TXLED1; //GREEN LED ON
  delay(500);
  digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
  //TXLED0; //GREEN LED OFF
  //digitalWrite(RXLED, LOW);//RX YELLOW LED ON
  delay(500);
  //digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
  TXLED0; //GREEN LED OFF
  digitalWrite(RXLED, LOW);//RX YELLOW LED ON
}  // exits with yellow on

#ifdef ECHO_TO_SERIAL
  Serial.begin(9600);  //Open serial communications and wait for port to open:
  //Serial1.begin(9600); //This is the hardware UART, pipes only to sensors attached to RXTX lines
#endif
   
  Wire.begin();          // start the i2c interface for the RTC
  TWBR = 2;//speeds up I2C bus to 400 kHz bus - ONLY Use this on 8MHz Pro Mini's
  // and remove TWBR = 2; if you have sensor reading problems on the I2C bus
  // onboard AT24c256 eeprom also ok @ 400kHz http://www.atmel.com/Images/doc0670.pdf  

  pinMode(RTC_INTERRUPT_PIN,INPUT_PULLUP);// RTC alarms low, so need pullup on the D2 line 
  //Note using the internal pullup is not needed if you have hardware pullups on SQW line, and most RTC modules do.
  RTC.begin();  // RTC initialization:
  clearClockTrigger(); //stops RTC from holding the interrupt low after power reset occured
  RTC.turnOffAlarm(1);
  DateTime now = RTC.now();
  sprintf(CycleTimeStamp, "%04d/%02d/%02d %02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute());

#ifdef ECHO_TO_SERIAL
  Serial.println(F("Initializing SD card..."));
#endif
// print lines in the setup loop only happen once
// see if the card is present and can be initialized
  if (!sd.begin(chipSelect,SPI_FULL_SPEED)) {   // some cards may need SPI_HALF_SPEED
    #ifdef ECHO_TO_SERIAL
    Serial.println(F("Card failed, or not present"));
    Serial.flush();
    #endif
    error(); //if you cant initialise the SD card, you can't save any data - so shut down the logger
    return;
  }
#ifdef ECHO_TO_SERIAL
  Serial.println(F("card initialized."));
#endif
  delay(50); //sd.begin hits the power supply pretty hard
  
// Find the next availiable file name
  // 2 GB or smaller cards should be used and formatted FAT16 - FAT16 has a limit of 512 files entries in root
  // O_CREAT = create the file if it does not exist,  O_EXCL = fail if the file exists, O_WRITE - open for write
  if (!file.open(FileName, O_CREAT | O_EXCL | O_WRITE)) { // note that restarts often generate empty log files!
    for (int i = 1; i < 512; i++) {
      delay(5);
      snprintf(FileName, sizeof(FileName), "data%03d.csv", i);//concatenates the next number into the filename
      if (file.open(FileName, O_CREAT | O_EXCL | O_WRITE)) // O_CREAT = create file if not exist, O_EXCL = fail if file exists, O_WRITE - open for write
      {
        break; //if you can open a file with the new name, break out of the loop
      }
    }
  }
  delay(25);
  //write the header information in the new file
  file.print(F("Filename:"));
  file.println((__FlashStringHelper*)codebuild); // writes the entire path + filename to the start of the data file
  file.print(F("Compiled:,"));
  file.print((__FlashStringHelper*)compileDate);
  file.print(F(","));
  file.print((__FlashStringHelper*)compileTime);
  file.print(F(","));
  file.print((__FlashStringHelper*)compilerVersion);
  file.println();
  file.println();file.println((__FlashStringHelper*)dataCollumnLabels);
  file.close(); delay(30);
  //Note: SD cards can continue drawing system power for up to 1 second after file close command
  digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
  
#ifdef ECHO_TO_SERIAL
  Serial.print(F("Data Filename:")); Serial.println(FileName); Serial.println(); Serial.flush();
#endif

  DIDR0 = 0x0F;  // This disables the digital inputs on analog lines 0..3 (analog 4&5 are used for I2C bus)

//setting any unused digital pins to input pullup reduces noise & risk of accidental short
//D7 = RTC alarm interrupts, d2&d3 are I2C bus, 
pinMode(4,INPUT_PULLUP); //only if you do not have anything connected to this pin
pinMode(5,INPUT_PULLUP); //only if you do not have anything connected to this pin
pinMode(7,INPUT_PULLUP); //only if you do not have anything connected to this pin
pinMode(8,INPUT_PULLUP); //only if you do not have anything connected to this pin
pinMode(9,INPUT_PULLUP); //only if you do not have anything connected to this pin
#ifndef ECHO_TO_SERIAL
 pinMode(0,INPUT_PULLUP); //but not if we are on usb - then these pins are needed for RX & TX 
 pinMode(1,INPUT_PULLUP);
#endif
TXLED1; //GREEN LED ON
  
//====================================================================================================
}   //   terminator for setup
//=====================================================================================================

// ========================================================================================================
//      *  *  *  *  *  *  MAIN LOOP   *  *  *  *  *  *
//========================================================================================================

void loop() {
  TXLED1; //GREEN LED ON 
  DateTime now = RTC.now(); //this reads the time from the RTC
  sprintf(CycleTimeStamp, "%04d/%02d/%02d %02d:%02d", now.year(), now.month(), now.day(), now.hour(), now.minute());
  //loads the time into a string variable - don’t record seconds in the time stamp because the interrupt to time reading interval is <1s, so seconds are always ’00’  
  // We set the clockInterrupt in the ISR, deal with that now:

if (clockInterrupt) {
    if (RTC.checkIfAlarm(1)) {       //Is the RTC alarm still on?
      RTC.turnOffAlarm(1);              //then turn it off.
    }
#ifdef ECHO_TO_SERIAL
   Serial.print("RTC Alarm on INT-0 triggered at ");  //(optional) debugging message
   Serial.println(CycleTimeStamp);
#endif
    clockInterrupt = false;                //reset the interrupt flag to false
}//=========================end of if (clockInterrupt) =========================
  
// read the RTC temp register - Note: the DS3231 temp registers (11h-12h) are only updated every 64seconds
  Wire.beginTransmission(DS3231_I2C_ADDRESS);
  Wire.write(0x11);                     //the register where the temp data is stored
  Wire.endTransmission();
  Wire.requestFrom(DS3231_I2C_ADDRESS, 2);   //ask for two bytes of data
  if (Wire.available()) {
  tMSB = Wire.read();            //2’s complement int portion
  tLSB = Wire.read();             //fraction portion
  rtc_TEMP_degC = ((((short)tMSB << 8) | (short)tLSB) >> 6) / 4.0;  // Allows for readings below freezing: thanks to Coding Badly
  //rtc_TEMP_degC = (rtc_TEMP_degC * 1.8) + 32.0; // To Convert Celcius to Fahrenheit
}
else {
  rtc_TEMP_degC = 0;  //if rtc_TEMP_degC contains zero, then you had a problem reading from the RTC!
}
#ifdef ECHO_TO_SERIAL
Serial.print(F(" TEMPERATURE from RTC is: "));
Serial.print(rtc_TEMP_degC);
Serial.println(F(" Celsius"));
#endif

// You could read in other variables here …like the analog pins, I2C sensors, etc
analogReference(DEFAULT);
AnalogReading = analogRead(analogPin);delay(1);  //throw away the first reading
AnalogReading = analogRead(analogPin);

//========== Pre SD saving battery check ===========
#ifdef unregulatedOperation 
preSDsaveBatterycheck = getRailVoltage(); //If running with no regulator then vcc = the battery voltage
if (preSDsaveBatterycheck < (systemShutdownVoltage+100)) { //on older lithium batteries the SD save can cause a 100mv drop
    error(); //shut down the logger because of low voltage reading
}
#endif

#ifdef voltageRegulated 
analogRead(BatteryPin);delay(5);  //throw away the first reading, high impedance divider
batteryVoltage = float(analogRead(BatteryPin));
batteryVoltage = (batteryVoltage+0.5)*(3.3/1024.0)*4.030303; // 4.0303 = (Rhigh+Rlow)/Rlow for 10M/3.3M resistor combination
preSDsaveBatterycheck=int(batteryVoltage*1000.0);
if (preSDsaveBatterycheck < (systemShutdownVoltage+300)) {  //on old alkaline batteries the 100-200mA SD save events can cause a 300mv drop
    error(); //shut down the logger because of low voltage reading
}
#endif

TXLED0; //GREEN LED OFF
digitalWrite(RXLED, LOW);//RX YELLOW LED ON
//========== Battery Good? then write the data to the SD card ===========
file.open(FileName, O_WRITE | O_APPEND); // open the file for write at end like the Native SD library
    delay(20);
    file.print(CycleTimeStamp);
    file.print(","); 
    file.print(BatteryReading);
    file.print(",");    
    file.print(rtc_TEMP_degC);
    file.print(",");    
    file.print(VccBGap);
    file.print(",");
    file.print(AnalogReading);
    file.println(",");
    file.close();

//========== POST SD saving battery check ===========
//the SD card can pull up to 200mA, and so more representative battery readings are those taken after this load
#ifdef unregulatedOperation  /If you are running from raw battery power (with no regulator) vcc = the battery voltage
VccBGap = getRailVoltage(); //takes this reading immediately directly after the SD save event
BatteryReading = VccBGap;
  if (VccBGap < systemShutdownVoltage) { 
    error(); //shut down the logger because of low voltage reading
  }
#endif

#ifdef voltageRegulated 
analogReference(DEFAULT);
AnalogReading = analogRead(BatteryPin);delay(5);  //throw away the first reading, high impedance divider
analogRead(BatteryPin);delay(5);  //throw away the first reading, high impedance divider
batteryVoltage = float(analogRead(BatteryPin));
batteryVoltage = (batteryVoltage+0.5)*(3.3/1024.0)*4.030303 ; // 4.0303 = (Rhigh+Rlow)/Rlow for 10M/3.3M resistor combination
BatteryReading = int(batteryVoltage*1000.0);

if (BatteryReading < systemShutdownVoltage) { 
    error(); //shut down the logger because of low voltage reading
}

VccBGap = getRailVoltage(); //if your system is regulated, take this reading after the main battery - it should be very stable!
#endif

digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
TXLED1; //GREEN LED ON

#ifdef ECHO_TO_SERIAL  // print to the serial port too:
    Serial.print(CycleTimeStamp);
    Serial.print(","); 
    Serial.print(BatteryReading);
    Serial.print(",");      
    Serial.print(rtc_TEMP_degC);
    Serial.print(",");    
    Serial.print(VccBGap);
    Serial.print(",");
    Serial.println(AnalogReading);
    Serial.flush();
#endif

//============Set the next alarm time =============
Alarmhour = now.hour();
Alarmminute = now.minute() + SampleIntervalMinutes;
Alarmday = now.day();

// check for roll-overs
if (Alarmminute > 59) { //error catching the 60 rollover!
  Alarmminute = 0;
  Alarmhour = Alarmhour + 1;
  if (Alarmhour > 23) {
    Alarmhour = 0;
    // put ONCE-PER-DAY code here -it will execute on the 24 hour rollover
  }
}
// then set the alarm
RTC.setAlarm1Simple(Alarmhour, Alarmminute);
RTC.turnOnAlarm(1);
if (RTC.checkAlarmEnabled(1)) {
  //you would comment out most of this message printing
  //if your logger was actually being deployed in the field

#ifdef ECHO_TO_SERIAL
  Serial.print(F("RTC Alarm Enabled!"));
  Serial.print(F(" Going to sleep for : "));
  Serial.print(SampleIntervalMinutes);
  Serial.println(F(" minutes"));
  Serial.println();
  Serial.flush();//adds a carriage return & waits for buffer to empty
  delay(20);
#endif
}

  //delay(5); //this optional delay is only here so we can see the LED’s otherwise the entire loop executes so fast you might not see it.
  digitalWrite(RXLED, HIGH);    // YELLOW RX LED OFF
  TXLED0; //GREEN LED OFF
  // Note: Normally you would NOT leave a red indicator LED on during sleep! This is just so you can see when your logger is sleeping, & when it's awake
  //digitalWrite(RED_PIN, HIGH);  // Turn on red led as our indicator that the Arduino is sleeping. Remove this before deployment.
  //——– sleep and wait for next RTC alarm ————–
  // Enable interrupt on pin2 & attach it to rtcISR function:
  // Enter power down state with ADC module disabled to save power:
  
   sleepNwait4RTC();
   //processor starts HERE AFTER THE RTC ALARM WAKES IT UP
   //RTC alarm Interupt woke processor =>> go back to the start of the main loop
}
//============================= END of the MAIN LOOP =================================

ISR(INT6_vect) {  // for INT6 interrupt from RTC alarm-low signal pin D7
  clockInterrupt = true;
  }

//====================================================================================
void clearClockTrigger() // from http://forum.arduino.cc/index.php?topic=109062.0
{
  Wire.beginTransmission(0x68);   //Tell devices on the bus we are talking to the DS3231
  Wire.write(0x0F);               //Tell the device which address we want to read or write
  Wire.endTransmission();         //Before you can write to and clear the alarm flag you have to read the flag first!
  Wire.requestFrom(0x68,1);       //Read one byte
  bytebuffer1=Wire.read();        //In this example we are not interest in actually using the bye
  Wire.beginTransmission(0x68);   //Tell devices on the bus we are talking to the DS3231 
  Wire.write(0x0F);               //Status Register: Bit 3: zero disables 32kHz, Bit 7: zero enables the main oscilator
  Wire.write(0b00000000);         //Write the byte. //Bit1: zero clears Alarm 2 Flag (A2F), Bit 0: zero clears Alarm 1 Flag (A1F)
  Wire.endTransmission();
  clockInterrupt=false;           //Finally clear the flag we use to indicate the trigger occurred
}
//====================================================================================

void sleepNwait4RTC() {  //old method of sleeping (without using the RocketScreem lib)
  //disableUSB //must be done before using sleep
  USBCON |= _BV(FRZCLK);  //freeze USB clock
  PLLCSR &= ~_BV(PLLE);   // turn off USB PLL
  USBCON &= ~_BV(USBE);   // disable USB

  byte keep_ADCSRA = ADCSRA; // used for shutdown & wakeup of ADC
  ADCSRA = 0; //disable the ADC - worth 334 µA during sleep
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  noInterrupts (); // make sure we don't get interrupted before we sleep
 
  //RTC alarm-low connected to pin D7
  EIMSK  &= ~(1<<INT6); // de-activates the interrupt.
  EICRB &= ~(1<<ISC60); //forces bit to zero //ACSR &= ~(1<<ACIE); forces bit to zero, leaves others alone
  EICRB &= ~(1<<ISC61); //forces bit to zero - with both at zero = trigger on low
  EIMSK |= (1<<INT6); // activates the interrupt. // |= (1<<ACBG); forces bit to one
  /*
  ISCn0  ISCn1  Where n is the interrupt. 0 for 0, etc
  0      0  Triggers on low level
  1      0  Triggers on edge
  0      1  Triggers on falling edge
  1      1  Triggers on rising edge
  */

  EIFR = bit (INTF6);  // clear flag for interrupt 
  sleep_enable();
  //MCUCR = bit (BODS) | bit (BODSE);  // turn on brown-out enable select
  //MCUCR = bit (BODS); // this must be done within 4 clock cycles of above  //also see http://jeelabs.org/2010/09/01/sleep/
  interrupts ();// interrupts allowed now, next instruction WILL be executed
  sleep_mode();
  //HERE AFTER WAKING UP
  EIMSK  &= ~(1<<INT6); // de-activates the interrupt
  EIFR = bit (INTF6);  // clear flag for interrupt 
  ADCSRA = keep_ADCSRA;

#ifdef ECHO_TO_SERIAL
  //wakeUSB //must occur immediately after waking from sleep
  //these delay timings are important! //  https://arduino.stackexchange.com/questions/10408/starting-usb-on-pro-micro-after-deep-sleep-atmega32u4
  delay(100);
  USBDevice.attach(); // keep this
  delay(100);
  Serial.begin(9600);
  delay(100);
#endif

  ADCSRA = keep_ADCSRA;
  
}  //terminator for sleepNwait4RTC

int getRailVoltage()    // from http://forum.arduino.cc/index.php/topic,38119.0.html
{
  int result; 
  const long InternalReferenceVoltage = 1100L; //note your can read the band-gap voltage & set this value more accurately: https://forum.arduino.cc/index.php?topic=38119.0

  for (int i = 0; i < 4; i++) { // have to loop at least 4 times before it yeilds consistent results - the cap on aref needs to settle


  #if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
    ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
    ADMUX = _BV(MUX5) | _BV(MUX0);
  #elif defined (__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
    ADMUX = _BV(MUX3) | _BV(MUX2);
  #else
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #endif 

    delay(1);  // voltage droop/recover from pulse load in lithiums takes >10ms, so this delay is ok  http://data.energizer.com/pdfs/l91.pdf
    // Start a conversion
    ADCSRA |= _BV( ADSC );
    // Wait for it to complete
    while ( ( (ADCSRA & (1 << ADSC)) != 0 ) );
    // Scale the value
    result = (((InternalReferenceVoltage * 1023L) / ADC) + 5L); //scale Rail voltage in mV
    // note that you can tune the accuracy of this function by changing InternalReferenceVoltage to match your board
    // just tweak the constant till the reported rail voltage matches what you read with a DVM!
  }     // end of for (int i=0; i < 4; i++) loop

  ADMUX = bit (REFS0) | (0 & 0x07); analogRead(A0); // cleanup: select input port A0 + engage new Aref at rail voltage

  return result;
  
}  // terminator for getRailVoltage()

//========================================================================================
void error(){
    digitalWrite(RXLED, HIGH);// set the RX YELLOW LED OFF
    TXLED0; //macro turn GREEEN LED OFF
    for (int CNTR = 0; CNTR < 150; CNTR++) { //spend some time flashing red indicator light on error before shutdown!
    digitalWrite(RXLED, LOW);//RX YELLOW LED ON
    delay(120);
    digitalWrite(RXLED, HIGH);// YELLOW RX LED OFF
    delay(120);
  }
  sleepNwait4RTC(); // this is a permenant sleep because no alarm has been set
}


