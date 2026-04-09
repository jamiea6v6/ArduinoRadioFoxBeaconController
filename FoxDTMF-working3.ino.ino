// FoxDTMF-working3.ino

// Notes:  Set beacon cw text and make sure the radio is turned up at least half volume!
//         After beacon transmits, it will listen for one of the commands below for a fixed amount of seconds
//         DTMF 1 - Transmit beacon
//         DTMF 2 - Tturn off transmit beacon
/* Arduino Ham Radio Fox beacon program
   Uses a basic interface circuit board and a hacked Baofeng Mic cable from one of the typical earphone speaker/mics provided with each radio
   to interface Arduino and a K style connector HT
   I use a Quansheng K6 with firmware loaded to reduce transmit power to 20mw for easier hunting.
   Additionally, I usually use frequency 146.595 when Fox hunting on 2M usually.
   The Quansheng radio will run for a really long time with 20mw output
   The Arduino can be run separately by a 9v battery or similar.  A 9v alkaline will provide for a fairly long hunt.
   
   The program sends a morse beacon of call sign and 'fox', then a tone for 30 seconds
   after the beacon portion, the Fox listens for DTMF audio received over the air by the HT
   - if the radio receives a "1" DTMF tone pair, it goes back to beacon
   - if the radio receives a "2" DTMF tone pair during listening, it waits indefinitely (no longer transmitting)
   - sending a 1 during the wait will send the code back to the beacon routine and will TX morse
   - if no 1 or 2 key presses are detected, will wait for a predetermined amount of time and then go back to Beacon again
   
   I was inspired by many other hams and hobbyists and need to figure out which scripts I took the beacon from for credit.
   The DTMF receive was also an idea others had used successfully with Arduino bare hardware and Goertzel algorithm to save cpu effort.
   Ultimately I used a combination of ChatGPT online to start after experimenting with other people's software
   and then moved into OpenCode.ai run locally using GPT-5 Nano model to modify/enhance the Arduino script
   I never found a Goertzel library I got just right, so all the DTMF code is done in the script with no libraries.
   
   The interface board uses any common small bipolar transistor to switch PTT.  When the Arduino writes high to a specified digital pin, this 
   is connected to the transistor base.  The Collector/Emitter are hooked to PTT and Ground, so the typical Baofeng/Kenwood/etc
   radio can be keyed by grounding PTT pin at the speaker mic connector.
   The audio generated for beaconing on the Arduino is PWM audio on a digital pin that is passed through a basic r/c filter for smoother audio tones
   Finally the DTMF audio in from the HT is capacitively coupled to a resistor divider network that sets the voltage at A0 analog in at 2.5v between gnd and 5v rail
   for better signal quality at the sampling pin
   The radio should be set to about 50% volume for receive audio to A0 to be sufficient for detecting DTMF tones received.
   
   Quirks - Sometimes my Quansheng HT will go into flashlight mode when I turn everything on.  Best to turn the radio on, then the Arduino, then plug in the speaker/mic connector
   Happy Hunting! */

#include <cww_MorseTx.h>

// User-configurable parameters (move near the top for clarity)

// Beacon: Morse CW transmission
const int CW_SPEED = 20;        // Morse speed (words per minute)
const int TONE_FREQ = 1000;     // CW tone frequency (Hz)
const int PIN_LED   = 3;        // LED/Key pin used by cww_MorseTx
const int PIN_SND   = 13;       // Audio out pin used by cww_MorseTx

// IO pins
const int AUDIO_PIN = 13;         // Audio output for beacon tone
const int PTT_PIN   = 12;         // Transmit enable / key radio input
const int DTMF_PIN  = A0;         // Analog input for DTMF detection

// DTMF frequencies (for 697/770/852/941 x 1209/1336/1477/1633)
const float lowFreqs[4]  = {697, 770, 852, 941};
const float highFreqs[4] = {1209, 1336, 1477, 1633};

// DTMF keypad map
const char dtmfMap[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Sampling settings
const int N = 205;
const float SAMPLING_FREQ = 8900.0f;

// Thresholds
const float MAGNITUDE_THRESHOLD = 1.0e6;

// Time-window parameter for listen loop (ms)
const unsigned long LISTEN_WINDOW_MS = 30000;

// Sample buffer
int samples[N];

// Message for Morse beacon (editable at compile time by editing this string)
char MORSE_MESSAGE[] = "KM4WYO FOX";

// Morse transmitter instance (reuses the same object in beacon loop)
cww_MorseTx morseWithTone(PIN_LED, CW_SPEED, PIN_SND, TONE_FREQ);

// Runtime state
enum LoopState { BEACON, DTMF_LISTEN };
LoopState loopState = BEACON;
bool continuousDTMF = false; // DTMF listening mode activation flag

// Goertzel: compute magnitude^2 for a target frequency from a sample block
float goertzel(int* data, int numSamples, float targetFreq, float samplingFreq) {
  float omega = (2.0 * 3.1415926535 * targetFreq) / samplingFreq;
  float coeff = 2.0 * cos(omega);

  float sPrev = 0;
  float sPrev2 = 0;
  float sCurr;

  for (int i = 0; i < numSamples; i++) {
    sCurr = (float)data[i] + coeff * sPrev - sPrev2;
    sPrev2 = sPrev;
    sPrev = sCurr;
  }

  float power = sPrev2 * sPrev2 + sPrev * sPrev - coeff * sPrev * sPrev2;
  return power;
}

// Beacon routine
void runBeacon() {
  // Key the radio
  digitalWrite(PTT_PIN, HIGH);
  delay(1000); // give radio time to key cleanly before CW starts

  // Send Morse beacon
  morseWithTone.send(MORSE_MESSAGE);
  delay(1000);

  // 1 kHz tone for beacon
  tone(AUDIO_PIN, 1000);
  delay(20000);
  noTone(AUDIO_PIN);

  // Unkey radio
  delay(1000);
  digitalWrite(PTT_PIN, LOW);
}

void setup() {
  Serial.begin(9600);
  // Initialize LED/Radio Key pin
  pinMode(PTT_PIN, OUTPUT);
}

void loop() {
  // BEACON first, then switch to DTMF_LISTEN
  if (loopState == BEACON) {
    runBeacon();
    loopState = DTMF_LISTEN;
  }

  // DTMF_LISTEN: interpret decoded DTMF digits to control flow
  if (loopState == DTMF_LISTEN) {
    long listenStart = millis();
    // Listen for either a fixed window or indefinitely in continuous mode
    while ((continuousDTMF == false && (millis() - listenStart) < LISTEN_WINDOW_MS) || (continuousDTMF == true)) {
      // Collect ONE block of samples
      unsigned long startMicros = micros();
      unsigned long sampleInterval = (unsigned long)(1000000.0 / SAMPLING_FREQ);

      for (int i = 0; i < N; i++) {
        samples[i] = analogRead(DTMF_PIN);
        while ((micros() - startMicros) < (i + 1) * sampleInterval) {
          // wait for next sample time
        }
      }

      // Compute magnitude^2 for all 8 frequencies from the SAME sample block
      float lowMag[4];
      float highMag[4];

      for (int i = 0; i < 4; i++) {
        lowMag[i]  = goertzel(samples, N, lowFreqs[i], SAMPLING_FREQ);
        highMag[i] = goertzel(samples, N, highFreqs[i], SAMPLING_FREQ);
      }

      // Find strongest low frequency
      int lowIndex = 0;
      float maxLow = lowMag[0];
      for (int i = 1; i < 4; i++) {
        if (lowMag[i] > maxLow) {
          maxLow = lowMag[i];
          lowIndex = i;
        }
      }

      // Find strongest high frequency
      int highIndex = 0;
      float maxHigh = highMag[0];
      for (int i = 1; i < 4; i++) {
        if (highMag[i] > maxHigh) {
          maxHigh = highMag[i];
          highIndex = i;
        }
      }

      // Validate: both must exceed absolute threshold
      if (maxLow < MAGNITUDE_THRESHOLD || maxHigh < MAGNITUDE_THRESHOLD) {
        continue;
      }

      // Validate: twist ratio (prevent harmonics from fooling us)
      float minLow = lowMag[0];
      for (int i = 1; i < 4; i++) if (lowMag[i] < minLow) minLow = lowMag[i];
      float minHigh = highMag[0];
      for (int i = 1; i < 4; i++) if (highMag[i] < minHigh) minHigh = highMag[i];

      float lowRatio  = (minLow > 0)  ? maxLow  / minLow  : 999;
      float highRatio = (minHigh > 0) ? maxHigh / minHigh : 999;

      // Strongest must dominate the others
      if (lowRatio < 1.5 || highRatio < 1.5) {
        continue;
      }

      // Valid DTMF tone detected
      char key = dtmfMap[lowIndex][highIndex];
      Serial.print("Key: ");
      Serial.println(key);

      // Debounce - wait for tone to end
      delay(150);

      // Act on decoded key
      if (key == '1') {
        // Exit to BEACON (non-continuous mode)
        continuousDTMF = false;
        loopState = BEACON;
        break;
      } else if (key == '2') {
        // Enter/keep continuous listening
        continuousDTMF = true;
      } else {
        // Other keys: ignore for now
      }
    }

    // If we exit the listen window without receiving a '1', determine next step
    if (loopState == DTMF_LISTEN && continuousDTMF == false) {
      // timed window ended without 1 -> go back to beacon
      loopState = BEACON;
    }
  }
}