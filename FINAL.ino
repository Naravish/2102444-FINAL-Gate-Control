#include <SPI.h>
#include <MFRC522.h>
#include <Stepper.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>
#include <SHA256.h>
#include <RTClib.h>

// ==========================================
// CONFIGURATION & FLAGS
// ==========================================
#define RESET_PASSWORD_FLAG false // Set to true once to reset password to "1234"

// Pin definitions
#define RST_PIN A0          // RFID Reset pin
#define SS_PIN 53           // RFID Slave Select pin

// 74HC595 & 7-Segment Pins
const int latchPin = A14; 
const int dataPin  = A13; 
const int clockPin = A15; 
const int digitPins[] = {A8, A9, A10, A11};

// Stepper configuration (2048 steps per revolution, 512 steps for 90 degrees)
const int stepsPerRevolution = 2048; 
Stepper myStepper(stepsPerRevolution, 2, 4, 3, 5); // FULL4WIRE mapping for IN1-IN4 on pins 2, 4, 3, 5

// Initialize MFRC522 reader
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Initialize I2C LCD (Operator Facing)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Initialize RTC (Real-Time Clock)
RTC_DS3231 rtc;

// Keypad Configuration
const byte ROWS = 4;
const byte COLS = 4;
char hexaKeys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'-','0','+','D'}
};
byte rowPins[ROWS] = {28, 26, 24, 22};
byte colPins[COLS] = {36, 34, 32, 30};

Keypad customKeypad = Keypad(makeKeymap(hexaKeys), rowPins, colPins, ROWS, COLS);

// EEPROM Layout Configuration 
const int maxCards = 16;
const byte uidSize = 4;
const int cardListStartAddress = 0;
const int cardCountAddress = 64;
const int pwdLenAddress = 65;
const int pwdStartAddress = 66;

// Password management variables
String enteredPassword = "";
String newPasswordTemp = ""; 

// Operator UI States
enum OpState {
  OP_IDLE,
  OP_VIEW_CARDS,
  OP_REG_PASSWORD,
  OP_REG_SCAN_CARD,
  OP_DELETE_PASSWORD,
  OP_CHANGE_OLD_PASSWORD,
  OP_CHANGE_NEW_PASSWORD_1,
  OP_CHANGE_NEW_PASSWORD_2,
  OP_FORCE_OPEN_PASSWORD,
  OP_VIEW_PASSWORD
};
OpState opState = OP_IDLE;

// Timing variables
const unsigned long gateHoldOpenTime = 3000;    // 3 seconds
int targetSteps = 512; // 90 degrees rotation (2048 / 4)

// Card View & Messaging variables
int currentViewIndex = 0;
unsigned long lcdMessageTimer = 0;
bool showingMessage = false;

// Remote Serial Authentication Variables
String activeChallenge = "";
String serialInputBuffer = "";

// ==========================================
// 7-SEGMENT DISPLAY MULTIPLEXING (TIMER1)
// ==========================================
volatile byte displayChars[4] = {0xFF, 0xFF, 0xFF, 0xFF};
volatile int currentDigit = 0;

const byte SEG_S = ~0b01101101;
const byte SEG_T = ~0b01111000;
const byte SEG_O = ~0b00111111;
const byte SEG_P = ~0b01110011;
const byte SEG_E = ~0b01111001;
const byte SEG_N = ~0b00110111;
const byte SEG_BLANK = ~0b00000000;

void setupTimer1() {
  cli(); 
  TCCR1A = 0; 
  TCCR1B = 0; 
  OCR1A = 499; // 500Hz multiplexing rate
  TCCR1B |= (1 << WGM12); 
  TCCR1B |= (1 << CS11) | (1 << CS10); 
  TIMSK1 |= (1 << OCIE1A); 
  sei(); 
}

ISR(TIMER1_COMPA_vect) {
  digitalWrite(A8, LOW);
  digitalWrite(A9, LOW);
  digitalWrite(A10, LOW);
  digitalWrite(A11, LOW);

  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, displayChars[currentDigit]);
  digitalWrite(latchPin, HIGH);

  digitalWrite(digitPins[currentDigit], HIGH);
  currentDigit = (currentDigit + 1) % 4;
}

void setDisplayWord(char c1, char c2, char c3, char c4) {
  noInterrupts();
  displayChars[0] = getSegmentCode(c1);
  displayChars[1] = getSegmentCode(c2);
  displayChars[2] = getSegmentCode(c3);
  displayChars[3] = getSegmentCode(c4);
  interrupts();
}

byte getSegmentCode(char c) {
  switch (c) {
    case 'S': case 's': return SEG_S;
    case 'T': case 't': return SEG_T;
    case 'O': case 'o': return SEG_O;
    case 'P': case 'p': return SEG_P;
    case 'E': case 'e': return SEG_E;
    case 'N': case 'n': return SEG_N;
    default: return SEG_BLANK;
  }
}

// ==========================================
// TIMESTAMP FORMATTING HELPER
// ==========================================
String getTimestampStr() {
  DateTime now = rtc.now();
  char buf[25];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          now.year(), now.month(), now.day(),
          now.hour(), now.minute(), now.second());
  return String(buf);
}

void logMessage(String msg) {
  Serial.print("LOG: [");
  Serial.print(getTimestampStr());
  Serial.print("] ");
  Serial.println(msg);
}

// ==========================================
// SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(9600);
  randomSeed(analogRead(0));
  
  SPI.begin();          
  mfrc522.PCD_Init();   
  
  lcd.init();                      
  lcd.backlight();

  if (!rtc.begin()) {
    Serial.println("LOG: Couldn't find RTC!");
  }
  if (rtc.lostPower()) {
    Serial.println("LOG: RTC lost power, setting default time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  pinMode(latchPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(A8, OUTPUT);
  pinMode(A9, OUTPUT);
  pinMode(A10, OUTPUT);
  pinMode(A11, OUTPUT);

  setupTimer1();
  myStepper.setSpeed(10); 

  if (EEPROM.read(cardCountAddress) > maxCards) {
    EEPROM.write(cardCountAddress, 1);
    byte defaultUID[] = {0x3C, 0x79, 0x54, 0x83};
    for (int i = 0; i < uidSize; i++) {
      EEPROM.write(cardListStartAddress + i, defaultUID[i]);
    }
  }

  byte currentPwdLen = EEPROM.read(pwdLenAddress);
  if (RESET_PASSWORD_FLAG || currentPwdLen == 255 || currentPwdLen > 8) {
    savePassword("1234");
    logMessage("Admin password initialized/reset to '1234'.");
  }
  
  setDisplayWord('S', 'T', 'O', 'P'); 
  updateOperatorLCD();
  logMessage("System Initialized. Gate Controller Ready.");
}

void loop() {
  handleRFIDScanner();
  handleOperatorUI();
  handleSerialCommand();
}

String uidToString(byte *buffer, byte bufferSize) {
  String uidStr = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uidStr += "0";
    uidStr += String(buffer[i], HEX);
    if (i < bufferSize - 1) uidStr += " ";
  }
  uidStr.toUpperCase();
  return uidStr;
}

void executeGateOpenSequence(String source, String uidInfo = "") {
  logMessage("Gate opening triggered via " + source + (uidInfo != "" ? " | UID: " + uidInfo : ""));
  showTemporaryMessage("Gate Opening", source);
  setDisplayWord('O', 'P', 'E', 'N');

  myStepper.step(targetSteps);
  logMessage("Gate is fully OPEN.");
  
  delay(gateHoldOpenTime); 
  
  logMessage("Closing gate...");
  myStepper.step(-targetSteps);
  logMessage("Gate is fully CLOSED.");

  setDisplayWord('S', 'T', 'O', 'P');
}

void handleRFIDScanner() {
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String scannedUID = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
    
    if (opState == OP_REG_SCAN_CARD) {
      byte count = EEPROM.read(cardCountAddress);
      if (count < maxCards) {
        int writeAddress = cardListStartAddress + (count * uidSize);
        for (byte i = 0; i < uidSize; i++) {
          EEPROM.write(writeAddress + i, mfrc522.uid.uidByte[i]);
        }
        count++;
        EEPROM.write(cardCountAddress, count);
        logMessage("New RFID card registered and saved to EEPROM | UID: " + scannedUID);
        showTemporaryMessage("Card Added!", "Total: " + String(count));
      } else {
        logMessage("Card registration failed: Storage full | Attempted UID: " + scannedUID);
        showTemporaryMessage("Storage Full!", "Max 16 Cards");
      }
      opState = OP_IDLE;
    } 
    else {
      if (checkCardUID()) {
        logMessage("Access Granted | Valid RFID card detected | UID: " + scannedUID);
        executeGateOpenSequence("RFID Card", scannedUID);
      } else {
        logMessage("Access Denied | Unknown RFID card scanned | UID: " + scannedUID);
        showTemporaryMessage("Access Denied", "Unknown Card");
      }
    }
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
}

void handleOperatorUI() {
  if (showingMessage && millis() - lcdMessageTimer >= 2000) {
    showingMessage = false;
    updateOperatorLCD();
  }

  if (showingMessage) return; 

  char customKey = customKeypad.getKey();
  if (!customKey) return;

  switch (opState) {
    case OP_IDLE:
      if (customKey == '+') {
        enteredPassword = "";
        opState = OP_REG_PASSWORD;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Enter Password:");
        lcd.setCursor(0, 1);
        lcd.print("_[+]Confirm");
      } else if (customKey == 'C') {
        enteredPassword = "";
        opState = OP_VIEW_PASSWORD;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("View Cards Pwd:");
        lcd.setCursor(0, 1);
        lcd.print("_[+]Confirm");
      } else if (customKey == 'D') {
        enteredPassword = "";
        opState = OP_FORCE_OPEN_PASSWORD;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Override Password:");
        lcd.setCursor(0, 1);
        lcd.print("_[+]Confirm");
      }
      break;

    case OP_VIEW_PASSWORD:
      if (customKey == '-') {
        logMessage("Card list view operation cancelled during password entry.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          logMessage("Admin password verified for card list viewing.");
          currentViewIndex = 0;
          opState = OP_VIEW_CARDS;
          updateOperatorLCD();
        } else {
          logMessage("Incorrect password entered for card list viewing.");
          showTemporaryMessage("Wrong Password", "Access Denied");
          opState = OP_IDLE;
        }
      } else if (customKey >= '0' && customKey <= '9') {
        if (enteredPassword.length() < 8) {
          enteredPassword += customKey;
          lcd.setCursor(0, 1);
          for (int i = 0; i < enteredPassword.length(); i++) {
            lcd.print("*");
          }
          lcd.print("[+]Confirm");
        }
      }
      break;

    case OP_VIEW_CARDS:
      if (customKey == 'C') {
        opState = OP_IDLE;
        logMessage("Operator exited card list view.");
        updateOperatorLCD();
      } else if (customKey == 'A') {
        byte count = EEPROM.read(cardCountAddress);
        if (count > 0) {
          currentViewIndex = (currentViewIndex - 1 + count) % count;
          updateOperatorLCD();
        }
      } else if (customKey == 'B') {
        byte count = EEPROM.read(cardCountAddress);
        if (count > 0) {
          currentViewIndex = (currentViewIndex + 1) % count;
          updateOperatorLCD();
        }
      } else if (customKey == '-') {
        byte count = EEPROM.read(cardCountAddress);
        if (count > 0) {
          enteredPassword = "";
          opState = OP_DELETE_PASSWORD;
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Delete Card Pwd:");
          lcd.setCursor(0, 1);
          lcd.print("_[+]Confirm");
        }
      }
      break;

    case OP_FORCE_OPEN_PASSWORD:
      if (customKey == '-') {
        logMessage("Gate override operation cancelled by operator.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          logMessage("Admin password verified for physical gate override.");
          opState = OP_IDLE;
          executeGateOpenSequence("Physical Keypad Override");
          updateOperatorLCD();
        } else {
          logMessage("Incorrect admin password entered for physical gate override.");
          showTemporaryMessage("Wrong Password", "Access Denied");
          opState = OP_IDLE;
        }
      } else if (customKey >= '0' && customKey <= '9') {
        if (enteredPassword.length() < 8) {
          enteredPassword += customKey;
          lcd.setCursor(0, 1);
          for (int i = 0; i < enteredPassword.length(); i++) {
            lcd.print("*");
          }
          lcd.print("[+]Confirm");
        }
      }
      break;

    case OP_REG_PASSWORD:
      if (customKey == '-') {
        logMessage("Card registration operation cancelled during password entry.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          logMessage("Admin password verified for card registration.");
          showTemporaryMessage("Pass Correct!", "Scan New Card");
          opState = OP_REG_SCAN_CARD;
        } else {
          logMessage("Incorrect admin password entered for card registration.");
          showTemporaryMessage("Wrong Password", "Access Denied");
          opState = OP_IDLE;
        }
      } else if (customKey >= '0' && customKey <= '9') {
        if (enteredPassword.length() < 8) {
          enteredPassword += customKey;
          lcd.setCursor(0, 1);
          for (int i = 0; i < enteredPassword.length(); i++) {
            lcd.print("*");
          }
          lcd.print("[+]Confirm");
        }
      }
      break;

    case OP_REG_SCAN_CARD:
      if (customKey == '-') {
        logMessage("Card scan registration cancelled by operator.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      }
      break;

    case OP_DELETE_PASSWORD:
      if (customKey == '-') {
        logMessage("Card deletion operation cancelled during password entry.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_VIEW_CARDS;
        updateOperatorLCD();
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          byte count = EEPROM.read(cardCountAddress);
          int baseAddr = cardListStartAddress + (currentViewIndex * uidSize);
          byte deletedUIDBytes[4];
          for (byte b = 0; b < uidSize; b++) {
            deletedUIDBytes[b] = EEPROM.read(baseAddr + b);
          }
          String deletedUID = uidToString(deletedUIDBytes, uidSize);

          for (int i = currentViewIndex; i < count - 1; i++) {
            for (int b = 0; b < uidSize; b++) {
              byte nextByte = EEPROM.read(cardListStartAddress + (i + 1) * uidSize + b);
              EEPROM.write(cardListStartAddress + i * uidSize + b, nextByte);
            }
          }
          count--;
          EEPROM.write(cardCountAddress, count);
          logMessage("Card successfully deleted from EEPROM | Index: " + String(currentViewIndex) + " | UID: " + deletedUID);
          showTemporaryMessage("Card Deleted!", "");
          currentViewIndex = 0;
          opState = OP_VIEW_CARDS;
          updateOperatorLCD();
        } else {
          logMessage("Incorrect password entered for card deletion.");
          showTemporaryMessage("Wrong Password", "Access Denied");
          opState = OP_VIEW_CARDS;
          updateOperatorLCD();
        }
      } else if (customKey >= '0' && customKey <= '9') {
        if (enteredPassword.length() < 8) {
          enteredPassword += customKey;
          lcd.setCursor(0, 1);
          for (int i = 0; i < enteredPassword.length(); i++) {
            lcd.print("*");
          }
          lcd.print("[+]Confirm");
        }
      }
      break;

    default:
      break;
  }
}

// ==========================================
// REMOTE SERIAL CHALLENGE-RESPONSE & COMMAND PARSER
// ==========================================
void handleSerialCommand() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        processCommand(serialInputBuffer);
        serialInputBuffer = "";
      }
    } else {
      serialInputBuffer += c;
    }
  }
}

void processCommand(String cmd) {
  cmd.trim();
  
  if (cmd.equalsIgnoreCase("CHALLENGE")) {
    long challengeNum = random(100000, 999999);
    activeChallenge = String(challengeNum);

    logMessage("Remote challenge requested. Generated challenge: " + activeChallenge);
    Serial.print("CHALLENGE:");
    Serial.println(activeChallenge);
  } 
  else if (cmd.startsWith("AUTH:")) {
    int firstSpace = cmd.indexOf(' ');
    if (firstSpace == -1) {
      logMessage("Remote serial error: PARSING_ERROR");
      Serial.println("ERROR:PARSING_ERROR");
      return;
    }

    String authPart = cmd.substring(5, firstSpace);
    String rest = cmd.substring(firstSpace + 1);
    rest.trim();

    int cmdSpace = rest.indexOf(' ');
    String command = "";
    String param = "";
    if (cmdSpace == -1) {
      command = rest;
    } else {
      command = rest.substring(0, cmdSpace);
      param = rest.substring(cmdSpace + 1);
      param.trim();
    }
    command.toLowerCase();

    // 1. Check command validity FIRST before checking hash or consuming challenge
    bool isValidCmd = (command.equals("list") || command.equals("add") || command.equals("delete") || command.equals("open"));
    if (!isValidCmd) {
      logMessage("Remote serial error: INVALID_COMMAND (" + command + ")");
      Serial.println("ERROR:INVALID_COMMAND");
      return;
    }

    // 2. Check active challenge
    if (activeChallenge.length() == 0) {
      logMessage("Remote serial error: INVALID_AUTH (No active challenge)");
      Serial.println("ERROR:INVALID_AUTH");
      return;
    }

    // 3. Compute expected hash using challenge + password + command + parameter
    String storedPwd = getStoredPassword();
    String combined = activeChallenge + storedPwd + command + param;

    SHA256 sha256;
    sha256.reset();
    sha256.update(combined.c_str(), combined.length());
    uint8_t hash[32];
    sha256.finalize(hash, sizeof(hash));

    String expectedHex = "";
    for (int i = 0; i < 32; i++) {
      char buf[3];
      sprintf(buf, "%02x", hash[i]);
      expectedHex += buf;
    }

    // 4. Verify hash
    if (!authPart.equalsIgnoreCase(expectedHex)) {
      logMessage("Remote serial error: INVALID_AUTH (Wrong hash provided)");
      Serial.println("ERROR:INVALID_AUTH");
      activeChallenge = ""; // Invalidate challenge on invalid attempt
      return;
    }

    // 5. Invalidate challenge immediately (single-use)
    activeChallenge = "";

    // 6. Execute requested command
    if (command.equals("list")) {
      logMessage("Remote serial command executed: list");
      serialExecuteList();
    } 
    else if (command.equals("add")) {
      logMessage("Remote serial command executed: add with param " + param);
      serialExecuteAdd(param);
    } 
    else if (command.equals("delete")) {
      logMessage("Remote serial command executed: delete with param " + param);
      serialExecuteDelete(param);
    } 
    else if (command.equals("open")) {
      logMessage("Remote serial command executed: open");
      Serial.println("SUCCESS:GATE_OPENING");
      executeGateOpenSequence("Remote Serial Command");
    }
  } 
  else {
    logMessage("Remote serial error: PARSING_ERROR");
    Serial.println("ERROR:PARSING_ERROR");
  }
}

void serialExecuteList() {
  byte count = EEPROM.read(cardCountAddress);
  if (count == 0) {
    Serial.println("CARDS:EMPTY");
  } else {
    Serial.print("CARDS:COUNT=");
    Serial.println(count);
    for (int c = 0; c < count; c++) {
      Serial.print("INDEX ");
      Serial.print(c);
      Serial.print(": ");
      int baseAddr = cardListStartAddress + (c * uidSize);
      for (byte i = 0; i < uidSize; i++) {
        byte val = EEPROM.read(baseAddr + i);
        if (val < 0x10) Serial.print("0");
        Serial.print(val, HEX);
        if (i < uidSize - 1) Serial.print(" ");
      }
      Serial.println();
    }
  }
}

void serialExecuteAdd(String param) {
  param.replace(" ", "");
  if (param.length() != 8) {
    logMessage("Remote serial add error: PARSING_ERROR (Invalid UID length)");
    Serial.println("ERROR:PARSING_ERROR");
    return;
  }

  byte newUID[4];
  for (int i = 0; i < 4; i++) {
    String byteStr = param.substring(i * 2, i * 2 + 2);
    newUID[i] = (byte)strtol(byteStr.c_str(), NULL, 16);
  }

  byte count = EEPROM.read(cardCountAddress);
  if (count >= maxCards) {
    logMessage("Remote serial add error: STORAGE_FULL");
    Serial.println("ERROR:STORAGE_FULL");
    return;
  }

  int writeAddress = cardListStartAddress + (count * uidSize);
  for (byte i = 0; i < uidSize; i++) {
    EEPROM.write(writeAddress + i, newUID[i]);
  }
  count++;
  EEPROM.write(cardCountAddress, count);
  
  String addedUIDStr = uidToString(newUID, 4);
  logMessage("Card added via remote serial | UID: " + addedUIDStr);
  Serial.println("SUCCESS:CARD_ADDED");
}

void serialExecuteDelete(String param) {
  if (param.length() == 0) {
    logMessage("Remote serial delete error: PARSING_ERROR");
    Serial.println("ERROR:PARSING_ERROR");
    return;
  }

  int index = param.toInt();
  byte count = EEPROM.read(cardCountAddress);
  if (index < 0 || index >= count) {
    logMessage("Remote serial delete error: INVALID_INDEX (" + String(index) + ")");
    Serial.println("ERROR:INVALID_INDEX");
    return;
  }

  int baseAddr = cardListStartAddress + (index * uidSize);
  byte deletedUIDBytes[4];
  for (byte b = 0; b < uidSize; b++) {
    deletedUIDBytes[b] = EEPROM.read(baseAddr + b);
  }
  String deletedUID = uidToString(deletedUIDBytes, uidSize);

  for (int i = index; i < count - 1; i++) {
    for (int b = 0; b < uidSize; b++) {
      byte nextByte = EEPROM.read(cardListStartAddress + (i + 1) * uidSize + b);
      EEPROM.write(cardListStartAddress + i * uidSize + b, nextByte);
    }
  }
  count--;
  EEPROM.write(cardCountAddress, count);
  logMessage("Card deleted via remote serial | Index: " + String(index) + " | UID: " + deletedUID);
  Serial.println("SUCCESS:CARD_DELETED");
}

void savePassword(String pwd) {
  byte len = pwd.length();
  if (len > 8) len = 8;
  EEPROM.write(pwdLenAddress, len);
  for (byte i = 0; i < len; i++) {
    EEPROM.write(pwdStartAddress + i, pwd[i]);
  }
}

String getStoredPassword() {
  byte len = EEPROM.read(pwdLenAddress);
  if (len == 255 || len > 8) return "1234"; 
  String pwd = "";
  for (byte i = 0; i < len; i++) {
    pwd += (char)EEPROM.read(pwdStartAddress + i);
  }
  return pwd;
}

bool checkCardUID() {
  if (mfrc522.uid.size != uidSize) return false;
  byte count = EEPROM.read(cardCountAddress);
  for (int c = 0; c < count; c++) {
    bool match = true;
    int baseAddr = cardListStartAddress + (c * uidSize);
    for (byte i = 0; i < uidSize; i++) {
      if (mfrc522.uid.uidByte[i] != EEPROM.read(baseAddr + i)) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

void showTemporaryMessage(String line1, String line2) {
  showingMessage = true;
  lcdMessageTimer = millis();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void updateOperatorLCD() {
  lcd.clear();
  if (opState == OP_IDLE) {
    lcd.setCursor(0, 0);
    lcd.print("[+]Reg [C]Lst"); 
    lcd.setCursor(0, 1);
    lcd.print("[D]Opr [E]Pwd"); 
  } else if (opState == OP_VIEW_CARDS) {
    byte count = EEPROM.read(cardCountAddress);
    if (count == 0) {
      lcd.setCursor(0, 0);
      lcd.print("Card List: Empty");
      lcd.setCursor(0, 1);
      lcd.print("[C] Exit");
    } else {
      lcd.setCursor(0, 0);
      lcd.print("Card ");
      lcd.print(currentViewIndex + 1);
      lcd.print("/");
      lcd.print(count);
      lcd.print(" [-]Del");
      
      lcd.setCursor(0, 1);
      int baseAddr = cardListStartAddress + (currentViewIndex * uidSize);
      for (byte i = 0; i < uidSize; i++) {
        byte val = EEPROM.read(baseAddr + i);
        if (val < 0x10) lcd.print("0");
        lcd.print(val, HEX);
        if (i < uidSize - 1) lcd.print(" ");
      }
    }
  } else if (opState == OP_REG_SCAN_CARD) {
    lcd.setCursor(0, 0);
    lcd.print("Scan New Card");
    lcd.setCursor(0, 1);
    lcd.print("[-] Cancel");
  }
}