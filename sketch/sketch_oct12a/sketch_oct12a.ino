#include <Wire.h>
#include <LCD_I2C.h>
#include <EEPROM.h>
#include <Mouse.h>

LCD_I2C lcd(0x27, 16, 4);

// === CONFIGURATION ===
const byte LED_PIN = 8;
const byte BUTTON_PINS[] = {A5, A4, A3, A2, A1, A0};

// === PINS RELAIS (ATmega32U4) ===
// PD6 = Arduino D12  |  PD7 = Arduino D6
const byte RELAY1_PIN = 12; // PD6
const byte RELAY2_PIN = 6;  // PD7

// Variables temporelles
unsigned long lastButtonActivity, lastDebounceTime[6], lastMouseMove, previousUpdateMillis;
const unsigned int BACKLIGHT_TIMEOUT = 30000, DEBOUNCE_DELAY = 50, UPDATE_INTERVAL = 100;

// Variables d'état
struct {
  bool lastButtonState[6] = {HIGH, HIGH, HIGH, HIGH, HIGH, HIGH};
  bool currentButtonState[6];
  bool mouseActive : 1;
  bool lcdBacklightOn : 1;
  bool lcdBacklightActive : 1;
  bool mainMenuInit : 1;
  bool configMenuInit : 1;
  bool serialWasConnected : 1;
} state;

// EEPROM addresses
enum { 
  EEPROM_SPEED = 0,           // Vitesse de mouvement (1-10)
  EEPROM_ZONE = 4,            // Zone de mouvement (50-500 pixels)
  EEPROM_INTERVAL = 8,        // Intervalle entre mouvements (100-5000ms)
  EEPROM_MODE = 12,           // Mode de simulation (0-2)
  EEPROM_BACKLIGHT = 16       // État rétroéclairage
};

// Modes de simulation
enum MouseMode : byte { 
  MODE_RANDOM,      // Mouvements complètement aléatoires
  MODE_JIGGLE,      // Petits mouvements pour éviter veille
  MODE_CIRCULAR     // Mouvements circulaires lents
};

// États du menu
enum MenuState : byte { 
  MENU_MAIN,        // Menu principal
  MENU_CONFIG,      // Configuration
  MOUSE_RUNNING,    // Simulation active
  MENU_TEST_RELAY   // <-- NOUVEAU : Test relais (dernier dans le menu)
};

MouseMode currentMouseMode = MODE_RANDOM;
MenuState currentMenu = MENU_MAIN;
byte menuSelection = 0;
byte configSelection = 0;

// Sélection pour le mode test relais
byte relayTestSelection = 0; // 0 = RELAY1 (PD6/D12), 1 = RELAY2 (PD7/D6)

// Paramètres de simulation
int mouseSpeed = 5;          // Vitesse de mouvement (1-10)
int mouseZone = 200;         // Zone de mouvement en pixels (50-500)
int mouseInterval = 1000;    // Intervalle entre mouvements en ms (100-5000)

// Variables pour les mouvements
int centerX = 0, centerY = 0;  // Position de référence
float circleAngle = 0;         // Pour le mode circulaire
int currentX = 0, currentY = 0; // Position actuelle estimée

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
  EEPROM.get(EEPROM_SPEED, mouseSpeed);
  EEPROM.get(EEPROM_ZONE, mouseZone);
  EEPROM.get(EEPROM_INTERVAL, mouseInterval);
  EEPROM.get(EEPROM_MODE, currentMouseMode);
  
  byte backlight;
  EEPROM.get(EEPROM_BACKLIGHT, backlight);
  state.lcdBacklightOn = (backlight <= 1) ? (backlight == 1) : true;
  
  // Validation des paramètres
  mouseSpeed = constrain(mouseSpeed, 1, 50);          // Augmenté de 10 à 50
  mouseZone = constrain(mouseZone, 50, 2000);         // Augmenté de 500 à 2000 pixels
  mouseInterval = constrain(mouseInterval, 50, 5000); // Réduit minimum à 50ms
  currentMouseMode = (MouseMode)constrain(currentMouseMode, 0, 2);
  
  state.lcdBacklightActive = state.lcdBacklightOn;
  lastButtonActivity = millis();
  
  if (state.lcdBacklightActive) lcd.backlight();
  else lcd.noBacklight();
  
  smartPrint(F("Paramètres: Vitesse=")); smartPrint(String(mouseSpeed));
  smartPrint(F("/50 Zone=")); smartPrint(String(mouseZone));
  smartPrint(F("/2000 Intervalle=")); smartPrint(String(mouseInterval));
  smartPrint(F(" Mode=")); 
  const __FlashStringHelper* modes[] = {F("Random"), F("Jiggle"), F("Circular")};
  smartPrintln(modes[currentMouseMode]);
}

void saveSettings() {
  EEPROM.put(EEPROM_SPEED, mouseSpeed);
  EEPROM.put(EEPROM_ZONE, mouseZone);
  EEPROM.put(EEPROM_INTERVAL, mouseInterval);
  EEPROM.put(EEPROM_MODE, currentMouseMode);
  EEPROM.put(EEPROM_BACKLIGHT, (byte)state.lcdBacklightOn);
  smartPrintln(F("Paramètres sauvegardés"));
}

// === AFFICHAGE ===
void displayMainMenu() {
  if (!state.mainMenuInit) {
    lcd.clear();
    
    // Menu à 5 options (ajout "Test Relais" en dernière position)
    const char* items[] = {
      "Mode: ",
      "Configuration", 
      "Demarrer Simu",
      "Retro: ",
      "Test Relais"
    };
    
    // Affiche 3 lignes autour de la sélection
    byte start = constrain(menuSelection - 1, 0, 2); // <- ajusté pour 5 éléments
    for (byte i = 0; i < 3; i++) {
      byte idx = start + i;
      if (idx < 5) { // <- ajusté (était 4)
        lcd.setCursor(0, i + 1);
        lcd.print(menuSelection == idx ? "> " : "  ");
        lcd.print(items[idx]);
        
        if (idx == 0) {
          const char* modes[] = {"Random", "Jiggle", "Circular"};
          lcd.print(modes[currentMouseMode]);
        } else if (idx == 3) {
          lcd.print(state.lcdBacklightOn ? "ON" : "OFF");
          if (state.lcdBacklightOn && !state.lcdBacklightActive) lcd.print("*");
        }
      }
    }
    state.mainMenuInit = true;
  }
  
  // Titre avec état
  lcd.setCursor(0, 0);
  lcd.print("MOUSE SIMULATOR ");
  if (state.mouseActive) {
    lcd.setCursor(15, 0);
    lcd.print("*");
  }
}

void displayConfigMenu() {
  if (!state.configMenuInit) {
    lcd.clear();
    lcd.print(F("CONFIGURATION"));
    state.configMenuInit = true;
  }
  
  lcd.setCursor(0, 1);
  lcd.print("Vitesse:  ");
  if (configSelection == 0) lcd.print(">");
  lcd.print(mouseSpeed);
  if (configSelection == 0) lcd.print("<");
  lcd.print("     "); // Effacer le reste
  
  lcd.setCursor(0, 2);
  lcd.print("Zone:     ");
  if (configSelection == 1) lcd.print(">");
  lcd.print(mouseZone);
  if (configSelection == 1) lcd.print("<");
  lcd.print("     ");
  
  lcd.setCursor(0, 3);
  lcd.print("Delai:    ");
  if (configSelection == 2) lcd.print(">");
  lcd.print(mouseInterval);
  if (configSelection == 2) lcd.print("<");
  lcd.print("     ");
}

void displayMouseRunning() {
  lcd.clear();
  lcd.print(F("SIMULATION ACTIVE"));
  
  lcd.setCursor(0, 1);
  lcd.print("Mode: ");
  const char* modes[] = {"Random", "Jiggle", "Circular"};
  lcd.print(modes[currentMouseMode]);
  
  lcd.setCursor(0, 2);
  lcd.print("Pos: ");
  lcd.print(currentX);
  lcd.print(",");
  lcd.print(currentY);
  lcd.print("     ");
  
  lcd.setCursor(0, 3);
  lcd.print("RETURN = Arreter");
}

// === AFFICHAGE : MENU TEST RELAIS (NOUVEAU) ===
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

// === SIMULATION SOURIS ===
void startMouseSimulation() {
  state.mouseActive = true;
  currentMenu = MOUSE_RUNNING;
  Mouse.begin();
  
  // Réinitialiser la position de référence
  centerX = 0; centerY = 0;
  currentX = 0; currentY = 0;
  circleAngle = 0;
  lastMouseMove = millis();
  
  smartPrint(F("Simulation démarrée - Mode: "));
  const __FlashStringHelper* modes[] = {F("Random"), F("Jiggle"), F("Circular")};
  smartPrintln(modes[currentMouseMode]);
  
  displayMouseRunning();
}

void stopMouseSimulation() {
  state.mouseActive = false;
  currentMenu = MENU_MAIN;
  state.mainMenuInit = false;
  Mouse.end();
  
  smartPrintln(F("Simulation arrêtée"));
  displayMainMenu();
}

void performMouseMovement() {
  if (!state.mouseActive) return;
  
  unsigned long currentTime = millis();
  if (currentTime - lastMouseMove < mouseInterval) return;
  
  int deltaX = 0, deltaY = 0;
  
  switch (currentMouseMode) {
    case MODE_RANDOM:
      {
        // Mouvement aléatoire dans la zone définie - PLUS GRANDS MOUVEMENTS
        float angle = random(0, 628) / 100.0; // 0 à 2π
        int distance = random(mouseSpeed, mouseSpeed * 25); // Distance plus importante
        deltaX = cos(angle) * distance;
        deltaY = sin(angle) * distance;
        
        // Limiter à la zone avec retour élastique
        if (abs(currentX + deltaX) > mouseZone/2) {
          deltaX = -(currentX / 3); // Retour vers le centre
        }
        if (abs(currentY + deltaY) > mouseZone/2) {
          deltaY = -(currentY / 3); // Retour vers le centre
        }
      }
      break;
      
    case MODE_JIGGLE:
      {
        // Mouvements moyens pour éviter la mise en veille - AMPLIFIÉS
        deltaX = random(-5, 6) * mouseSpeed; // Multiplié par mouseSpeed directement
        deltaY = random(-5, 6) * mouseSpeed;
        
        // Revenir vers le centre de temps en temps
        if (random(0, 15) == 0) {
          deltaX = -currentX / 2;
          deltaY = -currentY / 2;
        }
      }
      break;
      
    case MODE_CIRCULAR:
      {
        // Mouvement circulaire avec rayon ajustable
        float radius = mouseZone / 3; // Plus grand rayon
        deltaX = cos(circleAngle) * radius - currentX;
        deltaY = sin(circleAngle) * radius - currentY;
        
        circleAngle += 0.05 * mouseSpeed / 10.0; // Vitesse plus fine
        if (circleAngle >= 6.28) circleAngle = 0; // 2π
      }
      break;
  }
  
  // Effectuer le mouvement
  if (deltaX != 0 || deltaY != 0) {
    Mouse.move(deltaX, deltaY);
    currentX += deltaX;
    currentY += deltaY;
    
    // LED clignotante pour indiquer l'activité
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    
    lastMouseMove = currentTime;
    
    smartPrint(F("Mouvement: "));
    smartPrint(String(deltaX) + "," + String(deltaY));
    smartPrint(F(" -> Position: "));
    smartPrintln(String(currentX) + "," + String(currentY));
  }
}

// === HANDLERS BOUTONS ===
void handleMainMenuButton(int buttonIndex) {
  switch (buttonIndex) {
    case 0: // ENTER
      switch (menuSelection) {
        case 0: // Mode
          currentMouseMode = (MouseMode)((currentMouseMode + 1) % 3);
          saveSettings(); 
          state.mainMenuInit = false; 
          displayMainMenu();
          break;
        case 1: // Configuration
          currentMenu = MENU_CONFIG; 
          configSelection = 0; 
          state.configMenuInit = false;
          displayConfigMenu();
          break;
        case 2: // Démarrer
          startMouseSimulation();
          break;
        case 3: // Backlight
          state.lcdBacklightOn = !state.lcdBacklightOn;
          if (state.lcdBacklightOn) { 
            state.lcdBacklightActive = true; 
            lcd.backlight(); 
          } else { 
            state.lcdBacklightActive = false; 
            lcd.noBacklight(); 
          }
          lastButtonActivity = millis(); 
          saveSettings(); 
          state.mainMenuInit = false; 
          displayMainMenu();
          break;
        case 4: // <-- Nouveau : Test Relais (dernier item)
          currentMenu = MENU_TEST_RELAY;
          relayTestSelection = 0;
          displayRelayTestMenu();
          break;
      }
      break;
    case 2: // UP
      menuSelection = (menuSelection == 0) ? 4 : menuSelection - 1; // <-- ajusté (max = 4)
      state.mainMenuInit = false; 
      displayMainMenu(); 
      break;
    case 4: // DOWN
      menuSelection = (menuSelection == 4) ? 0 : menuSelection + 1; // <-- ajusté (max = 4)
      state.mainMenuInit = false; 
      displayMainMenu(); 
      break;
  }
}

void handleConfigMenuButton(int buttonIndex) {
  activateBacklight();
  
  switch (buttonIndex) {
    case 0: // ENTER - Sauvegarder
      saveSettings(); 
      currentMenu = MENU_MAIN; 
      state.mainMenuInit = false; 
      lcd.clear(); 
      displayMainMenu(); 
      break;
    case 1: // RETURN - Annuler
      loadSettings(); 
      currentMenu = MENU_MAIN; 
      state.mainMenuInit = false; 
      lcd.clear(); 
      displayMainMenu(); 
      break;
    case 2: // UP - Augmenter valeur
      switch (configSelection) {
        case 0: mouseSpeed = constrain(mouseSpeed + 2, 1, 50); break;        // +2 par pas
        case 1: mouseZone = constrain(mouseZone + 50, 50, 2000); break;      // +50 pixels par pas
        case 2: mouseInterval = constrain(mouseInterval + 50, 50, 5000); break; // +50ms par pas
      }
      displayConfigMenu(); 
      break;
    case 3: // RIGHT - Changer paramètre
      configSelection = (configSelection + 1) % 3; 
      displayConfigMenu(); 
      break;
    case 4: // DOWN - Diminuer valeur
      switch (configSelection) {
        case 0: mouseSpeed = constrain(mouseSpeed - 2, 1, 50); break;        // -2 par pas
        case 1: mouseZone = constrain(mouseZone - 50, 50, 2000); break;      // -50 pixels par pas
        case 2: mouseInterval = constrain(mouseInterval - 50, 50, 5000); break; // -50ms par pas
      }
      displayConfigMenu(); 
      break;
    case 5: // LEFT - Changer paramètre (sens inverse)
      configSelection = (configSelection == 0) ? 2 : configSelection - 1; 
      displayConfigMenu(); 
      break;
  }
}

// === HANDLER : TEST RELAIS (NOUVEAU) ===
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
  smartPrintln(F("=== Mouse Simulator v1.0 ==="));
  
  lcd.begin(); 
  loadSettings();
  
  lcd.clear(); 
  lcd.setCursor(0, 1); 
  lcd.print(F("MOUSE SIMULATOR"));
  lcd.setCursor(0, 2); 
  lcd.print(F("Ver.1.0 par Chewby")); 
  delay(2000);
  
  displayMainMenu();
  
  smartPrintln(F("Système initialisé !"));
  smartPrintln(F("ATTENTION: Ce programme contrôle la souris !"));
}

void loop() {
  // Gestion du buffer série
  if (Serial && !state.serialWasConnected) { 
    state.serialWasConnected = true; 
    flushBuffer(); 
  } else if (!Serial && state.serialWasConnected) {
    state.serialWasConnected = false;
  }
  
  unsigned long currentMillis = millis();
  updateBacklight();
  
  // Simulation de la souris
  if (state.mouseActive) {
    performMouseMovement();
  } else {
    digitalWrite(LED_PIN, LOW); // LED éteinte si pas actif
  }
  
  // Mise à jour de l'affichage
  if (currentMillis - previousUpdateMillis >= UPDATE_INTERVAL) {
    previousUpdateMillis = currentMillis;
    
    if (currentMenu == MOUSE_RUNNING && state.mouseActive) {
      displayMouseRunning();
    }
  }
  
  // Gestion des boutons
  for (byte i = 0; i < 6; i++) {
    int reading = digitalRead(BUTTON_PINS[i]);
    if (reading != state.lastButtonState[i]) lastDebounceTime[i] = currentMillis;
    
    if (currentMillis - lastDebounceTime[i] > DEBOUNCE_DELAY) {
      if (reading != state.currentButtonState[i]) {
        state.currentButtonState[i] = reading;
        if (reading == LOW) {
          activateBacklight();
          
          switch (currentMenu) {
            case MENU_MAIN: 
              handleMainMenuButton(i); 
              break;
            case MENU_CONFIG: 
              handleConfigMenuButton(i); 
              break;
            case MOUSE_RUNNING: 
              if (i == 1) { // RETURN
                stopMouseSimulation(); 
              } 
              break;
            case MENU_TEST_RELAY: // <-- NOUVEAU
              handleRelayTestMenuButton(i);
              break;
          }
        }
      }
    }
    state.lastButtonState[i] = reading;
  }
}