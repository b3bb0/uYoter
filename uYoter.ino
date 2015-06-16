/*
 * uYoter, uF toYota-er v 0.4
 * 
 * TOYOTA (before 1996) car utility computer
 * 
 * PINS:  A   T    Mode   A = Arduino Pin  |  T = ATMEGA Pin 
 *  +     2   4    I/O    12v 4 Channel Wireless Remote (on switch 1) & CAR door UNLOCK switch
 *  +     3   5     I     12v check if keys are inserted
 *  +     4   6    I/O    12v 4 Channel Wireless Remote (on switch 2) & CAR door LOCK switch
 *  +     5  11     O     SSR (solid state relay) to self battery charger
 *  +     6  12     O     SSR (solid state relay) to hazard flashers AKA 4 blinkers
 *  +     7  13     I     Connected to the ECU (Engine Control Unit)
 *  -     8  17     O     SSR fridge (peltier cell)
 *  +    A0  23     I     12v first battery voltage check http://www.electroschematics.com/9351/arduino-digital-voltmeter/
 *  +    A1  24     I     12v second battery voltage check
 *  +    A5  28     I     fridge thermometer
 */
#include <avr/sleep.h>
#include <EEPROM.h>
#include <math.h>
#include "PinChangeInterrupt.h"
#include "Toyota.h"
#include "PulsePattern.h"

#define serialDebug true


#define UNLOCK_PIN  2
#define KEY_PIN     3
#define LOCK_PIN    4
#define CHARGER_PIN 5
#define HAZARD_PIN  6
#define TOYOTA_PIN  7
#define FRIDGE_PIN  7

#define BATT1_SENSOR  A1
#define BATT2_SENSOR  A2
#define FRIDGE_SENSOR A5

#define V12ON LOW // when 12v is detected is LOW

// EEPROM configuration [start]
#define CONFIG_VERSION "004"
#define CONFIG_START 32
struct StoreStruct {
	// conf variables
	unsigned shutDown;
	unsigned fridgeMin;
	unsigned fridgeMax;
	char uYoter_v[4]; // it is the last variable of the struct so when settings are saved, they will only be validated if they are stored completely.
} settings = {
	60 * 5, // 5 minutes
	4, // fridge min temp (celcius)
	5, // fridge max temp (celcius)
	CONFIG_VERSION
};
// EEPROM configuration [stop]


// main variables
boolean ECUconnected = false;
boolean carCharger = false;
boolean doorsLocked = false;

unsigned lastAction;

Toyota Toyota(TOYOTA_PIN, LOW, HIGH);

uint8_t hazardOn[6]  = { 500,500,500,500,500,500 };
uint8_t hazardOff[2] = { 1000,1000 };

/*
 * 0 = just on
 * 1 = waken-up
 * 3 = car ON
 * 7 = going to sleep soon
 * 9 = sleeping
 */
int status = 1;

// 12v battery resistors
float BATT_R1 = 100000.0;
float BATT_R2 = 10000.0;

void setup() {
	
	// restore configuration from the EEprom
	loadConfig();
	
	runStatus(1);
	
	pinMode(HAZARD_PIN, OUTPUT);
	pinMode(CHARGER_PIN, OUTPUT);
	pinMode(KEY_PIN, INPUT_PULLUP);
	pinMode(UNLOCK_PIN, INPUT_PULLUP);
	pinMode(LOCK_PIN, INPUT_PULLUP);
	
	// analog inputs
	pinMode(BATT1_SENSOR, INPUT);
	pinMode(BATT2_SENSOR, INPUT);
	pinMode(FRIDGE_SENSOR, INPUT);
	
	attachPinChangeInterrupt(digitalPinToPinChangeInterrupt(TOYOTA_PIN), myChange, CHANGE);
	
	if (serialDebug) {
		Serial.begin(115200);
		Serial.println("System Started");
	}
	
}

void myChange() {
	Toyota.change();
}

// VOID LOOP
void loop() {
	
	// check if we have to sleep?!
	int dateDiff = (millis() / 1000) - lastAction;
	if ( status==7 && dateDiff  > settings.shutDown) {
		runStatus(9);
	} else if ( status==7 && dateDiff  > (settings.shutDown - 10) && !doorsLocked) {
		// lock doors automatically 10 seconds before the shut down
		if (serialDebug) Serial.println("Automatic door lock!");
		PPGenerator.init(HAZARD_PIN, hazardOff, sizeof(hazardOff) , LOW, PRESCALE_1024, false);
		doorsLocked = true;
		
		// TODO: lock doors?
	} else if (status==7) {
		//Serial.print(dateDiff);
		//Serial.print(" - ");
		//Serial.println(settings.shutDown);
	}
	
	// check car key [start]
	if (digitalRead(KEY_PIN)==V12ON) {
		runStatus(3);
		
		checkFridge();
	} else {
		if (status!=9 && status!=7 ) {
			runStatus(7);
		}
		if (digitalRead(LOCK_PIN)==V12ON && !doorsLocked) {
			if (serialDebug) Serial.println("Doors manually locked");
			PPGenerator.init(HAZARD_PIN, hazardOff, sizeof(hazardOff) , LOW, PRESCALE_1024, false);
			doorsLocked = true;
		}
		if (digitalRead(UNLOCK_PIN)==V12ON && doorsLocked) {
			lastAction = millis() / 1000;
			if (serialDebug) Serial.println("Doors manually un-locked");
			PPGenerator.init(HAZARD_PIN, hazardOn, sizeof(hazardOn) , LOW, PRESCALE_1024, false);
			doorsLocked = false;
		}
	}
	// check car key [stop]
	
	
	Toyota.status();
	
	if ( Toyota.isConnected ) {
		if (serialDebug) {
			Serial.print("0: ");
			Serial.println(Toyota.readData(0));
			
			Serial.print("1: ");
			Serial.println(Toyota.readData(1));
			
			Serial.print("2: ");
			Serial.println(Toyota.readData(2));
		}
	} else if (ECUconnected) {
		ECUconnected = false;
		if (serialDebug) Serial.println("Lost Connection");
	}
} 

void runStatus(int code) {
	if (code==status) return;
	
	switch (code) {
		
		case 0:
			if (serialDebug) Serial.println("Started...");
			status = code;
		case 1:
		
		// JUST BOOTED / AWAKEN [start]
			if (serialDebug) Serial.println("Awake!");
			lastAction = millis() / 1000;
			doorsLocked = false;
			status = code;
			
			break;
		// JUST BOOTED / AWAKEN [stop]
		
		case 3:
		
		// KEY IS ON [start]
			
			if (serialDebug) Serial.println("Key is IN!");
			lastAction = millis() / 1000;
			status = 3;
			doorsLocked = false;
			
			// self battery 
			if (!carCharger) {
				if (serialDebug) Serial.println(" - self battery charger connected");
				digitalWrite(CHARGER_PIN,HIGH);
				carCharger = true;
			}
			 
			break;
		// KEY IS ON [stop]
		
		case 7:
		
		// ALMOST BED TIME [start]
			if (serialDebug) Serial.println("Almost bed time...");
			lastAction = millis() / 1000;
			status = 7;
			
			// self battery 
			if (carCharger) {
				if (serialDebug) Serial.println(" - self battery unplugged");
				digitalWrite(CHARGER_PIN,LOW);
				carCharger = false;
			}
			
			break;
		// ALMOST BED TIME [stop]
		
		case 9:
		
		// GOING TO SLEEP [start]
			if (serialDebug) Serial.println("Going to sleep...");
			lastAction = millis() / 1000;
			status = 9;
			
			// make sure the self battery if off
			digitalWrite(CHARGER_PIN,LOW);
			carCharger = false;
			
			attachInterrupt(0,wakeUpNow, V12ON); // 0 = pin2
			attachInterrupt(1,wakeUpNow, V12ON); // 1 = pin3
			
			delay(100);
			
			set_sleep_mode(SLEEP_MODE_PWR_DOWN);
			sleep_enable();
			sleep_mode();

			/* The program will continue from here. */
			/* First thing to do is disable sleep. */
			sleep_disable(); 
			
			break;
		// GOING TO SLEEP [stop]
			
		default:
			if (serialDebug) Serial.print("Unknown status: ");
			if (serialDebug) Serial.println(code);
	}
}

void wakeUpNow() {
	Serial.println("got wake up!");
	detachInterrupt(0);
	detachInterrupt(1);
	
	if (serialDebug) Serial.println("Flashing lights");
	PPGenerator.init(HAZARD_PIN, hazardOn, sizeof(hazardOn) , LOW, PRESCALE_1024, false);
	lastAction = millis() / 1000;
	
	runStatus(1);
}


void loadConfig() {
	if (EEPROM.read(CONFIG_START + sizeof(settings) - 2) == settings.uYoter_v[2]
	 && EEPROM.read(CONFIG_START + sizeof(settings) - 3) == settings.uYoter_v[1]
	 && EEPROM.read(CONFIG_START + sizeof(settings) - 4) == settings.uYoter_v[0]){
		if (serialDebug) Serial.println("Loading EEPROM configuration: ");
		for (unsigned int t=0; t<sizeof(settings); t++) {
			*((char*)&settings + t) = EEPROM.read(CONFIG_START + t);
		}
	} else {
		if (serialDebug) Serial.println("Resetting EEPROM configuration to default");
		// settings aren't valid! will overwrite with default settings
		saveConfig();
	}
}


void saveConfig() {
	for (unsigned int t=0; t<sizeof(settings); t++) {
		// write the data
		EEPROM.write(CONFIG_START + t, *((char*)&settings + t));
		// and verifies the data
		if (EEPROM.read(CONFIG_START + t) != *((char*)&settings + t)) {
			// error writing to EEPROM (TODO: handle this!)
		}
	}
}




//Function to perform the fancy math of the Steinhart-Hart equation
double Thermister(int RawADC) {
	double Temp;
	Temp = log(((10240000/RawADC) - 10000));
	Temp = 1 / (0.001129148 + (0.000234125 + (0.0000000876741 * Temp * Temp ))* Temp );
	Temp = Temp - 273.15;              // Convert Kelvin to Celsius
	return Temp;
}

double Volts(int PIN) {
	float vout = (analogRead(PIN) * 5.0) / 1024.0;
	float vin = vout / (BATT_R2/(BATT_R1+BATT_R2)); 
	if (vin<0.09) return 0.0;
	return vin;
} 

void checkFridge() {
	// TODO: study/research how to work on isolated batteries to keep the fridge running also engine OFF
	if (Volts(BATT1_SENSOR)>13) {
		
		double temp = Thermister(analogRead(FRIDGE_SENSOR));
		
		if (temp < settings.fridgeMin) {
			digitalWrite(FRIDGE_PIN,LOW);
		} else if(temp > settings.fridgeMax) {
			digitalWrite(FRIDGE_PIN,HIGH);
		}
	}
}
