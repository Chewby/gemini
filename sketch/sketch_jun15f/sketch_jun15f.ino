#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>

LCD_I2C lcd(0x27, 16, 4);
RTC_DS3231 rtc;

// === CONFIGURATION ===
const byte LED_PIN = 8;
const byte BUTTON_PINS[] = {A5, A4, A3, A2, A1, A0};

// === PINS RELAIS (ATmega32U4) ===
// PD6 = Arduino D12  |  PD7 = Arduino D6
const byte RELAY1_PIN = 12; // PD6
const byte RELAY2_PIN = 6;  // PD7

// Variables temporelles
unsigned long alarmStartTime, lastButtonActivity, lastDebounceTime[6], previousClockMillis;
const unsigned int BACKLIGHT_TIMEOUT = 30000, DEBOUNCE_DELAY = 50, CLOCK_UPDATE_INTERVAL = 1000, ALARM_DURATION = 1000;

// Variables d'état
struct {
  bool lastButtonState[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
  bool currentButtonState[6];
  bool alarmActive : 1;
  bool timerRunning : 1;
  bool timerFinished : 1;
  bool alarmEnabled : 1;
  bool alarmTriggered : 1;
  bool lcdBacklightOn : 1;
  bool lcdBacklightActive : 1;
  bool timerScreenInit : 1;
  bool alarmScreenInit : 1;
  bool mainMenuInit : 1;
  bool serialWasConnected : 1;
} state;

// EEPROM addresses
enum { EEPROM_MINUTES = 0, EEPROM_SECONDS = 4, EEPROM_MODE = 8, EEPROM_ALARM_YEAR = 12, 
       EEPROM_ALARM_MONTH = 16, EEPROM_ALARM_DAY = 20, EEPROM_ALARM_HOUR = 24, 
       EEPROM_ALARM_MINUTE = 28, EEPROM_BACKLIGHT = 32, EEPROM_COMPILE_TIME = 36 };

// Modes et menu
enum TimerMode : byte { MODE_SIMPLE, MODE_REPEAT, MODE_ALARM, MODE_ALARM_DAILY };
enum MenuState : byte { MENU_MAIN, MENU_SET_TIME, MENU_SET_ALARM, MENU_SET_DATETIME, TIMER_RUNNING, TIMER_FINISHED, ALARM_WAITING, MENU_TEST_RELAY };

TimerMode currentTimerMode = MODE_SIMPLE;
MenuState currentMenu = MENU_MAIN;
byte menuSelection = 0;
byte timeSetSelection = 0;
byte alarmSetSelection = 0;
byte dateTimeSetSelection = 0;
byte cycleCount = 0;

// Sélection pour le mode test relais
byte relayTestSelection = 0; // 0 = RELAY1 (PD6/D12), 1 = RELAY2 (PD7/D6)

// Variables timer/alarme
int timerMinutes = 5, timerSeconds = 0;
int alarmYear = 2025, alarmMonth = 6, alarmDay = 15, alarmHour = 12, alarmMinute = 0;
int rtcYear = 2025, rtcMonth = 6, rtcDay = 15, rtcHour = 12, rtcMinute = 0, rtcSecond = 0;
DateTime timerStartTime;

// Protection contre déclenchements multiples alarme daily
bool dailyAlarmTriggeredToday = false;
int lastCheckedDay = -1;

// Buffer série
#define BUFFER_SIZE 128
char serialBuffer[BUFFER_SIZE];
byte bufferIndex = 0;
bool bufferFull = false;

// === FONCTIONS UTILITAIRES ===
void smartPrint(const __FlashStringHelper* msg) {
  if (Serial) Serial.print(msg);
  else {
    const char* p = (const char*)msg;
    while (char c = pgm_read_byte(p++)) {
      serialBuffer[bufferIndex++] = c;
      if (bufferIndex >= BUFFER_SIZE) { bufferIndex = 0; bufferFull = true; }
    }
  }
}

void smartPrint(const String& msg) {
  if (Serial) Serial.print(msg);
  else {
    int len = min(msg.length(), BUFFER_SIZE - bufferIndex);
    msg.substring(0, len).toCharArray(serialBuffer + bufferIndex, len + 1);
    bufferIndex += len;
    if (bufferIndex >= BUFFER_SIZE) { bufferIndex = 0; bufferFull = true; }
  }
}

void smartPrintln(const __FlashStringHelper* msg) { smartPrint(msg); smartPrint(F("\n")); }
void smartPrintln(const String& msg) { smartPrint(msg); smartPrint(F("\n")); }

void flushBuffer() {
  if (bufferIndex > 0 || bufferFull) {
    Serial.println(F("=== BUFFER ==="));
    if (bufferFull) {
      for (int i = bufferIndex; i < BUFFER_SIZE; i++) Serial.print(serialBuffer[i]);
    }
    for (int i = 0; i < bufferIndex; i++) Serial.print(serialBuffer[i]);
    Serial.println(F("=== FIN ==="));
    bufferIndex = 0; bufferFull = false;
  }
}

void updateBacklight() {
  if (state.lcdBacklightOn && state.lcdBacklightActive && 
      (millis() - lastButtonActivity >= BACKLIGHT_TIMEOUT)) {
    state.lcdBacklightActive = false;
    lcd.noBacklight();
    smartPrintln(F("Rétroéclairage éteint (30s)"));
  } else if (!state.lcdBacklightOn && state.lcdBacklightActive) {
    state.lcdBacklightActive = false;
    lcd.noBacklight();
  }
}

void activateBacklight() {
  lastButtonActivity = millis();
  if (state.lcdBacklightOn && !state.lcdBacklightActive) {
    state.lcdBacklightActive = true;
    lcd.backlight();
    smartPrintln(F("Rétroéclairage réactivé"));
  }
}

// === EEPROM ===
void loadSettings() {
  EEPROM.get(EEPROM_MINUTES, timerMinutes);
  EEPROM.get(EEPROM_SECONDS, timerSeconds);
  EEPROM.get(EEPROM_MODE, currentTimerMode);
  EEPROM.get(EEPROM_ALARM_YEAR, alarmYear);
  EEPROM.get(EEPROM_ALARM_MONTH, alarmMonth);
  EEPROM.get(EEPROM_ALARM_DAY, alarmDay);
  EEPROM.get(EEPROM_ALARM_HOUR, alarmHour);
  EEPROM.get(EEPROM_ALARM_MINUTE, alarmMinute);
  
  byte backlight;
  EEPROM.get(EEPROM_BACKLIGHT, backlight);
  state.lcdBacklightOn = (backlight <= 1) ? (backlight == 1) : true;
  
  // Validation
  timerMinutes = constrain(timerMinutes, 0, 99);
  timerSeconds = constrain(timerSeconds, 0, 59);
  currentTimerMode = (TimerMode)constrain(currentTimerMode, 0, 3);
  alarmYear = constrain(alarmYear, 2025, 2030);
  alarmMonth = constrain(alarmMonth, 1, 12);
  alarmDay = constrain(alarmDay, 1, 31);
  alarmHour = constrain(alarmHour, 0, 23);
  alarmMinute = constrain(alarmMinute, 0, 59);
  
  state.lcdBacklightActive = state.lcdBacklightOn;
  lastButtonActivity = millis();
  
  if (state.lcdBacklightActive) lcd.backlight();
  else lcd.noBacklight();
  
  smartPrint(F("Timer: ")); smartPrint(String(timerMinutes) + ":" + String(timerSeconds));
  smartPrint(F(" Mode: ")); 
  const __FlashStringHelper* modes[] = {F("Simple"), F("Repeat"), F("Alarm"), F("Daily")};
  smartPrintln(modes[currentTimerMode]);
}

void saveSettings() {
  EEPROM.put(EEPROM_MINUTES, timerMinutes);
  EEPROM.put(EEPROM_SECONDS, timerSeconds);
  EEPROM.put(EEPROM_MODE, currentTimerMode);
  EEPROM.put(EEPROM_ALARM_YEAR, alarmYear);
  EEPROM.put(EEPROM_ALARM_MONTH, alarmMonth);
  EEPROM.put(EEPROM_ALARM_DAY, alarmDay);
  EEPROM.put(EEPROM_ALARM_HOUR, alarmHour);
  EEPROM.put(EEPROM_ALARM_MINUTE, alarmMinute);
  EEPROM.put(EEPROM_BACKLIGHT, (byte)state.lcdBacklightOn);
  smartPrintln(F("Settings saved"));
}

// === AFFICHAGE ===
void printTime(int h, int m, int s = -1) {
  if (h < 10) lcd.print("0"); lcd.print(h); lcd.print(":");
  if (m < 10) lcd.print("0"); lcd.print(m);
  if (s >= 0) { lcd.print(":"); if (s < 10) lcd.print("0"); lcd.print(s); }
}

void printDate(int d, int m, int y = -1, bool shortYear = false) {
  if (d < 10) lcd.print("0"); lcd.print(d); lcd.print("/");
  if (m < 10) lcd.print("0"); lcd.print(m);
  if (y >= 0) { 
    lcd.print("/"); 
    if (shortYear) {
      int shortY = y % 100;
      if (shortY < 10) lcd.print("0"); // Ajouter 0 devant si nécessaire
      lcd.print(shortY);
    } else {
      lcd.print(y);
    }
  }
}

void displayMainMenu() {
  DateTime now = rtc.now();
  
  if (!state.mainMenuInit) {
    lcd.clear();

    // Menu à 6 options avec navigation
    const char* items[] = {"Mode: ",
                          (currentTimerMode == MODE_ALARM || currentTimerMode == MODE_ALARM_DAILY) ? "Regler Alarme" : "Regler Temps",
                          (currentTimerMode == MODE_ALARM || currentTimerMode == MODE_ALARM_DAILY) ?
                            (currentTimerMode == MODE_ALARM ? "Start Alarme" : "Start Daily") : "Start",
                          "Regler Heure", "Retro: ", "Test Relais"};

    byte start = constrain(menuSelection - 1, 0, 3);
    for (byte i = 0; i < 3; i++) {
      byte idx = start + i;
      if (idx < 6) {
        lcd.setCursor(0, i + 1);
        lcd.print(menuSelection == idx ? "> " : "  ");
        lcd.print(items[idx]);

        if (idx == 0) {
          const char* modes[] = {"Simple", "Repeat", "Alarme", "Daily"};
          lcd.print(modes[currentTimerMode]);
        } else if (idx == 2 && currentTimerMode != MODE_ALARM && currentTimerMode != MODE_ALARM_DAILY) {
          lcd.print(" ("); lcd.print(timerMinutes); lcd.print(":");
          if (timerSeconds < 10) lcd.print("0"); lcd.print(timerSeconds); lcd.print(")");
        } else if (idx == 4) {
          lcd.print(state.lcdBacklightOn ? "ON" : "OFF");
          if (state.lcdBacklightOn && !state.lcdBacklightActive) lcd.print("*");
        }
      }
    }
    state.mainMenuInit = true;
  }
  
  // Affichage de l'heure AVEC L'ANNÉE sur 2 chiffres
  lcd.setCursor(0, 0);
  // Effacer la ligne complètement
  lcd.print("                "); // 16 espaces pour effacer
  lcd.setCursor(0, 0);
  lcd.print("T:");
  printDate(now.day(), now.month(), now.year(), true); // true = année courte
  lcd.print(" ");
  printTime(now.hour(), now.minute(), now.second());
}

void displaySetTimeMenu() {
  lcd.clear();
  lcd.print(F("REGLAGE TEMPS"));
  lcd.setCursor(0, 1);
  lcd.print(F("Minutes: "));
  if (timeSetSelection == 0) lcd.print(">");
  lcd.print(timerMinutes);
  if (timeSetSelection == 0) lcd.print("<");
  
  lcd.setCursor(0, 2);
  lcd.print(F("Secondes: "));
  if (timeSetSelection == 1) lcd.print(">");
  lcd.print(timerSeconds);
  if (timeSetSelection == 1) lcd.print("<");
  
  lcd.setCursor(0, 3);
  lcd.print(F("ENTER=OK RET=Ann"));
}

void displaySetAlarmMenu() {
  lcd.clear();
  if (currentTimerMode == MODE_ALARM) {
    lcd.print(F("REGLAGE ALARME"));
  } else {
    lcd.print(F("REGLAGE DAILY"));
  }
  
  if (currentTimerMode == MODE_ALARM) {
    // Mode alarme unique - afficher la date
    lcd.setCursor(0, 1);
    if (alarmSetSelection == 0) lcd.print(">");
    if (alarmDay < 10) lcd.print("0");
    lcd.print(alarmDay);
    if (alarmSetSelection == 0) lcd.print("<");
    lcd.print("/");
    if (alarmSetSelection == 1) lcd.print(">");
    if (alarmMonth < 10) lcd.print("0");
    lcd.print(alarmMonth);
    if (alarmSetSelection == 1) lcd.print("<");
    lcd.print("/");
    if (alarmSetSelection == 2) lcd.print(">");
    lcd.print(alarmYear);
    if (alarmSetSelection == 2) lcd.print("<");
    
    lcd.setCursor(0, 2);
    lcd.print(F("Heure: "));
    if (alarmSetSelection == 3) lcd.print(">");
    if (alarmHour < 10) lcd.print("0");
    lcd.print(alarmHour);
    if (alarmSetSelection == 3) lcd.print("<");
    lcd.print(":");
    if (alarmSetSelection == 4) lcd.print(">");
    if (alarmMinute < 10) lcd.print("0");
    lcd.print(alarmMinute);
    if (alarmSetSelection == 4) lcd.print("<");
  } else {
    // Mode alarme daily - afficher seulement l'heure
    lcd.setCursor(0, 1);
    lcd.print(F("Tous les jours:"));
    
    lcd.setCursor(0, 2);
    if (alarmSetSelection == 0) lcd.print(">");
    if (alarmHour < 10) lcd.print("0");
    lcd.print(alarmHour);
    if (alarmSetSelection == 0) lcd.print("<");
    lcd.print(":");
    if (alarmSetSelection == 1) lcd.print(">");
    if (alarmMinute < 10) lcd.print("0");
    lcd.print(alarmMinute);
    if (alarmSetSelection == 1) lcd.print("<");
  }
  
  lcd.setCursor(0, 3);
  lcd.print(F("ENTER=OK RET=Ann"));
}

void displaySetDateTimeMenu() {
  lcd.clear();
  lcd.print(F("REGLAGE HEURE"));
  
  lcd.setCursor(0, 1);
  // Date: jour/mois/année
  if (dateTimeSetSelection == 0) lcd.print(">");
  if (rtcDay < 10) lcd.print("0");
  lcd.print(rtcDay);
  if (dateTimeSetSelection == 0) lcd.print("<");
  lcd.print("/");
  if (dateTimeSetSelection == 1) lcd.print(">");
  if (rtcMonth < 10) lcd.print("0");
  lcd.print(rtcMonth);
  if (dateTimeSetSelection == 1) lcd.print("<");
  lcd.print("/");
  if (dateTimeSetSelection == 2) lcd.print(">");
  lcd.print(rtcYear);
  if (dateTimeSetSelection == 2) lcd.print("<");
  
  lcd.setCursor(0, 2);
  // Heure: heure:minute:seconde
  if (dateTimeSetSelection == 3) lcd.print(">");
  if (rtcHour < 10) lcd.print("0");
  lcd.print(rtcHour);
  if (dateTimeSetSelection == 3) lcd.print("<");
  lcd.print(":");
  if (dateTimeSetSelection == 4) lcd.print(">");
  if (rtcMinute < 10) lcd.print("0");
  lcd.print(rtcMinute);
  if (dateTimeSetSelection == 4) lcd.print("<");
  lcd.print(":");
  if (dateTimeSetSelection == 5) lcd.print(">");
  if (rtcSecond < 10) lcd.print("0");
  lcd.print(rtcSecond);
  if (dateTimeSetSelection == 5) lcd.print("<");
  
  lcd.setCursor(0, 3);
  lcd.print(F("ENTER=OK RET=Ann"));
}

void displayTimerRunning() {
  DateTime now = rtc.now();
  long elapsed = now.unixtime() - timerStartTime.unixtime();
  long total = timerMinutes * 60 + timerSeconds;
  long remaining = max(0L, total - elapsed);
  
  if (elapsed >= total) {
    if (currentTimerMode == MODE_REPEAT) {
      cycleCount++; timerStartTime = rtc.now(); state.alarmActive = true;
      alarmStartTime = millis();
      smartPrint(F("CYCLE ")); smartPrint(String(cycleCount)); smartPrintln(F(" FINI"));
      return;
    } else {
      state.timerRunning = false; state.timerFinished = true; state.alarmActive = true;
      alarmStartTime = millis(); currentMenu = TIMER_FINISHED; state.timerScreenInit = false;
      smartPrintln(F("TIMER FINI")); displayTimerFinished(); return;
    }
  }
  
  if (!state.timerScreenInit) {
    lcd.clear();
    lcd.print(currentTimerMode == MODE_SIMPLE ? F("TIMER SIMPLE") : F("TIMER REPEAT #"));
    if (currentTimerMode == MODE_REPEAT) lcd.print(cycleCount + 1);
    lcd.setCursor(0, 1); lcd.print(F("Temps restant:"));
    lcd.setCursor(0, 3); lcd.print(F("RETURN = Arreter"));
    state.timerScreenInit = true;
  }
  
  lcd.setCursor(0, 2); lcd.print(F("    ")); printTime(remaining / 60, remaining % 60); lcd.print(F("    "));
  if (currentTimerMode == MODE_REPEAT) { lcd.setCursor(14, 0); lcd.print(cycleCount + 1); }
}

void displayTimerFinished() {
  lcd.clear(); lcd.print(F("*** FINI ***"));
  lcd.setCursor(0, 1); lcd.print(F("Timer termine"));
  lcd.setCursor(0, 2); lcd.print(F("Relais 1 active"));
  lcd.setCursor(0, 3); lcd.print(F("RETURN = Retour"));
}

void displayAlarmWaiting() {
  DateTime now = rtc.now();
  
  if (!state.alarmScreenInit) {
    lcd.clear(); 
    if (currentTimerMode == MODE_ALARM) {
      lcd.print(F("ALARME ACTIVE"));
    } else {
      lcd.print(F("ALARME DAILY"));
    }
    lcd.setCursor(0, 3); lcd.print(F("RETURN = Arreter"));
    state.alarmScreenInit = true;
  }
  
  lcd.setCursor(0, 1); 
  lcd.print("C:");
  if (currentTimerMode == MODE_ALARM) {
    printDate(alarmDay, alarmMonth, alarmYear, true); // Année courte
    lcd.print(" ");
  }
  printTime(alarmHour, alarmMinute, 0);
  
  lcd.setCursor(0, 2); 
  lcd.print("T:");
  if (currentTimerMode == MODE_ALARM) {
    printDate(now.day(), now.month(), now.year(), true); // Année courte
    lcd.print(" ");
  }
  printTime(now.hour(), now.minute(), now.second());
}

// === AFFICHAGE : MENU TEST RELAIS ===
void displayRelayTestMenu() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("TEST RELAIS"));
  lcd.setCursor(0, 1);
  lcd.print(relayTestSelection == 0 ? F("> Relais 1  PD6 D12") : F("  Relais 1  PD6 D12"));
  lcd.setCursor(0, 2);
  lcd.print(relayTestSelection == 1 ? F("> Relais 2  PD7 D6 ") : F("  Relais 2  PD7 D6 "));
  lcd.setCursor(0, 3);
  lcd.print(F("ENTER=Act  RET=Menu"));
}

// === ACTIONS TIMER/ALARME ===
void startTimer() {
  timerStartTime = rtc.now(); state.timerRunning = true; state.timerFinished = false;
  state.timerScreenInit = false; cycleCount = 0; currentMenu = TIMER_RUNNING;
  smartPrint(F("Timer démarré: ")); smartPrint(String(timerMinutes) + ":" + String(timerSeconds));
  smartPrintln(currentTimerMode == MODE_SIMPLE ? F(" Simple") : F(" Repeat"));
}

void stopTimer() {
  state.timerRunning = false; state.timerFinished = false; state.alarmActive = false;
  state.timerScreenInit = false; cycleCount = 0; currentMenu = MENU_MAIN;
  state.mainMenuInit = false; smartPrintln(F("Timer arrêté"));
}

void startAlarm() {
  DateTime now = rtc.now();
  
  if (currentTimerMode == MODE_ALARM) {
    DateTime alarmTime(alarmYear, alarmMonth, alarmDay, alarmHour, alarmMinute, 0);
    
    if (alarmTime.unixtime() <= now.unixtime()) {
      lcd.clear(); lcd.print(F("ERREUR ALARME"));
      lcd.setCursor(0, 1); lcd.print(F("Date/heure passee"));
      lcd.setCursor(0, 2); lcd.print(F("Regler a nouveau"));
      lcd.setCursor(0, 3); lcd.print(F("RETURN = Retour"));
      smartPrintln(F("ERREUR: Alarme dans le passé"));
      delay(3000); currentMenu = MENU_MAIN; state.mainMenuInit = false; 
      lcd.clear(); displayMainMenu(); return;
    }
    
    smartPrint(F("Alarme programmée: ")); smartPrint(String(alarmDay) + "/" + String(alarmMonth) + "/" + String(alarmYear));
    smartPrint(F(" à ")); smartPrintln(String(alarmHour) + ":" + String(alarmMinute));
  } else {
    // Alarme daily
    smartPrint(F("Alarme quotidienne: ")); smartPrintln(String(alarmHour) + ":" + String(alarmMinute));
    // Reset protection daily
    dailyAlarmTriggeredToday = false;
    lastCheckedDay = now.day();
  }
  
  state.alarmEnabled = true; state.alarmTriggered = false; state.alarmScreenInit = false;
  currentMenu = ALARM_WAITING;
}

void stopAlarm() {
  state.alarmEnabled = false; state.alarmTriggered = false; state.alarmActive = false;
  state.alarmScreenInit = false; currentMenu = MENU_MAIN; state.mainMenuInit = false;
  smartPrintln(F("Alarme arrêtée"));
}

// === NOUVELLE FONCTION DE VÉRIFICATION ALARME ===
void checkAlarmTrigger() {
  if (!state.alarmEnabled || state.alarmTriggered || currentMenu != ALARM_WAITING) {
    return;
  }
  
  DateTime now = rtc.now();
  
  // Reset protection daily à chaque nouveau jour
  if (currentTimerMode == MODE_ALARM_DAILY && lastCheckedDay != now.day()) {
    dailyAlarmTriggeredToday = false;
    lastCheckedDay = now.day();
    smartPrintln(F("Nouveau jour - alarme daily réarmée"));
  }
  
  bool shouldTrigger = false;
  
  if (currentTimerMode == MODE_ALARM) {
    // Alarme unique - vérifier date complète + seconde exacte
    shouldTrigger = (now.year() == alarmYear && now.month() == alarmMonth && 
                    now.day() == alarmDay && now.hour() == alarmHour && 
                    now.minute() == alarmMinute && now.second() == 0);
  } else if (currentTimerMode == MODE_ALARM_DAILY) {
    // Alarme quotidienne - vérifier heure/minute + EXACTEMENT à la seconde 0 + pas déjà déclenchée
    shouldTrigger = (now.hour() == alarmHour && now.minute() == alarmMinute && 
                    now.second() == 0 && !dailyAlarmTriggeredToday);
  }
  
  if (shouldTrigger) {
    state.alarmTriggered = true; state.alarmActive = true; alarmStartTime = millis();
    smartPrintln(F("*** ALARME DECLENCHEE ! ***"));
    
    if (currentTimerMode == MODE_ALARM) {
      // Alarme unique - afficher écran alarme
      state.alarmScreenInit = false;
      lcd.clear(); lcd.print(F("*** ALARME ! ***"));
      lcd.setCursor(0, 1); lcd.print(F("Relais 1 active"));
      lcd.setCursor(0, 3); lcd.print(F("RETURN = Arreter"));
    } else {
      // Alarme daily - marquer comme déclenchée aujourd'hui
      dailyAlarmTriggeredToday = true;
    }
  }
}

// === HANDLERS BOUTONS ===
void handleMainMenuButton(int buttonIndex) {
  switch (buttonIndex) {
    case 0: // ENTER
      switch (menuSelection) {
        case 0: // Mode
          currentTimerMode = (TimerMode)((currentTimerMode + 1) % 4);
          saveSettings(); state.mainMenuInit = false; displayMainMenu();
          break;
        case 1: // Config
          if (currentTimerMode == MODE_ALARM || currentTimerMode == MODE_ALARM_DAILY) {
            currentMenu = MENU_SET_ALARM; alarmSetSelection = 0; displaySetAlarmMenu();
          } else {
            currentMenu = MENU_SET_TIME; timeSetSelection = 0; displaySetTimeMenu();
          }
          break;
        case 2: (currentTimerMode == MODE_ALARM || currentTimerMode == MODE_ALARM_DAILY) ? startAlarm() : startTimer(); break;
        case 3: // Régler heure
          {
            DateTime now = rtc.now();
            rtcYear = now.year(); rtcMonth = now.month(); rtcDay = now.day();
            rtcHour = now.hour(); rtcMinute = now.minute(); rtcSecond = now.second();
            currentMenu = MENU_SET_DATETIME; dateTimeSetSelection = 0; displaySetDateTimeMenu();
          }
          break;
        case 4: // Backlight
          state.lcdBacklightOn = !state.lcdBacklightOn;
          if (state.lcdBacklightOn) { state.lcdBacklightActive = true; lcd.backlight(); }
          else { state.lcdBacklightActive = false; lcd.noBacklight(); }
          lastButtonActivity = millis(); saveSettings(); state.mainMenuInit = false; displayMainMenu();
          break;
        case 5: // Test Relais
          currentMenu = MENU_TEST_RELAY;
          relayTestSelection = 0;
          displayRelayTestMenu();
          break;
      }
      break;
    case 2: menuSelection = (menuSelection == 0) ? 5 : menuSelection - 1; state.mainMenuInit = false; displayMainMenu(); break;
    case 4: menuSelection = (menuSelection == 5) ? 0 : menuSelection + 1; state.mainMenuInit = false; displayMainMenu(); break;
  }
}

void handleSetTimeButton(int buttonIndex) {
  activateBacklight();
  
  switch (buttonIndex) {
    case 0: saveSettings(); currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu(); break;
    case 1: loadSettings(); currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu(); break;
    case 2: 
      if (timeSetSelection == 0) timerMinutes = (timerMinutes >= 99) ? 0 : timerMinutes + 1;
      else timerSeconds = (timerSeconds >= 59) ? 0 : timerSeconds + 1;
      displaySetTimeMenu(); break;
    case 3: timeSetSelection = (timeSetSelection >= 1) ? 0 : timeSetSelection + 1; displaySetTimeMenu(); break;
    case 4: 
      if (timeSetSelection == 0) timerMinutes = (timerMinutes <= 0) ? 99 : timerMinutes - 1;
      else timerSeconds = (timerSeconds <= 0) ? 59 : timerSeconds - 1;
      displaySetTimeMenu(); break;
    case 5: timeSetSelection = (timeSetSelection <= 0) ? 1 : timeSetSelection - 1; displaySetTimeMenu(); break;
  }
}

void handleSetAlarmButton(int buttonIndex) {
  activateBacklight();
  
  switch (buttonIndex) {
    case 0: saveSettings(); currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu(); break;
    case 1: loadSettings(); currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu(); break;
    case 2: // UP
      if (currentTimerMode == MODE_ALARM) {
        switch (alarmSetSelection) {
          case 0: alarmDay = (alarmDay >= 31) ? 1 : alarmDay + 1; break;
          case 1: alarmMonth = (alarmMonth >= 12) ? 1 : alarmMonth + 1; break;
          case 2: alarmYear = (alarmYear >= 2030) ? 2025 : alarmYear + 1; break;
          case 3: alarmHour = (alarmHour >= 23) ? 0 : alarmHour + 1; break;
          case 4: alarmMinute = (alarmMinute >= 59) ? 0 : alarmMinute + 1; break;
        }
      } else {
        switch (alarmSetSelection) {
          case 0: alarmHour = (alarmHour >= 23) ? 0 : alarmHour + 1; break;
          case 1: alarmMinute = (alarmMinute >= 59) ? 0 : alarmMinute + 1; break;
        }
      }
      displaySetAlarmMenu(); break;
    case 3: // RIGHT
      if (currentTimerMode == MODE_ALARM) alarmSetSelection = (alarmSetSelection >= 4) ? 0 : alarmSetSelection + 1;
      else alarmSetSelection = (alarmSetSelection >= 1) ? 0 : alarmSetSelection + 1;
      displaySetAlarmMenu(); break;
    case 4: // DOWN
      if (currentTimerMode == MODE_ALARM) {
        switch (alarmSetSelection) {
          case 0: alarmDay = (alarmDay <= 1) ? 31 : alarmDay - 1; break;
          case 1: alarmMonth = (alarmMonth <= 1) ? 12 : alarmMonth - 1; break;
          case 2: alarmYear = (alarmYear <= 2025) ? 2030 : alarmYear - 1; break;
          case 3: alarmHour = (alarmHour <= 0) ? 23 : alarmHour - 1; break;
          case 4: alarmMinute = (alarmMinute <= 0) ? 59 : alarmMinute - 1; break;
        }
      } else {
        switch (alarmSetSelection) {
          case 0: alarmHour = (alarmHour <= 0) ? 23 : alarmHour - 1; break;
          case 1: alarmMinute = (alarmMinute <= 0) ? 59 : alarmMinute - 1; break;
        }
      }
      displaySetAlarmMenu(); break;
    case 5: // LEFT
      if (currentTimerMode == MODE_ALARM) alarmSetSelection = (alarmSetSelection <= 0) ? 4 : alarmSetSelection - 1;
      else alarmSetSelection = (alarmSetSelection <= 0) ? 1 : alarmSetSelection - 1;
      displaySetAlarmMenu(); break;
  }
}

void handleSetDateTimeButton(int buttonIndex) {
  activateBacklight();
  
  switch (buttonIndex) {
    case 0: // ENTER
      rtc.adjust(DateTime(rtcYear, rtcMonth, rtcDay, rtcHour, rtcMinute, rtcSecond));
      smartPrintln(F("RTC mis à jour !"));
      currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu();
      break;
    case 1: // RETURN
      currentMenu = MENU_MAIN; state.mainMenuInit = false; lcd.clear(); displayMainMenu();
      break;
    case 2: // UP
      switch (dateTimeSetSelection) {
        case 0: rtcDay = (rtcDay >= 31) ? 1 : rtcDay + 1; break;
        case 1: rtcMonth = (rtcMonth >= 12) ? 1 : rtcMonth + 1; break;
        case 2: rtcYear = (rtcYear >= 2030) ? 2025 : rtcYear + 1; break;
        case 3: rtcHour = (rtcHour >= 23) ? 0 : rtcHour + 1; break;
        case 4: rtcMinute = (rtcMinute >= 59) ? 0 : rtcMinute + 1; break;
        case 5: rtcSecond = (rtcSecond >= 59) ? 0 : rtcSecond + 1; break;
      }
      displaySetDateTimeMenu(); break;
    case 3: // RIGHT
      dateTimeSetSelection = (dateTimeSetSelection >= 5) ? 0 : dateTimeSetSelection + 1;
      displaySetDateTimeMenu(); break;
    case 4: // DOWN
      switch (dateTimeSetSelection) {
        case 0: rtcDay = (rtcDay <= 1) ? 31 : rtcDay - 1; break;
        case 1: rtcMonth = (rtcMonth <= 1) ? 12 : rtcMonth - 1; break;
        case 2: rtcYear = (rtcYear <= 2025) ? 2030 : rtcYear - 1; break;
        case 3: rtcHour = (rtcHour <= 0) ? 23 : rtcHour - 1; break;
        case 4: rtcMinute = (rtcMinute <= 0) ? 59 : rtcMinute - 1; break;
        case 5: rtcSecond = (rtcSecond <= 0) ? 59 : rtcSecond - 1; break;
      }
      displaySetDateTimeMenu(); break;
    case 5: // LEFT
      dateTimeSetSelection = (dateTimeSetSelection <= 0) ? 5 : dateTimeSetSelection - 1;
      displaySetDateTimeMenu(); break;
  }
}

// === HANDLER : TEST RELAIS ===
void handleRelayTestMenuButton(int buttonIndex) {
  activateBacklight();

  switch (buttonIndex) {
    case 0: // ENTER -> activer le relais sélectionné 1s
      if (relayTestSelection == 0) { // Relais 1 (PD6/D12)
        digitalWrite(RELAY1_PIN, HIGH);
        delay(1000);
        digitalWrite(RELAY1_PIN, LOW);
      } else { // Relais 2 (PD7/D6)
        digitalWrite(RELAY2_PIN, HIGH);
        delay(1000);
        digitalWrite(RELAY2_PIN, LOW);
      }
      displayRelayTestMenu();
      break;

    case 1: // RETURN -> retour au menu principal
      currentMenu = MENU_MAIN;
      state.mainMenuInit = false;
      lcd.clear();
      displayMainMenu();
      break;

    case 2: // UP -> changer de relais
      relayTestSelection = (relayTestSelection == 0) ? 1 : 0;
      displayRelayTestMenu();
      break;

    case 4: // DOWN -> changer de relais
      relayTestSelection = (relayTestSelection == 0) ? 1 : 0;
      displayRelayTestMenu();
      break;

    // LEFT/RIGHT ignorés pour ce menu
  }
}

// === SETUP ET LOOP ===
void setup() {
  pinMode(LED_PIN, OUTPUT);
  for (byte i = 0; i < 6; i++) pinMode(BUTTON_PINS[i], INPUT_PULLUP);

  // Relais
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  Serial.begin(9600);
  smartPrintln(F("=== Smart Timer v0.1 ==="));
  lcd.begin(); loadSettings();
  
  lcd.clear(); lcd.setCursor(0, 1); lcd.print(F("SMART TIMER"));
  lcd.setCursor(0, 2); lcd.print(F("Ver.0.1 par Chewby")); delay(2000);
  
  if (!rtc.begin()) {
    smartPrintln(F("Erreur : DS3231 introuvable !"));
    lcd.clear(); lcd.print(F("ERREUR RTC")); delay(2000);
  } else {
    if (rtc.lostPower()) {
      smartPrintln(F("RTC perdue - mise à l'heure !"));
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    smartPrintln(F("RTC initialisé !"));
  }
  
  displayMainMenu();
}

void loop() {
  if (Serial && !state.serialWasConnected) { state.serialWasConnected = true; flushBuffer(); }
  else if (!Serial && state.serialWasConnected) state.serialWasConnected = false;
  
  unsigned long currentMillis = millis();
  updateBacklight();
  
  // RELAIS 1
  if (state.alarmActive) {
    digitalWrite(RELAY1_PIN, HIGH);
    if (currentMillis - alarmStartTime >= ALARM_DURATION) {
      state.alarmActive = false; digitalWrite(RELAY1_PIN, LOW); smartPrintln(F("Signal Relais 1 terminé"));
    }
  } else digitalWrite(RELAY1_PIN, LOW);
  
  // *** MODIFICATION PRINCIPALE : Synchroniser alarme avec affichage ***
  if (currentMillis - previousClockMillis >= CLOCK_UPDATE_INTERVAL) {
    previousClockMillis = currentMillis;
    
    // Vérification de l'alarme SYNCHRONISÉE avec l'update de l'horloge
    checkAlarmTrigger();
    
    // Affichage après vérification de l'alarme
    switch (currentMenu) {
      case TIMER_RUNNING: displayTimerRunning(); break;
      case ALARM_WAITING: displayAlarmWaiting(); break;
      case MENU_MAIN: displayMainMenu(); break;
    }
  }
  
  // Auto-stop alarme (garder cette logique en continu)
  if (state.alarmTriggered && !state.alarmActive) {
    if (currentTimerMode == MODE_ALARM_DAILY) {
      state.alarmTriggered = false;
      smartPrintln(F("Alarme daily - prête pour demain"));
    } else if (currentMillis - alarmStartTime >= ALARM_DURATION + 3000) {
      smartPrintln(F("Alarme arrêtée automatiquement"));
      stopAlarm(); lcd.clear(); displayMainMenu();
    }
  }
  
  // Timer check (garder cette logique en continu pour la précision)
  if (state.timerRunning && currentMenu == TIMER_RUNNING) {
    DateTime now = rtc.now();
    long elapsed = now.unixtime() - timerStartTime.unixtime();
    if (elapsed >= timerMinutes * 60 + timerSeconds) {
      if (currentTimerMode == MODE_REPEAT) {
        cycleCount++; timerStartTime = rtc.now(); state.timerScreenInit = false;
        state.alarmActive = true; alarmStartTime = millis();
        smartPrint(F("*** CYCLE ")); smartPrint(String(cycleCount)); smartPrintln(F(" FINI ***"));
      } else {
        state.timerRunning = false; state.timerFinished = true; state.alarmActive = true;
        alarmStartTime = millis(); currentMenu = TIMER_FINISHED;
        smartPrintln(F("*** TIMER FINI ! ***")); displayTimerFinished();
      }
    }
  }
  
  // Boutons (reste identique)
  for (byte i = 0; i < 6; i++) {
    int reading = digitalRead(BUTTON_PINS[i]);
    if (reading != state.lastButtonState[i]) lastDebounceTime[i] = currentMillis;
    
    if (currentMillis - lastDebounceTime[i] > DEBOUNCE_DELAY) {
      if (reading != state.currentButtonState[i]) {
        state.currentButtonState[i] = reading;
        if (reading == LOW) {
          activateBacklight();
          
          switch (currentMenu) {
            case MENU_MAIN: handleMainMenuButton(i); break;
            case MENU_SET_TIME: handleSetTimeButton(i); break;
            case MENU_SET_ALARM: handleSetAlarmButton(i); break;
            case MENU_SET_DATETIME: handleSetDateTimeButton(i); break;
            case MENU_TEST_RELAY: handleRelayTestMenuButton(i); break;
            case TIMER_RUNNING: if (i == 1) { stopTimer(); displayMainMenu(); } break;
            case TIMER_FINISHED:
              if (i == 1) {
                state.alarmActive = false; digitalWrite(RELAY1_PIN, LOW); state.timerFinished = false;
                state.timerScreenInit = false; currentMenu = MENU_MAIN; state.mainMenuInit = false; displayMainMenu();
              }
              break;
            case ALARM_WAITING: if (i == 1) { stopAlarm(); displayMainMenu(); } break;
          }
        }
      }
    }
    state.lastButtonState[i] = reading;
  }
}