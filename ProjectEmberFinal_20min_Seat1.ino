#include <EEPROMVar.h>
#include <EEPROMex.h>
#include <Adafruit_VS1053.h>
#include <Adafruit_DotStar.h>
#include <SPI.h>

#define NUMPIXELS 		60
#define DATA_PIN		22
#define CLOCK_PIN		26

#define BUTTON_PIN		2	// Reset button pin location
#define LED_PIN			13	// LED for testing

#define HEAD_LOC		0	// Location of header information in EEPROM
#define COUNT_LOC		2	// Location of count information in EEPROM
#define START_LOC		4	// Location of where timecards begin in EEPROM
#define MAX_CARD_COUNT	818	// Number of cards stored in EEPROM (Pretend that there are only 64 bytes to play with)
#define EEPROM_SIZE		4094	// Number of bytes available in EEPROM
#define CARD_SIZE		5	// Number of bytes taken up by a block
#define MAX_TIME		3000 // 5 minutes //864000	// Number of (1/10th) seconds until time reset
#define MIN_DURATION	5

#define RESET_PIN		-1		// Unused pin
#define CS_PIN			7		// Chip select
#define DCS_PIN			6		// Data/command select
#define CARDCS_PIN		4		// SD Card chip select
#define DATA_REQ_PIN	3		// Data request interrupt

#define LOWVOL	180
#define HALFVOL	30
#define HIGHVOL	10
#define SECONDS			10	// Seconds in a decisecond

// The length of time (deciseconds) that a ramp lasts
#define PERIOD			15

// The length of time (deciseconds) before ramping down to half volume. This value
// is two seconds longer than it actually plays for, since it adds in the initial ramp
// up. Since it's set at 80ds, it would play for 60ds.
#define SWITCH			60
#define AM8				0	// 8:00 AM in deciseconds
#define PM11			864000	// 11:00 PM in deciseconds

#define QUARTET_1 		255		// Bit mask byte 1
#define QUARTET_2 		65280	// Bit mask byte 2
#define QUARTET_3 		16711680 // Bit mask byte 3

#define BIAS_O		15
#define BIAS_N		0
#define BIAS_U	   -15
#define BASE_LEVEL	329
#define THRESH_H (BASE_LEVEL + (BIAS_O * 2))
#define THRESH_L (BASE_LEVEL + (BIAS_U * 2))

struct uint24_t {
	// bytes go in order determined by the EEPROMex function writeBlock()
	byte b3;	// most significant
	byte b2;
	byte b1;	// least significant
};

struct timeCard {
	uint24_t timeIn;		// Tenths of a second since midnight
	uint16_t duration;		// Tenths of a second since timeIn
};

enum level {
	OVER,
	NONE,
	UNDER
};

level getSeatLevel(int reading) {
	if (reading > THRESH_H) {
		return OVER;
	} else if (reading < THRESH_L) {
		return UNDER;
	} else {
		return NONE;
	}
}

class FixReading {
private:
	int bias;
	level last;
	int last1;
	int last2;
	int last3;
public:
	FixReading(int num) {
		last1 = num;
		last2 = num;
		last3 = num;

		// the bias will be added to the average in a way that makes it differ
		// more greatly from the baselevel. If the reading would be over, than
		// it's given a raise. This means it has to go even lower (essentially
		// to the base level) to switch back. This helps with debouncing.
		bias = BIAS_N;
		last = getSeatLevel(num);
	}
	int operator () (int num) {
		level seat = getSeatLevel(num + bias);
		if (seat != last) {
			switch (seat) {
			case OVER:
				bias = BIAS_O;
				break;
			case NONE:
				bias = BIAS_N;
				break;
			case UNDER:
				bias = BIAS_U;
				break;
			}
			last = seat;
		}
		
		int average = (num + last1 + last2 + last3) / 4;
		last3 = last2;
		last2 = last1;
		last1 = num;
		return average + bias;
	}
};

// Conversion from uint24_t to uint32_t
uint32_t blockToLong(const uint24_t & block) {
	return block.b1 + ((uint16_t)block.b2 << 8) + ((uint32_t)block.b3 << 16);
}

// Conversion from uint32_t to uint24_t
uint24_t longToBlock(const uint32_t & longNum) {
	uint24_t block;
	block.b1 =  longNum & QUARTET_1;
	block.b2 = (longNum & QUARTET_2) >> 8;
	block.b3 = (longNum & QUARTET_3) >> 16;
	return block;
}

bool isTCEqual(const timeCard &first, const timeCard &second) {
	if (first.timeIn.b1 != second.timeIn.b1) {return false;}
	if (first.timeIn.b2 != second.timeIn.b2) {return false;}
	if (first.timeIn.b3 != second.timeIn.b3) {return false;}
	if (first.duration != second.duration) {return false;}
	return true;
}

enum playState
{
	PLAYING = 0,
	DONE,
	NOPLAY,
	START
};

// Used to store current data between loops
uint32_t timeIn;
uint32_t timeOut;
unsigned long millisIn;
unsigned long timeSince;
const struct timeCard emptyCard = {{0,0,0},0}; 
struct timeCard currentCard;
uint32_t strainTimeSince;

playState state;
bool cardSet;

bool buttonDown;
bool wasDown;
uint32_t buttonTimeIn;
uint32_t buttonTimeOut;

uint16_t storedCards;
uint16_t headAddress;

const char *trackName = "track001.mp3";

FixReading fix(BASE_LEVEL);

Adafruit_VS1053_FilePlayer player = Adafruit_VS1053_FilePlayer(
	RESET_PIN,
	CS_PIN,
	DCS_PIN,
	DATA_REQ_PIN,
	CARDCS_PIN
);

Adafruit_DotStar strip = Adafruit_DotStar(
	NUMPIXELS, 
	DATA_PIN, 
	CLOCK_PIN, 
	DOTSTAR_BRG
);

void(* reboot)(void) = 0;

void setup() 
{
	pinMode(BUTTON_PIN, INPUT);
	pinMode(LED_PIN, OUTPUT);
	Serial.begin(9600);
	while(!Serial){} // Wait until serial communication can begin

	// Music Player setup
	if (!player.begin()) {
		Serial.println(F("No VS1053"));
		while (1);
	}
	Serial.println(F("VS1053 found"));
	player.setVolume(LOWVOL, LOWVOL);
	player.useInterrupt(VS1053_FILEPLAYER_PIN_INT); // Data request interrupt
	
	// Initialize SD card
	SD.begin(CARDCS_PIN);

	strip.begin();

	EEPROM.setMaxAllowedWrites(16000);
	storedCards = EEPROM.readInt(COUNT_LOC);
	headAddress = EEPROM.readInt(HEAD_LOC); // Tells us where the first card is (or would be) in EEPROM

	cardSet = false;

	state = NOPLAY;

	buttonDown = false;
	wasDown = false;
	strainTimeSince = 0;
	
	delay(50);
	player.startPlayingFile(trackName);
	strip.show();
}

void loop() 
{
	uint32_t mSec = millis() / 100;	// Get the current time in tenths of a second
	uint32_t sec = mSec % MAX_TIME;

	if (mSec >= (MAX_TIME * 50)) { // If we've passed 50 days
		reboot();
		Serial.println("####### REBOOT #######");
	}

	// checking for time of day:
	int daytime;
	if (sec > AM8 && sec < PM11) {
		daytime = true;
	} else {
		daytime = false;
	}
	
	/*Serial.print(sec);
	switch(state) {
		case PLAYING: 	Serial.println(" - PLAYING"); break;
		case DONE: 		Serial.println(" - DONE"); break;
		case NOPLAY: 	Serial.println(" - NOPLAY"); break;
		case START: 	Serial.println(" - START"); break;
		default:		Serial.println("WHUT?!"); break;
	}*/

	// PLAYING MUSIC
	if (player.stopped()) {
		player.startPlayingFile(trackName);
	}

	uint8_t volume = LOWVOL;
	uint8_t brightness = 0;
	switch (state) {
		case PLAYING	:
		// Wait until we have to stop playing
			timeSince = millis() - millisIn;
			timeSince /= 100;
			
			volume = getVolume(daytime, timeSince, currentCard.duration);
			brightness = getBrightness(timeSince, currentCard.duration);
			
			Serial.print("Vol: ");
			Serial.print(volume);
			Serial.print("\tBri: ");
			Serial.println(brightness);			
			
			if (timeSince > (unsigned long)currentCard.duration){
				state = DONE;
			}
			break;
		case DONE	:	
		// Stop the music, update the currentCard and header information, then move on
			removeCard();
			timeIn = 0;
			timeSince = 0;
			currentCard = emptyCard;
			
			state = NOPLAY;
			cardSet = false;

			volume = LOWVOL;

			digitalWrite(LED_PIN, LOW);
			//player.stopPlaying();
			
			break;
		case NOPLAY	:
		// Wait until we have a card (and can play)
			if (storedCards > 0 && !cardSet) {	// Only moves on if there is a card to play
				Serial.println("Card set");
				EEPROM.readBlock(headAddress, currentCard);	// currentCard is now the new card
				timeIn = blockToLong(currentCard.timeIn);
				timeOut = timeIn + currentCard.duration;
				cardSet = true;
			}

			if (cardSet) {
				Serial.print(sec);
				Serial.print(" ~ ");
				Serial.print(timeIn);
				Serial.print(":");
				Serial.println(timeOut);
				
				if (timeIn < timeOut) {
					if (sec >= timeIn && sec < timeOut) {
						state = START;
					}
				} else {
					//Serial.print("timeIn greaterthan timeOut\n");
					if (sec >= timeIn) {
						state = START;
					}
				}
			}
			
			break;
		case START	:	// Play the music
			state = PLAYING;
			millisIn = millis(); // the time when we start playing according to the actual clock
			
			//player.startPlayingFile(trackName);
			
			break;
		default:
			// Just in case I suck more than I think I do
			state = NOPLAY;
	};
	player.setVolume(volume, volume);

	for (int i = 0; i < NUMPIXELS; i++) {
		strip.setPixelColor(i, brightness);
	}
	strip.show();

	// CHECKING INPUT

	if (mSec > strainTimeSince) {
		//Serial.print("Reading Seat\n");
		// Read button input here through custom get method
		getSeatState(buttonDown);
		
		if (buttonDown && !wasDown) {	// Just sat down; record the time
			buttonTimeIn = sec;
			Serial.print("Time in:\t");
			Serial.println(buttonTimeIn);
	
			wasDown = true;
		}
	
		if (!buttonDown && wasDown) {	// Just sat down, record time and set new card
			buttonTimeOut = sec;
			timeCard newCard;
			
			newCard.timeIn = longToBlock(buttonTimeIn);
			if (buttonTimeOut > buttonTimeIn) {
				newCard.duration = buttonTimeOut - buttonTimeIn;
				Serial.print("Time out:\t");
				Serial.println(buttonTimeOut);
			} else {
				newCard.duration = (MAX_TIME - buttonTimeIn) + buttonTimeOut;
			}
			Serial.print("Time In:\t");
			Serial.println(blockToLong(newCard.timeIn));
			Serial.print("Duration:\t");
			Serial.println(newCard.duration);
	
			// New card gets added here
			addCard(newCard);
			wasDown = false;
		}
		strainTimeSince = mSec;
	}
}

void getSeatState(bool &seatState) {
	/*/ This is the simple button code
	if (digitalRead(BUTTON_PIN) == HIGH) {
		seatState = true;
	} else {
		seatState = false;
	}*/

	//Serial.println("Get seat state");
	// This is the strain gauge version (the final (it works))
	float reading_Now = fix(analogRead(1));  // analog-in pin 1 for Strain 2

	if (getSeatLevel(reading_Now) == NONE) {
		seatState = false;
	} else {
		seatState = true;
	}
}

void removeCard () {
	if (storedCards > 0){
		storedCards--;
	}
	
	headAddress += CARD_SIZE;
	if (headAddress >= EEPROM_SIZE) {
		headAddress = START_LOC;
	}
	
	EEPROM.updateInt(COUNT_LOC, storedCards);
	Serial.print("Stored updated to ");
	Serial.println(storedCards);
	EEPROM.updateInt(HEAD_LOC, headAddress);
	Serial.print("Head updated to ");
	Serial.println(headAddress);

	// Don't worry about zero-ing out the bytes in EEPROM. That uses up read/writes.
	// Instead, whether a byte is important is determined by header and count
}

void addCard (const timeCard &card) {
	if (storedCards < MAX_CARD_COUNT && card.duration > MIN_DURATION) { // Only do this if we have room
		int cardLocation = headAddress + (CARD_SIZE * storedCards);
		if (cardLocation >= EEPROM_SIZE) {
			cardLocation -= (EEPROM_SIZE - START_LOC);
		}

		storedCards++;
		EEPROM.update(COUNT_LOC, storedCards);

		EEPROM.updateBlock(cardLocation, card);
	} else if (card.duration <= MIN_DURATION) {
		Serial.println("Card dropped, too short;");
	}
}

// Get used to these nested ifs. I can't do squat about it right now,
// and I hate myself for doing it like this. It could be cleaner, but it
// just needs to work.
// b_DayTime should be set to true if it's between 8AM and 8PM
// deltaTime should be the time passed since the music began playing
uint8_t getVolume(bool b_DayTime, unsigned long deltaTime, unsigned long wholeTime){

	if (deltaTime > wholeTime) { return LOWVOL; }
	// This means it is nightTime, play softly
	if (!b_DayTime)
	{
		// Too short
		if (wholeTime < (PERIOD * 2))
		{
			Serial.println("Short");
			int halfTime = wholeTime / 2;

			if (deltaTime < halfTime) {
				return (uint8_t)map(deltaTime, 0, halfTime, LOWVOL, HALFVOL);
			} else {
				return (uint8_t)map(deltaTime, halfTime, wholeTime, HALFVOL, LOWVOL);
			}
		} else
		{
			Serial.println("Long QUIET");
			if (deltaTime < PERIOD) {
				return (uint8_t)map(deltaTime, 0, PERIOD, LOWVOL, HALFVOL);
			} else if (deltaTime >= PERIOD && deltaTime < wholeTime - PERIOD) {
				return (uint8_t)HALFVOL;
			} else {
				return (uint8_t)map(deltaTime, (wholeTime - PERIOD), wholeTime, HALFVOL, LOWVOL);
			}
		}
	}

	// It's not Day time, play loudly depending on the timescale
	// Ellapsed time is too short for full envelope
	if (wholeTime < (PERIOD * 2))
	{
		//Serial.print("4 Second or less\t");
		int halfTime = wholeTime / 2;

		if (deltaTime < halfTime) {
			//Serial.println("Ramp Up");
			return (uint8_t)map(deltaTime, 0, halfTime, LOWVOL, ((HALFVOL * wholeTime) / (PERIOD * 2)));
		} else {
			//Serial.println("Ramp Down");
			return (uint8_t)map(deltaTime, halfTime, wholeTime, ((HALFVOL * wholeTime) / (PERIOD * 2)), LOWVOL);
		}
	} else if (wholeTime < (12 * SECONDS))
	{
		//Serial.print("4 to 12 Second\t");
		//Serial.print(deltaTime);
		//Serial.print(" < ");
		//Serial.print(PERIOD);
		//Serial.print("\t");
		if (deltaTime < PERIOD) {
			//Serial.println("Ramp Up");
			return (uint8_t)map(deltaTime, 0, PERIOD, LOWVOL, HIGHVOL);
		} else if (deltaTime >= PERIOD && deltaTime < wholeTime - PERIOD) {
			//Serial.println("Sustain HIGH");
			return (uint8_t)HIGHVOL;
		} else {
			//Serial.println("Ramp Down");
			return (uint8_t)map(deltaTime, (wholeTime - PERIOD), wholeTime, HIGHVOL, LOWVOL);
		}
	} else // This is the special profile
	{
		//Serial.print("Long LOUD\t");
		//Serial.print(deltaTime);
		//Serial.print(" < ");
		//Serial.print(PERIOD);
		//Serial.print("\t");
		if (deltaTime < PERIOD) {
			//Serial.println("| Ramp Up to high");
			return (uint8_t)map(deltaTime, 0, PERIOD, LOWVOL, HIGHVOL);
		} else if (deltaTime < SWITCH) {
			//Serial.println("| Sustain high");
			return (uint8_t)HIGHVOL;
		} else if (deltaTime < (SWITCH + PERIOD)) {
			//Serial.println("| Ramp down to half");
			return (uint8_t)map(deltaTime, SWITCH, (SWITCH + PERIOD), HIGHVOL, HALFVOL);
		} else if (deltaTime < (wholeTime - PERIOD)) {
			//Serial.println("| Sustain Half");
			return (uint8_t)HALFVOL;
		} else {
			//Serial.println("| Ramp Down full");
			return (uint8_t)map(deltaTime, (wholeTime - PERIOD), wholeTime, HALFVOL, LOWVOL);
		}
	}
}

uint8_t getBrightness(unsigned long delta, unsigned long whole) {
	if (whole > PERIOD * 2) {
		if (delta <= PERIOD) {
			return (uint8_t)map(delta, 0, PERIOD, 0, 255);
		} else if (delta <= whole - PERIOD) {
			return 255;			
		} else {
			return (uint8_t)map(delta, (whole - PERIOD), whole + 1, 255, 0);
		}
	} else {
		int half = whole / 2;
		if (delta <= half) {
			return (uint8_t)map(delta, 0, half, 0, 255);
		} else {
			return (uint8_t)map(delta, half, whole + 1, 255, 0);
		}
	}
}













