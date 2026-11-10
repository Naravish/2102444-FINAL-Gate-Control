#include <SPI.h>
#include <MFRC522.h>
#include <Stepper.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>

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
  OP_CHANGE_NEW_PASSWORD_2
};
OpState opState = OP_IDLE;

// Timing variables
const unsigned long gateHoldOpenTime = 3000;    // 3 seconds
int targetSteps = 512; // 90 degrees rotation (2048 / 4)

// Card View & Messaging variables
int currentViewIndex = 0;
unsigned long lcdMessageTimer = 0;
bool showingMessage = false;

// ==========================================
// 7-SEGMENT DISPLAY MULTIPLEXING (TIMER1)
// ==========================================
volatile byte displayChars[4] = {0xFF, 0xFF, 0xFF, 0xFF};
volatile int currentDigit = 0;

// Character Segment Encoding for Common Anode (~0bGFEDCBA)
const byte SEG_S = ~0b01101101;
const byte SEG_T = ~0b01111000;
const byte SEG_O = ~0b00111111;
const byte SEG_P = ~0b01110011;
const byte SEG_E = ~0b01111001;
const byte SEG_N = ~0b00110111;
const byte SEG_BLANK = ~0b00000000;

void setupTimer1() {
  cli(); // stop interrupts
  TCCR1A = 0; 
  TCCR1B = 0; 
  // Set compare match register for 2ms interval (500Hz multiplexing rate)
  // 16MHz / (64 * 500) - 1 = 499
  OCR1A = 499; 
  TCCR1B |= (1 << WGM12); // turn on CTC mode
  TCCR1B |= (1 << CS11) | (1 << CS10); // set prescaler to 64
  TIMSK1 |= (1 << OCIE1A); // enable timer compare interrupt
  sei(); // allow interrupts
}

// Timer1 Interrupt Service Routine for multiplexing digits
ISR(TIMER1_COMPA_vect) {
  // Turn off all digits (Common Anode: LOW = Off)
  digitalWrite(A8, LOW);
  digitalWrite(A9, LOW);
  digitalWrite(A10, LOW);
  digitalWrite(A11, LOW);

  // Send segment pattern via 74HC595 shift register
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, displayChars[currentDigit]);
  digitalWrite(latchPin, HIGH);

  // Turn on current digit (Common Anode: HIGH = On)
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
// SETUP & MAIN LOOP
// ==========================================
void setup() {
  Serial.begin(9600);
  
  SPI.begin();          
  mfrc522.PCD_Init();   
  
  // Initialize LCD
  lcd.init();                      
  lcd.backlight();

  // Initialize 7-Segment Pins
  pinMode(latchPin, OUTPUT);
  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(A8, OUTPUT);
  pinMode(A9, OUTPUT);
  pinMode(A10, OUTPUT);
  pinMode(A11, OUTPUT);

  // Initialize Timer Interrupt for 7-Segment Display
  setupTimer1();
  
  myStepper.setSpeed(10); // Set motor speed in RPM

  // Initialize card count if uninitialized
  if (EEPROM.read(cardCountAddress) > maxCards) {
    EEPROM.write(cardCountAddress, 1);
    byte defaultUID[] = {0x3C, 0x79, 0x54, 0x83};
    for (int i = 0; i < uidSize; i++) {
      EEPROM.write(cardListStartAddress + i, defaultUID[i]);
    }
  }

  // Initialize or Reset Password based on flag
  byte currentPwdLen = EEPROM.read(pwdLenAddress);
  if (RESET_PASSWORD_FLAG || currentPwdLen == 255 || currentPwdLen > 8) {
    savePassword("1234");
    Serial.println("LOG: Admin password initialized/reset to '1234'.");
  }
  
  setDisplayWord('S', 'T', 'O', 'P'); // Default status
  updateOperatorLCD();
  Serial.println("System Initialized. Gate Controller Ready.");
}

void loop() {
  // 1. RFID Scanner check runs every loop
  handleRFIDScanner();

  // 2. Operator keypad & LCD interface runs concurrently
  handleOperatorUI();
}

void handleRFIDScanner() {
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    if (opState == OP_REG_SCAN_CARD) {
      byte count = EEPROM.read(cardCountAddress);
      if (count < maxCards) {
        int writeAddress = cardListStartAddress + (count * uidSize);
        for (byte i = 0; i < uidSize; i++) {
          EEPROM.write(writeAddress + i, mfrc522.uid.uidByte[i]);
        }
        count++;
        EEPROM.write(cardCountAddress, count);
        Serial.println("LOG: New RFID card registered and saved to EEPROM.");
        showTemporaryMessage("Card Added!", "Total Cards: " + String(count));
      } else {
        showTemporaryMessage("Storage Full!", "Max 16 Cards");
      }
      opState = OP_IDLE;
    } 
    else {
      if (checkCardUID()) {
        Serial.println("LOG: Access Granted. Valid RFID card detected.");
        showTemporaryMessage("Access Granted", "Opening Gate...");
        
        // Switch display to OPEN
        setDisplayWord('O', 'P', 'E', 'N');

        // Blocking gate operation sequence (7-segment continues refreshing via ISR)
        Serial.println("LOG: Gate is opening (90 degrees)...");
        myStepper.step(targetSteps);
        Serial.println("LOG: Gate is fully OPEN.");
        
        delay(gateHoldOpenTime); 
        
        Serial.println("LOG: Closing gate...");
        myStepper.step(-targetSteps);
        Serial.println("LOG: Gate is fully CLOSED.");

        // Switch display back to STOP
        setDisplayWord('S', 'T', 'O', 'P');
      } else {
        Serial.println("LOG: Access Denied. Unknown RFID card scanned.");
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
        currentViewIndex = 0;
        opState = OP_VIEW_CARDS;
        updateOperatorLCD();
      } else if (customKey == 'D') {
        enteredPassword = "";
        opState = OP_CHANGE_OLD_PASSWORD;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Old Password:");
        lcd.setCursor(0, 1);
        lcd.print("_[+]Confirm");
      }
      break;

    case OP_VIEW_CARDS:
      if (customKey == 'C') {
        opState = OP_IDLE;
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

    case OP_REG_PASSWORD:
      if (customKey == '-') {
        Serial.println("LOG: Registration password entry cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          Serial.println("LOG: Admin password verified for registration.");
          showTemporaryMessage("Pass Correct!", "Scan New Card");
          opState = OP_REG_SCAN_CARD;
        } else {
          Serial.println("LOG: Incorrect admin password entered.");
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
        Serial.println("LOG: Card scan registration cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      }
      break;

    case OP_DELETE_PASSWORD:
      if (customKey == '-') {
        Serial.println("LOG: Card deletion password entry cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_VIEW_CARDS;
        updateOperatorLCD();
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          byte count = EEPROM.read(cardCountAddress);
          for (int i = currentViewIndex; i < count - 1; i++) {
            for (int b = 0; b < uidSize; b++) {
              byte nextByte = EEPROM.read(cardListStartAddress + (i + 1) * uidSize + b);
              EEPROM.write(cardListStartAddress + i * uidSize + b, nextByte);
            }
          }
          count--;
          EEPROM.write(cardCountAddress, count);
          Serial.println("LOG: Card successfully deleted from EEPROM.");
          showTemporaryMessage("Card Deleted!", "");
          currentViewIndex = 0;
          opState = OP_VIEW_CARDS;
          updateOperatorLCD();
        } else {
          Serial.println("LOG: Incorrect password for card deletion.");
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

    case OP_CHANGE_OLD_PASSWORD:
      if (customKey == '-') {
        Serial.println("LOG: Password change cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == getStoredPassword()) {
          enteredPassword = "";
          opState = OP_CHANGE_NEW_PASSWORD_1;
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("New Password:");
          lcd.setCursor(0, 1);
          lcd.print("_[+]Confirm");
        } else {
          Serial.println("LOG: Incorrect old password entered.");
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

    case OP_CHANGE_NEW_PASSWORD_1:
      if (customKey == '-') {
        Serial.println("LOG: Password change cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword.length() > 0) {
          newPasswordTemp = enteredPassword;
          enteredPassword = "";
          opState = OP_CHANGE_NEW_PASSWORD_2;
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Confirm Password:");
          lcd.setCursor(0, 1);
          lcd.print("_[+]Confirm");
        } else {
          showTemporaryMessage("Invalid", "Empty Password");
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

    case OP_CHANGE_NEW_PASSWORD_2:
      if (customKey == '-') {
        Serial.println("LOG: Password change cancelled.");
        showTemporaryMessage("Operation", "Cancelled");
        opState = OP_IDLE;
      } else if (customKey == '+') {
        if (enteredPassword == newPasswordTemp) {
          savePassword(enteredPassword);
          Serial.println("LOG: Password successfully changed.");
          showTemporaryMessage("Password Changed", "Successfully");
          opState = OP_IDLE;
        } else {
          Serial.println("LOG: Passwords did not match.");
          showTemporaryMessage("Mismatch", "Failed to Change");
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

    default:
      break;
  }
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
    lcd.print("[D]Pwd       "); 
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