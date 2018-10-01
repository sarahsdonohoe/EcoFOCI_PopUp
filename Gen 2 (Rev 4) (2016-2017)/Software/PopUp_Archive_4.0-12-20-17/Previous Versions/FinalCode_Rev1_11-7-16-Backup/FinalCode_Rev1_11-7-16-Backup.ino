//include various libraries
#include <ctype.h>
#include <Wire.h>
#include <SPI.h>  
#include <math.h>
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
#include <SD.h>
#include <IridiumSBD.h>
#include <RTClib.h>
#include <RTC_DS3234.h>
#include <MemoryFree.h>

#define TEMP_SWITCH 3
#define SENSOR_POWER_5V 6
#define SENSOR_POWER_3V3 22
#define GPS_POWER 9
#define IRIDIUM_POWER 8
#define IRIDIUM_ON_OFF A6
#define ACCEL_CS A8
#define SDCARD_CS A5
#define RTC_CS A2
#define SHUTDOWN A0
#define GPS_TX 11
#define GPS_RX 12
#define IRIDIUM_RXD A14
#define IRIDIUM_TXD A15

#define keller3bar 0x40       		//Define pressure sensor control register
#define keller10bar 0x41      		//Define pressure sensor control register
#define checkpressure 0xAC    		//Define pressure sensor control register
#define ads1100A0 0x48        		//Define tempADC control register
#define ads1100A1 0x49        		//Define parADC control register


SoftwareSerial ss(GPS_TX,GPS_RX);		//Software Serial Object for GPS Communication 
SoftwareSerial nss(IRIDIUM_TXD,IRIDIUM_RXD);	//Software Serial Object for Iridium Communication
IridiumSBD isbd(nss,IRIDIUM_ON_OFF);		//ISBD Object for Iridium Commands
TinyGPSPlus gps;			        //Object for GPS commands and data
RTC_DS3234 RTC(RTC_CS);			        //Object for RTC

DateTime wakeupTime;				//Global Variable for when unit first wakes
DateTime alarmtime;			        //Global Variable for RTC Alarm
boolean foundGPS=false; 			//Global Variable for GPS lock success


//********************************************************************************************************************************************************//
//***SET DATES AND PROGRAM INTERVALS HERE*****************************************************************************************************************//
//********************************************************************************************************************************************************//

DateTime unitStart =  DateTime(16,9,19,10,10,0);            //Date and Time for first sample: 				DateTime(YEAR,MON,DAY,HOUR,MIN,SEC);
DateTime releaseTime =  DateTime(16,9,19,11,0,0);           //Date and Time for actual release of burn wire: 		DateTime(YEAR,MON,DAY,HOUR,MIN,SEC);
DateTime sendDataDate = DateTime(16,9,19,11,30,0);          //Date and Time to attempt data transmission twice a day: 	DateTime(YEAR,MON,DAY,HOUR,MIN,SEC);
DateTime iceFreeDate = DateTime(16,9,19,11,30,0);           //Date and Time to attempt data transmission every hour: 	DateTime(YEAR,MON,DAY,HOUR,MIN,SEC);
#define FAILSAFE_TIMER 86400				    //Alarm for Failsafe if program freezes somehow 		(nominal FAILSAFE_TIMER 86400 = 1 day)
#define BOTTOM_SAMPLE_INTERVAL 300			    //Interval between bottom samples 				(nominal BOTTOM_SAMPLE_INTERVAL 21600 = 6 hours)
#define PRE_RELEASE_WAKEUP_TIME 300			    //How long before planned release to wait for profile 	(nominal PRE_RELEASE_WAKEUP_TIME 21600 = 6 hours)
#define UNDER_ICE_SAMPLE_INTERVAL 300			    //Interval between samples at the surface (or uncer ice)	(nominal UNDER_ICE_SAMPLE_INTERVAL 3600	= 1 hour)
#define PROFILE_LENGTH_SECONDS 30			    //Length of profile in seconds				(nominal PROFILE_LENGTH_SECONDS 60 = 60 seconds)
#define DEPTH_CHANGE_TO_TRIP_PROFILE 1			    //Depth in meters needed to begin profile mode		(nominal DEPTH_CHANGE_TO_TRIP_PROFILE 3	= 3 meters)
#define GPS_SEARCH_SECONDS 180				    //Number of seconds to search for GPS signal		(nominal GPS_SEARCH_SECONDS 120	= 2 minutes)
#define GPS_SECONDS_BETWEEN_SEARCHES 180		    //Number of seconds between GPS Searches to send data	(nominal GPS_SECONDS_BETWEEN_SEARCHES 180 = 3 minutes)
#define GPS_SUCCESSES_TO_SEND 3				    //Number of successful gps hits before sending data		(nominal GPS_SUCCESSES_TO_SEND 3 = 2 attempts)
#define CHARS_PER_SBDMESSAGE 50				    //Number of bytes to pull from data file and send at once	(nominal CHARS_PER_SBDMESSAGE 50 = 50 bytes)

//********************************************************************************************************************************************************//
//***END PROGRAM DATES AND SAMPLE INTERVAL SECTION********************************************************************************************************//
//********************************************************************************************************************************************************//


void setup(){
  initializePins();		            //Initialize Arduino Pins for Output and Set Voltage Levels
  Wire.begin();				    //Start I2C Communication
  SPI.begin();                              //Start up SPI Communication (needed for various ICs) 
  SPI.setDataMode(SPI_MODE2);               //Set SPI Data Mode
  SPI.setBitOrder(MSBFIRST);                //Configure SPI Communication (needed for various ICs)
  SPI.setClockDivider(SPI_CLOCK_DIV16);     //Set SPI Clock Speed

  setupRTCandSD();			    //Start RTC and SD Card 
  setupAccel();				    //Start Accelerometer
  
  turnOn3v3SensorPower();		    //turn on 3.3V Power to sensors (PAR, TEMP, PRESSURE)
  turnOn5vSensorPower();		    //turn on 5V Power to sensors (Needed for 5V I2C communication)
  delay(100);				    //Short delay here to start ADCs
  setupTempADC();			    //Start Up the ADC for Temperature Measurement
  setupPARADC();			    //Start Up the ADC for PAR Measurement
  
  char mode = selectModeandSetAlarm();	    //Select mode based on current time and programmed dates and set the alarm
  sampleData(mode);    			    //Sample sensors and store data set 

  if(mode==1||mode==0){			    //Mode 1 = On Bottom, Not waiting for release //Mode 0 = Before Initial sample date
    shutdownUnit();			    //Turn off power and wait
  }
  else if(mode==2){			    //Mode 2 = On Bottom, waiting for profile to begin
    waitForProfile();  			    //continuously monitor depth waiting for change to signal start of profile
    collectProfile();			    //collect samples and store data at high sample rate for pre-determined length of time
    shutdownUnit();			    //Turn off power and wait
  }
  else if(mode==4){			    //Mode 4 = Under Ice, too early to attempt data transmission
    underIceEarlyMode();  		    //Look for GPS once a day, store position if found 
    shutdownUnit();			    //Turn off power and wait
  }
  else if(mode==5){			    //Mode 5 = Possibly Free of ice, attempt to transmit data
    underIceLateMode(); 		    //Look for GPS, Attempt to Send Iridium Data if position found
    shutdownUnit();			    //Turn off power and wait
  }    
}

void initializePins(){
  pinMode(SHUTDOWN,OUTPUT);		        //SHUTDOWN PIN for turning unit off (Sleep mode)
  digitalWrite(SHUTDOWN,HIGH);			//SHUTDOWN PIN HIGH keeps unit's power on
  pinMode(TEMP_SWITCH,OUTPUT);			//TEMP_SWITCH PIN for switching between thermistor and reference resistor
  digitalWrite(TEMP_SWITCH,HIGH);		//TEMP_SWITCH PIN HIGH for reading thermistor
  pinMode(SENSOR_POWER_5V,OUTPUT);		//SENSOR_POWER_5V PIN for turning 5V sensor power on and off
  digitalWrite(SENSOR_POWER_5V,HIGH);		//SENSOR_POWER_5V PIN HIGH for 5V sensor power off (P-Channel Transistor)
  pinMode(SENSOR_POWER_3V3,OUTPUT);		//SENSOR_POWER_3V3 PIN for turning 3.3V power on and off
  digitalWrite(SENSOR_POWER_3V3,HIGH);		//SENSOR_POWER_3V3 PIN HIGH for 3.3V sensor power off (P-Channel Transistor)
  pinMode(GPS_POWER,OUTPUT);			//GPS_POWER PIN for turning power to GPS on and off
  digitalWrite(GPS_POWER,HIGH);			//GPS_POWER PIN HIGH for GPS power off (P-Channel Transistor)
  pinMode(IRIDIUM_POWER,OUTPUT);		//IRIDIUM_POWER PIN for turning power to Iridium on and off
  digitalWrite(IRIDIUM_POWER,HIGH);		//IRIDIUM_POWER PIN HIGH for Iridium power off (P-Channel Transistor)
  pinMode(IRIDIUM_ON_OFF,OUTPUT);		//IRIDIUM_ON_OFF PIN for Iridium sleep or awake
  digitalWrite(IRIDIUM_ON_OFF,LOW);		//IRIDIUM_ON_OFF PIN LOW for Iridium sleep mode
  pinMode(ACCEL_CS,OUTPUT);			//ACCEL_CS PIN for SPI communication to Accelerometer
  digitalWrite(ACCEL_CS,HIGH);			//ACCEL_CS PIN HIGH for SPI communication to Accelerometer inactive
  pinMode(SDCARD_CS,OUTPUT);			//SDCARD_CS PIN for SPI communication to SDCard
  digitalWrite(SDCARD_CS,HIGH);    		//ACCEL_CS PIN HIGH for SPI communication to SDCard inactive
  pinMode(RTC_CS,OUTPUT);			//RTC_CS PIN for SPI communication to RTC
  digitalWrite(RTC_CS,HIGH);			//ACCEL_CS PIN HIGH for SPI communication to RTC inactive
}

void setupRTCandSD(){
  SPI.setDataMode(SPI_MODE1);		//Set SPI Data Mode to 1 for RTC
  RTC.begin();                          //Start RTC (defined by RTC instance in global declarations)
  wakeupTime = RTC.now();               //Get time from RTC and store into global variable for when unit awoke
  SPI.setDataMode(SPI_MODE0);           //Set SPI Data Mode to 0 for SDCard
  SD.begin(SDCARD_CS);                  //Start SD Card so it is available for other function calls 
  SPI.setDataMode(SPI_MODE2);           //Set SPI Data Mode to default
}

char selectModeandSetAlarm(){
  alarmtime = wakeupTime.unixtime() + FAILSAFE_TIMER;  		//failsafe if program freezes (t+1day)																				
  char modeSelect=0;  						//initialize variable to select mode	
  
 if(wakeupTime.unixtime()<unitStart.unixtime()){               	//if time is before first sample time, set alarm for first sample time and go to sleep																		
    modeSelect=0;
    alarmtime = unitStart.unixtime();
 }	
  else if((wakeupTime.unixtime()<(releaseTime.unixtime()-PRE_RELEASE_WAKEUP_TIME))&&wakeupTime.unixtime()>=unitStart.unixtime()){      	  //if time is earlier than pre-release wakeup, just take bottom sample and go to sleep																								
    modeSelect=1;	
	long preReleaseWakeUpBuffer=PRE_RELEASE_WAKEUP_TIME+BOTTOM_SAMPLE_INTERVAL+10;							  //must be at least this many seconds from release to take normal bottom sample.  10s buffer added so unit does not skip the correct wake up time
    if(wakeupTime.unixtime()<(releaseTime.unixtime()-preReleaseWakeUpBuffer)){          						  //If still far enough from release for a normal bottom sample, set alarm for time + BOTTOM_SAMPLE_INTERVAL     
      alarmtime = wakeupTime.unixtime()+BOTTOM_SAMPLE_INTERVAL;            				
    }   	
    else{    																  //If within 1 BOTTOM_SAMPLE_INTERVAL of pre release wake up, then set alarm for pre release wake up time                                                            
      alarmtime = releaseTime.unixtime()-PRE_RELEASE_WAKEUP_TIME;          
    } 
  }
  else if((wakeupTime.unixtime()>=(releaseTime.unixtime()-PRE_RELEASE_WAKEUP_TIME))&&(wakeupTime.unixtime()<=(releaseTime.unixtime()))){  //if time is equal or after pre-release wakeup but before or equal to release, then wait for profile
    modeSelect=2;
	long postReleaseWorstCase=PRE_RELEASE_WAKEUP_TIME+UNDER_ICE_SAMPLE_INTERVAL;
    alarmtime = releaseTime.unixtime()+postReleaseWorstCase;       
  }  
  else if((wakeupTime.unixtime()>releaseTime.unixtime())&&(wakeupTime.unixtime()<sendDataDate.unixtime())){     		          //if after Release Date and Before Send Data Date then sample data and go to sleep. (Look for GPS once a day and Don't attempt to transmit data)
    modeSelect=4;
	alarmtime = wakeupTime.unixtime()+UNDER_ICE_SAMPLE_INTERVAL;     	
  }  
  else if(wakeupTime.unixtime()>=sendDataDate.unixtime()){        									  //if after Send Data Date, look for GPS and send Iridium Attempt if GPS lock found								                                                        
    modeSelect=5;
	alarmtime = wakeupTime.unixtime()+UNDER_ICE_SAMPLE_INTERVAL;     	
  }  

  setAlarmTime();		//Set the Alarm by writing to RTC registers
  return modeSelect; 		//return mode for program flow
}


void sampleData(char modeSelected){
  unsigned int pressureVal10 = readPressure(true);	    //Read 10 bar Pressure Sensor
  unsigned int pressureVal3 = readPressure(false);	    //Read 3 Bar Pressure Sensor
  int parVal = readPAR();				    //Read PAR Sensor
  float tiltInvCos = readAccel();			    //Get Tilt Angle from Accelerometer
  int tempRefVal = 0;					    //Initialize Temperature Reference Value
  if(modeSelected!=3){					    //If not profiling, then check and store the temperature reference value (This takes ~1 second for voltage to stabilize on switch. Profile must sample much faster and reference value is not needed at every sample for data quality)
    tempRefVal = readTemp(true);			    //Read Temperature Reference Resistor Value
  }
  int tempVal = readTemp(false);		            //Read Thermistor Resistor Value
  
  SPI.setDataMode(SPI_MODE0);                               //Set SPI Data Mode to 0 for SDCard 
  File dataFile;					    //Initialize dataFile
  if(modeSelected==1){                                      //If just a bottom sample
    dataFile = SD.open("botdat.txt", FILE_WRITE);           //open file with write permissions
  }
  else if(modeSelected==2||modeSelected==3){		    //if profile or waiting for profile
    dataFile = SD.open("prodat.txt", FILE_WRITE);           //open file with write permissions      
  }
  else if(modeSelected==4||modeSelected==5){		    //If under ise or at surface
    dataFile = SD.open("icedat.txt", FILE_WRITE);           //open file with write permissions
  }
  
  if (dataFile) {                                           //if file is available, write to it:
    dataFile.print(F("/"));                                 //Mark the start of a new dataset
    if(modeSelected==3){				    //If in profiling mode, then we want a new timestamp for each sample
      SPI.setDataMode(SPI_MODE1);                           //Set SPI Data Mode to 2 for reading RTC
      DateTime now = RTC.now();				    //Get Timestamp from RTC
      SPI.setDataMode(SPI_MODE0);                           //Set SPI Data Mode to 0 for SD Card 
      dataFile.print(now.unixtime(),HEX);                   //write timestamp to file
    }
    else{
      dataFile.print(wakeupTime.unixtime(),HEX);            //If not in profiling mode, use the timestamp for when the unit woke up
    }
    dataFile.print(F(","));                                 //,
    dataFile.print(pressureVal10);                          //ADC Value from 10 bar pressure sensor
    dataFile.print(F(","));                                 //,
    dataFile.print(pressureVal3);                           //ADC Value from 3 bar pressure sensor
    dataFile.print(F(","));                                 //,
    dataFile.print(tempRefVal);                      	    //ADC Value from Temperature Reference Resistor
    dataFile.print(F(","));                        	    //,
    dataFile.print(tempVal);                   		    //ADC Value from Temperature Thermistor
    dataFile.print(F(","));                      	    //,
    dataFile.print(parVal);                     	    //ADC Value from PAR Sensor
    dataFile.print(F(","));                      	    //,
    dataFile.print(tiltInvCos,0);                 	    //1000*Cosine of angle from vertical (4 decimal places gives <<1 degree resolution, factor of 1000 removes decimal place and 0)
    dataFile.close();					    //Close data file (Also flushes datastream to make sure all data is written)
  }
  SPI.setDataMode(SPI_MODE2);                         	    //Set SPI mode to default
}


void waitForProfile(){
  unsigned int pressureAverageLong = averagePressureLong();			//store baseline reading for average pressure (20 readings over 20 seconds)
  unsigned int pressureAverageShort = averagePressureShort();			//store reading for current pressure (3 readings over 300 ms)
  unsigned int pressureChangeVal = 328*DEPTH_CHANGE_TO_TRIP_PROFILE;		//calculate ADC reading change needed for desired depth change 
  SPI.setDataMode(SPI_MODE1);                                     		//Set SPI Data Mode to 1 for reading RTC
  DateTime now = RTC.now();							//Get current time from RTC
  while(pressureAverageShort>(pressureAverageLong-pressureChangeVal)){          //while short average of last 3 depth readings is less still deeper than cutoff depth to trigger profile
    now = RTC.now();                                     			//update the RTC time
    if(now.unixtime()>(releaseTime.unixtime()+PRE_RELEASE_WAKEUP_TIME)){        //if past expected release window, shut the unit down (alarm already set for releaseTime+Wait Period+Sample Interval)
		missedProfile();						//store message on data file indicating unit missed the profile
		shutdownUnit();							//immediately just shut down unit and don't continue with collect profile function
    }
	delay(500);								//wait half a second between sample bins
    pressureAverageShort=averagePressureShort();                  		//update the short depth average
  }
}

unsigned int averagePressureLong(){						//function to get baseline depth reading (used to establish depth before while waiting for profile)
  long depthAverage=0;								//initialize depth average (long datatype so we can sum 20 samples without overflow)
  for(char sample=0;sample<20;sample++){					//do this 20 times
    depthAverage=depthAverage+readPressure(true); 				//running sum of depth reading from 10 bar sensor
    delay(1000);								//delay 1 second between samples
  }
  depthAverage=depthAverage/20;							//divide sum by 20 to get an average (will truncate any decimals)
  return depthAverage;								//return average value, will automatically convert to unsigned int type
}

unsigned int averagePressureShort(){						//function to get an average of 3 depth readings to see if unit is ascending and profile should begin
  long depthAverage=0;								//initialize depth average (long datatype so we can sum 3 samples without overflow)
  for(char sample=0;sample<3;sample++){						//do this 3 times
    depthAverage=depthAverage+readPressure(true); 				//running sum of depth reading from 10 bar sensor
    delay(100);									//delay 100ms between samples
  }
  depthAverage=depthAverage/3;							//divide sum by 3 to get an average (will truncate any decimals)
  return depthAverage;								//return average value, will automatically convert to unsigned int type 
}

void collectProfile(){								//function to collect data once profile has been triggered by depth change 
  long profileLengthMillis=PROFILE_LENGTH_SECONDS*1000;			        //calculate length of profile in milliseconds
  long endProfileTime=millis()+profileLengthMillis;        			//timestamp for end of profile
  long quarterSecond=millis()+250;  						//timestamp to trigger next data sample
  while(millis()<endProfileTime){						//while current timestamp is less than profile cutoff timestamp, keep taking samples
    while(millis()<quarterSecond){}						//loop to take sample every 250 ms (4Hz)
      sampleData(3);								//function call to sample sensors (3=profile mode, don't take reference resistor reading)
      quarterSecond=millis()+250;						//timestamp to trigger next data sample
  }
}

void missedProfile(){								//function to write message to sd card if profile was never triggered at appropriate time 
	SPI.setDataMode(SPI_MODE0);                                     	//Set SPI Data Mode to 0 for SDCard
	File dataFile=SD.open("prodat.txt", FILE_WRITE);			//open file with write permissions
	if (dataFile) {                                           	        //if the file is open
 		dataFile.print(F(",MISSED_PROFILE")); 				//print message saying profile has been missed
		dataFile.close(); 						//close the file
	}
	SPI.setDataMode(SPI_MODE2);                               	        //Set SPI Data Mode back to normal
}

void underIceEarlyMode(){							//mode for when unit is expected to be locked under ice, don't waste time or battery trying to send data
  turnOff3v3SensorPower();							//turn off 3.3V power for sensors (no longer needed)
  turnOff5vSensorPower();							//turn off 5V power for sensors (no longer needed)
  if(wakeupTime.hour()==15){							//if hour==15 (3pm, usually warmest time of day...most likely to be ice free)
    lookForGPS(1);								//look for GPS once and store position if found
  }
}										//other than looking for GPS once a day, unit will just sample data and go back to sleep in this mode

void underIceLateMode(){										//mode for when unit is expected to be possibly free of ice
  turnOff3v3SensorPower();										//turn off 3.3V power for sensors (no longer needed)
  turnOff5vSensorPower();										//turn off 5V power for sensors (no longer needed)
  if(wakeupTime.hour()==7||wakeupTime.hour()==15||wakeupTime.unixtime()>iceFreeDate.unixtime()){	//if hour==7 or 15 (morning and afternoon), OR if past the date it's expected to be totally free of ice
    lookForGPS(GPS_SUCCESSES_TO_SEND);									//look for GPS n times.  send data if found successfully n times in a row
  }
  if(foundGPS==true){											//if GPS is found successfully n times in a row
    sendIridiumData();											//try to send data over iridium
  }													//if GPS is not found n times successfully, then shut down unit 
}

void lookForGPS(char numTimes){										//function to search for GPS - looks for GPS n times and returns foundGPS=true if found successfully n times
  float maxtiltInvCos = readAccel();									//initialize max tilt angle variable
  float tiltInvCos=maxtiltInvCos;									//initialize tilt angle reading, set to same as max reading
  for(int n=0;n<numTimes;n++){										//loop n times
	  foundGPS=false;										//start each loop assuming GPS has not been found
	  turnOnGPSPower();										//turn on power to GPS chip
	  delay(100);											//short delay to let GPS chip initialize and begin streaming data
	  ss.begin(9600);										//begin software serial comms at 9600 baud with GPS chip
	  long gpsSearchMillis = GPS_SEARCH_SECONDS*1000;						//calculate number os milliseconds to search for GPS
	  long startSearchMillis=millis();								//timestamp for when GPS search started  (we want to log the time to fix)
      long endSearchMillis=startSearchMillis+gpsSearchMillis;						//timestamp for when to end GPS search
	  while((millis()<endSearchMillis)&&(foundGPS==false)){						//while time to search is not over and gps hasn't been foundGPS
	    while ((ss.available() > 0)&&(foundGPS==false)){						//while data stream from GPS chip is incoming and gps hasn't been found	
		  if (gps.encode(ss.read())){								//parse incoming data and everytime a full message is encoded, execute the following
			if(n==0){  									//if this is the first time searching, we want to record the max tilt angle during search (some indication of sea state)
			  tiltInvCos = readAccel();							//read instantaneous tilt angle
			  if(tiltInvCos<maxtiltInvCos){							//if new tilt angle is more than old tilt angle (current cosine value is less than old cosine value)
				maxtiltInvCos=tiltInvCos;						//store the current cosine value into the max angle
			  }
			}				
			if (gps.location.isValid()&&gps.date.isValid()&&gps.time.isValid()){		//if a valid fix is found (location, time, and date all valid)
			  foundGPS=true;								//flag GPS fix has been found
			  if(n==0){  									//if this is the first time searching, then record the fix, time to fix, and max tilt angle during search (some indication of sea state) 
				  storeGPSFix(gps.location.lat(), gps.location.lng(), gps.time.value(), gps.date.value(), millis()-startSearchMillis, maxtiltInvCos);
			  }
		    }
		  }  
	    }
	  }												//keep searching and parsing data until GPS has been found or search timeout is exceeded
	  ss.end();											//end software serial comms with GPS chip
      turnOffGPSPower();										//turn off GPS power in between searches (last position stored in chip and has coin cell for hot start)
      if(foundGPS==true&&(n<numTimes-1)){								//if unit still needs to search for GPS again
		long gpsDelaySearchMillis = GPS_SECONDS_BETWEEN_SEARCHES*1000;				//calculate the delay needed between searches in milliseconds
		delay(gpsDelaySearchMillis);								//delay between searches
	  }
	  else{												//if this is the last time to search
		n=numTimes;										//increment n so search isn't executed again
	  }
  }
}
  
void storeGPSFix(double gpsLat, double gpsLon, unsigned long gpsTimeVal, unsigned long gpsDateVal, long timeToFix, float maxtiltInvCos){
  SPI.setDataMode(SPI_MODE1);                           //Set SPI Data Mode to 1 for reading RTC
  DateTime now = RTC.now();				//Get current time to compare to GPS time for clock drift
  SPI.setDataMode(SPI_MODE0);                        	//Set SPI Data Mode to 0 for SD Card Use
  File dataFile = SD.open("icedat.txt", FILE_WRITE); 	//open the file with write permissions
  timeToFix=timeToFix/1000;				//convert timetofix from ms to seconds
  if (dataFile) {                                    	//if the file is available, write to it:
    dataFile.print(F(","));                          	//, 	
	dataFile.print(gpsLat,4);                     	//GPS Latitude with 4 decimal places (~11m accuracy, best possibly without DGPS)   		
	dataFile.print(F(","));                         //,	
	dataFile.print(gpsLon,4);                       //GPS Latitude with 4 decimal places (~11m accuracy, best possibly without DGPS) 
    dataFile.print(F(","));                          	//,	
	dataFile.print(gpsDateVal);                     //GPS Date (DDMMYY format)
	dataFile.print(F(","));                         //,	
	dataFile.print(gpsTimeVal);                     //GPS Time (HHMMSSCC format)
	dataFile.print(F(","));                         //,	
    dataFile.print(timeToFix,0);			//Seconds to acquire fix (helps with diagnostics)
    dataFile.print(F(","));                          	//,	
    dataFile.print(maxtiltInvCos,0);			//maxTilt while searching for GPS	//1000*Cosine of angle from vertical (4 decimal places gives <<1 degree resolution, factor of 1000 removes decimal place and 0) 
    dataFile.print(F(","));                          	//,	
    dataFile.print(now.unixtime(),HEX);			//RTC Time in UNIXTIME
	dataFile.close();				//Close data file (Also flushes datastream to make sure all data is written)
  }
  SPI.setDataMode(SPI_MODE2);                         	//Set SPI Data Mode back to default
}

void sendIridiumData(){
  turnOnIridiumPower();					    						//switch on voltage to Iridium module 
  delay(50);						    						//short delay to let voltage stabilize 
  int signalQuality = -1;				    						//Initialize signal quality variable (0-5)
  nss.begin(19200);  					    						//start serial comms on nss pins defined above
  isbd.setPowerProfile(0); 				    						//1 for low power application (USB, limited to ~90mA, interval between transmits = 60s)
  isbd.setMinimumSignalQuality(2);			    						//minimum signal quality (0-5) needed before sending message.  normally set to 2 (by isbd recommendations)
  isbd.begin();  					    						//wake up the Iridium module and prepare it to communicate
  
  const int sendbuffer_size = CHARS_PER_SBDMESSAGE+1;	    						//calculate size of array for send message buffer
  char iridiumSendBuffer[sendbuffer_size];		    						//initialize array for send message buffer (reserve adequate space in memory)
    iridiumSendBuffer[0] = 'H';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[1] = 'e';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[2] = 'l';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[3] = 'l';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[4] = 'o';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[5] = '!';			            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)
    iridiumSendBuffer[6] = '\0';		            						//First message to send if unit wake up is "Hello!"  (preferred to not send data in case of file corruption - unit will still respond)  
  uint8_t iridiumReceiveBuffer[15];			    						//Initialize array for received message (will only send short messages of 12 chars to change filename/position)
  size_t receiveBufferSize = sizeof(iridiumReceiveBuffer);  						//Define size of array for received message 
  int err = isbd.sendReceiveSBDText(iridiumSendBuffer, iridiumReceiveBuffer, receiveBufferSize);	//Attempt to send first message ("I am alive!")
  if (err != 0){return;}										//If sending message fails, then exit routine and shutdown unit 
  
  char whichFile=getWhichFile(iridiumReceiveBuffer[0]);							//First character in received message designates which file to read
  unsigned long filePosition=getFilePosition(iridiumReceiveBuffer);					//Parse the received message and return value for where to start reading the file (default is 0)

  while(whichFile!='x'){										//'x' is marker for all files being completely sent; as long as this is not true, keep parsing and sending data
    fillIridiumBuffer(whichFile, filePosition, iridiumSendBuffer);					//Fill the send message buffer based on file and file position 
	if(iridiumSendBuffer[0]!=0){									//As long as the message contains data
          err = isbd.sendReceiveSBDText(iridiumSendBuffer, iridiumReceiveBuffer, receiveBufferSize);	//attempt to send message
		if (err != 0){return;}									//If sending message fails, then exit routine and shutdown unit 
	}
	whichFile=getWhichFile(iridiumReceiveBuffer[0]);						//Use updated received message to change file (if necessary, otherwise continue with current file)
	filePosition=getFilePosition(iridiumReceiveBuffer);						//Use updated received message to change file position (if necessary, otherwise continue with current file position)
  }
  turnOffIridiumPower();										//Once unit is done sending messages, turn off power to the iridium module 
}

unsigned long getFilePosition(uint8_t receiveBuffer[]){		//Function to parse received message and get file position (example message = 'p,1234567890' or 'p,95')
  unsigned long filePos=0;					//unsigned long is a maximum of 10 digits (4,294,967,295)
  for(int d=2;d<12;d++){					//for chars 2-11 in the message
	if(receiveBuffer[d]!=0){				//if there is a digit in the next slot
	  filePos=filePos*10+receiveBuffer[d]-'0';		//multiply the previous value by 10 and add the new digit
	}
  }
  return filePos;						//return the parsed value for position as a single number
}

char getWhichFile(char fileChar){				//return char to designate which file to read;  'p' = prodat.txt (profile data), 'i'=icedat.txt (under ice data), 'b'=botdat.txt (bottom data), 'x'=exit (all files complete)
  if(fileChar=='i'||fileChar=='b'){				//if message specifically designates icedat or botdat, select the appropriate file
	return fileChar;
  }
  else{								//if message does not designate anything, or designates prodat, then select prodat
	return 'p';
  }
}

void fillIridiumBuffer(char &fileLabel, unsigned long &filePos, char sendbuffer[]){	//function to fill message based on file and file position
  SPI.setDataMode(SPI_MODE0);                                                           //Set SPI Data Mode to 0 for sd card
  File currentFile;									//Initialize file variable
  if(fileLabel=='p'){									//'p' designates profile data (prodat.txt)
    currentFile=SD.open("prodat.txt", FILE_READ);		                        //open file with read access
  }
  else if(fileLabel=='i'){								//'i' designates under ice data (icedat.txt)
    currentFile=SD.open("icedat.txt", FILE_READ);  		                        //open file with read access
  }
  else if(fileLabel=='b'){								//'p' designates bottom data (botdat.txt)
    currentFile=SD.open("botdat.txt", FILE_READ);  		                        //open file with read access
  }	
  if(currentFile){									//If file is open for reading
	currentFile.seek(filePos);							//move forward to proper file position
	for(int i=0;i<CHARS_PER_SBDMESSAGE;i++){			                //parse one char at a time for length of message
	  if(currentFile.available()){						        //If not yet at end of the file
		  sendbuffer[i]=currentFile.read();  			                //put the next char into the send message buffer
		  filePos++;								//increment file position each time a character is read
		}      
	  else{										//If end of file has been reached
	    fileLabel=nextFile(fileLabel);					        //Get marker for the next file to read
		filePos=0;								//Reset File position to start of next file
		sendbuffer[i]='\0';							//Mark the end of the message will null character
		currentFile.close();							//close the file
		SPI.setDataMode(SPI_MODE2);                                             //Set SPI Data Mode back to default
		return;									//exit the routine and go directly to sending message (if it has any data)
	  }
	}
	sendbuffer[CHARS_PER_SBDMESSAGE]='\0';				                //if send message buffer is filled to max size, mark the end with null character
  }  
  else{
	  fileLabel=nextFile(fileLabel);					        //if file does not open or is not available, go to the next file
  }
  currentFile.close();									//close the file
  SPI.setDataMode(SPI_MODE2);                      		                        //Set SPI Data Mode back to default
}

char nextFile(char fileLabel){			//function for selecting the next file based on the current file (when file does not open or end of file has been reached)
  if(fileLabel=='p'){				//send prodat.txt first.  if prodat.txt is finished
    return 'i';					//move to icedat.txt
  }
  else if(fileLabel=='i'){			//if icedat.txt has finished
    return 'b';					//move to botdat.txt
  }
  else if(fileLabel=='b'){			//if botdat.txt has finished, then all files are done
    return 'x';				        //'x' for exit.  all files complete so stop sending data	
  }
  else{
    return 'x';	  				//failsafe if something strange happens, exit and stop sending data
  }
}

void loop(){  				
  shutdownUnit();				//If program exits routine unexpectedly (all contained in 'setup'), then shut down						
}

unsigned int readPressure(boolean deepsensor){	//depth in m = [(reading-16384)*maxbar/32768]*10
  byte p3status;				//initialize variable for sensor status
  unsigned int p3;				//initialize variable for pressure reading 
  byte eocDelay=8;				//variable for conversion delay (8 ms defined in Keller communication protocol)
  char kellerADDR = keller3bar;			//initialize I2C address 
  if(deepsensor){				//if reading deep pressure sensor (10 bar),
    kellerADDR = keller10bar;			//then change I2C address
  }
  Wire.beginTransmission(kellerADDR);  		//Begin I2C comms with pressure sensor
  Wire.write(checkpressure);  			//Send write command to start pressure conversion 
  Wire.endTransmission();			//End command for pressure conversion 
  delay(eocDelay);                       	//Delay for conversion (8 ms defined in Keller communication protocol)
  Wire.requestFrom(kellerADDR,3);        	//Read data from pressure sensor
  while(Wire.available()){               	//Ensure all the data comes in
    p3status = Wire.read(); 			//First byte is Sensor Status  (Can possibly be used for error checking)
    p3 = Wire.read(); 				//Second byte is Pressure reading MSB
    p3 = ((unsigned int)p3 << 8);		//Shift second byte to MSB 
    p3 += Wire.read(); 				//Third byte is LSB, add to pressure reading
  }
  Wire.endTransmission();			//End I2C communication for reading pressure
  return p3;					//Return value for pressure only
}

void setupTempADC(){
  Wire.beginTransmission(ads1100A0);		//Begin I2C comms with Temp ADC 
  Wire.write(B10001101); 			//set ADC gain to 2, 8 Samples per Second (16-bit Resolution)
  Wire.endTransmission();			//End I2C Comms
  delay(50);					//Short delay to let ADC initialize
}

int readTemp(boolean ref){			//function to read temperature ADC (for thermistor and reference resistor)
  int delaytime=500;				//milliseconds to delay between reference resistor and thermistor reading (needed for voltage to stabilize and geet a good reading)
  if(ref){					//if reference is true (i.e. we want to read the reference resistor)
    digitalWrite(TEMP_SWITCH,LOW);		//send signal to switch to reference reading
    delay(delaytime);				//delay to let voltage stabilize
  }
  int measurement = readTempADC();		//read the ADC (Same for thermistor or reference)
  if(ref){					//if reference is true (i.e. we have just read the reference resistor)
    digitalWrite(TEMP_SWITCH,HIGH);		//send signal to switch back to thermistor reading
    delay(delaytime);				//delay to let voltage stabilize
  }
  return measurement;				//return the measured value from the ADC
}

int readTempADC(){							
  byte controlRegister = 0;			//initialize control registoer variable 
  int adcVal = 0;				//initialize ADC Value variable
  Wire.requestFrom(ads1100A0, 3);  		//request 3 bytes from appropriate ADC using I2C
  while(Wire.available()){ 			//ensure all the data comes in
    adcVal = Wire.read(); 		        //first byte is MSB 
    adcVal = ((unsigned int)adcVal << 8);	//shift first byte 8 bits to the left to move it into MSB
    adcVal += Wire.read(); 			//second byte is LSB.  add this to the MSB
    controlRegister = Wire.read();		//third byte is control register
  }
  return adcVal;				//return single value for ADC reading
}

void setupPARADC(){
  Wire.beginTransmission(ads1100A1);		//Begin I2C comms with Temp ADC 
  Wire.write(B10001100); 			//set ADC gain to 1, 8 Samples per Second (16-bit Resolution)
  Wire.endTransmission();			//End I2C Comms
  delay(50);					//Short delay to let ADC initialize
}

int readPAR(){
  byte controlRegister =0;			//initialize control registoer variable 
  int adcVal = 0;				//initialize ADC Value variable
  Wire.requestFrom(ads1100A1, 3);  	        //request 3 bytes from appropriate ADC using I2C
  while(Wire.available()){ 			//ensure all the data comes in
    adcVal = Wire.read(); 			//first byte is MSB 
    adcVal = ((unsigned int)adcVal << 8);	//shift first byte 8 bits to the left to move it into MSB
    adcVal += Wire.read(); 			//second byte is LSB.  add this to the MSB
    controlRegister = Wire.read();	        //third byte is control register
  }
  return adcVal;				//return single value for ADC reading
}


void setupAccel(){				//Setup ADXL345 Chip 
  SPI.setDataMode(SPI_MODE3);                   //Set SPI Data Mode to 3 for Accelerometer
  sendSPIdata(ACCEL_CS,0x2C,B00001010);		//0x2C=data rate and power mode control, B00001010=normal power mode, SPI data rate =100HZ
  sendSPIdata(ACCEL_CS,0x31,B00001011);		//0x31=data format control, B00001011=no self test, SPI mode, Active LOW, full resolution mode, left justified (MSB), +/-16g range 
  sendSPIdata(ACCEL_CS,0x2D,B00001000);		//0x2D=power control, B00001000=measurement mode
  SPI.setDataMode(SPI_MODE2);                   //Set SPI Data Mode to default
}

float readAccel(){				//Read tilt angle from ADXL345 Chip
  SPI.setDataMode(SPI_MODE3);		        //Set SPI Data Mode to 3 for Accelerometer
  digitalWrite(ACCEL_CS, LOW);			//Select Accelerometer for SPI Communication	
  SPI.transfer(0x32 | 0xC0);			//Request x,y,and z Acceleration Values from Accelerometer 
  byte x0 = SPI.transfer(-1);			//x0=Accel LSB
  byte x1 = SPI.transfer(-1);			//x1=Accel MSB
  byte y0 = SPI.transfer(-1);			//y0=Accel LSB
  byte y1 = SPI.transfer(-1);			//y1=Accel MSB
  byte z0 = SPI.transfer(-1);			//z0=Accel LSB
  byte z1 = SPI.transfer(-1);			//z1=Accel MSB
  digitalWrite(ACCEL_CS, HIGH);			//De-Select Accelerometer for SPI Communication
  SPI.setDataMode(SPI_MODE2);           	//Set SPI Data Mode to default

  float x = x1<<8 | x0;				//Combine x1(MSB) and x0(LSB) into x
  float y = y1<<8 | y0;				//Combine y1(MSB) and y0(LSB) into y
  float z = z1<<8 | z0;				//Combine z1(MSB) and z0(LSB) into z
  float xg = x*3.9/1000;			//Convert x accel value to g force units
  float yg = y*3.9/1000;			//Convert x accel value to g force units
  float zg = z*3.9/1000;			//Convert x accel value to g force units
  float gForce=sqrt(sq(xg)+sq(yg)+sq(zg));	//Find Total G force (3 dim Pythagorean)
  float tiltCos=yg/gForce;			//Find y component of G force (equal to cosine of angle from vertical)
  tiltCos=tiltCos*1000;				//Multiply by 1000 to get desired resolution and save 2 characters (0 and decimal point) for data output
  return tiltCos;				//Return the cosine of the angle from vertical (Arduino doesn't know ArcCos)
}

void sendSPIdata(char pin, byte b1, byte b2){	//generic function for sending 2 bytes of SPI data
  digitalWrite(pin, LOW);			//Select designated chip for SPI Communication
  SPI.transfer(b1);				//Transfer first byte
  SPI.transfer(b2);				//Transfer second byte
  digitalWrite(pin, HIGH);			//De-Select designated chip for SPI Communication
}

void setAlarmTime(){
  SPI.setDataMode(SPI_MODE1);                   //Set SPI Data Mode to 1 for RTC
  RTCWrite(0x87,alarmtime.second() & 0x7F);     //set alarm time: seconds     //87=write to location for alarm seconds    //binary & second with 0x7F required to turn alarm second "on"
  RTCWrite(0x88,alarmtime.minute() & 0x7F);     //set alarm time: minutes     //88=write to location for alarm minutes    //binary & minute with 0x7F required to turn alarm minute "on"
  RTCWrite(0x89,alarmtime.hour() & 0x7F);       //set alarm time: hour        //89=write to location for alarm hour       //binary & hour with 0x7F required to turn alarm hour "on"
  RTCWrite(0x8A,alarmtime.day() & 0x3F);        //set alarm time: day         //8A=write to location for alarm day        //binary & day with 0x3F required to turn alarm day "on" (not dayofWeek) 
  RTCWrite(0x8B,0);                             //Set Alarm #2 to zll zeroes (disable)
  RTCWrite(0x8C,0);                             //Set Alarm #2 to zll zeroes (disable)
  RTCWrite(0x8D,0);                             //Set Alarm #2 to zll zeroes (disable)
  RTCWrite(0x8F,B00000000);                     //reset flags                //8F=write to location for control/status flags    //B00000000=Ocillator Stop Flag 0, No Batt Backed 32 KHz Output, Keep Temp CONV Rate at 64 sec (may change later), disable 32 KHz output, temp Not Busy, alarm 2 not tripped, alarm 1 not tripped
  RTCWrite(0x8E,B00000101);                     //set control register       //8E=write to location for control register        //B01100101=Oscillator always on, SQW on, Convert Temp off, SQW freq@ 1Hz, Interrupt enabled, Alarm 2 off, Alarm 1 on
  SPI.setDataMode(SPI_MODE2);                   //Set SPI Data Mode to default
}

void RTCWrite(char reg, char val){
  digitalWrite(RTC_CS, LOW);                 //Select RTC for SPI Communication
  SPI.transfer(reg);                         //Send RTC register 
  SPI.transfer(bin2bcd(val));                //Send value to RTC register in proper format
  digitalWrite(RTC_CS, HIGH);                //De-Select RTC for SPI Communication
}

void turnOn3v3SensorPower(){
  digitalWrite(SENSOR_POWER_3V3,LOW);  		//Pin LOW=Power ON (P-Channel switch)
}
void turnOff3v3SensorPower(){
  digitalWrite(SENSOR_POWER_3V3,HIGH);    	//Pin HIGH=Power OFF (P-Channel switch)
}
void turnOn5vSensorPower(){
  digitalWrite(SENSOR_POWER_5V,LOW);  		//Pin LOW=Power ON (P-Channel switch)
}
void turnOff5vSensorPower(){
  digitalWrite(SENSOR_POWER_3V3,HIGH);  	//Pin HIGH=Power OFF (P-Channel switch)
}
void turnOnGPSPower(){
  digitalWrite(GPS_POWER,LOW);  		//Pin LOW=Power ON (P-Channel switch)
}
void turnOffGPSPower(){
  digitalWrite(GPS_POWER,HIGH);  		//Pin HIGH=Power OFF (P-Channel switch)
}	
void turnOnIridiumPower(){
  digitalWrite(IRIDIUM_POWER,LOW);  		//Pin LOW=Power ON (P-Channel switch)
  digitalWrite(IRIDIUM_ON_OFF,HIGH);  		//Digital Pin for Iridium module sleep.  HIGH = wake from sleep
}
void turnOffIridiumPower(){
  digitalWrite(IRIDIUM_POWER,HIGH);  		//Pin HIGH=Power OFF (P-Channel switch)
  digitalWrite(IRIDIUM_ON_OFF,LOW);  		//Digital Pin for Iridium module sleep.  LOW = go to sleep
}

void shutdownUnit(){
  digitalWrite(SHUTDOWN,LOW);			//Pin Low=Power to entire unit off
  delay(100000);				//long delay to ensure nothing else happens and unit shuts down
}
	
bool ISBDCallback()				//This function executes in the background while doing certain Iridium functions, we don't need it to do anything here
{
   return true;					//Must return true to continue using ISBD protocol
}


