
//        Bibliothèque ESP32audioI2S capricieuse faire plusieurs tentatives ou redemarrer IDE
//        Passage en version 3.3.12  et moins d'erreurs, mais si toujour encore aléatoirement
//        -----------------------------------------------------------------------------------    
// ESP32-S3 N16R8 + Waveshare 320x480 tactile + lecteur SD séparé
// Lecteur de fichiers audio de la carte SD (/rec), interface PAYSAGE 480x320 (fond + galets)
// Dossier du sketch : lecteur_sd/  ->  lecteur_sd.ino, types.h
// SPIFFS : /fond.jpg  /galetGauche.bmp  /galetDroit.bmp  /play.bmp
// Carte SD : fichiers .mp3 / .aac / .ogg dans le dossier /rec
// Pochette d'album (ID3 APIC, JPEG) affichée en x115 y220, zone 140x55

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include "U8g2_for_TFT_eSPI.h"
#include <TJpg_Decoder.h>
#include <Audio.h>

#include "types.h"                        // Scroller, RecEntry, Zone

// =====================  À ADAPTER  =====================
// I2S -> MAX98357A
#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 4

// Tactile FT6336U (I2C 0x38)
#define TP_ADDR 0x38
#define TP_SDA  8
#define TP_SCL  9
#define TP_RST  16

// Carte SD (bus SPI séparé de l'écran)
#define SD_CS   38
#define SD_MOSI 40
#define SD_SCK  41
#define SD_MISO 42
#define SD_FREQ 4000000

// Panneau natif (portrait)
#define PANEL_W 320
#define PANEL_H 480

// 1 = interface (écran, tactile, galets) sur le coeur 0, audio seul sur le coeur 1
#define UI_ON_CORE0 1

// Animation des galets : intervalle en ms (plus grand = plus léger pour le processeur)
#define GALET_MS 45

// fond.jpg : si le fichier est en 320x480 (portrait), il est tourné pour le mode paysage.
// 1 = rotation horaire, 2 = anti-horaire (essaie l'autre valeur si l'image est à l'envers).
// Si le fichier est déjà en 480x320, il est utilisé tel quel.
#define BG_ROT_DIR 1

// Coordonnées des galets : 0 = (x, y) donnés = coin haut-gauche du galet, 1 = CENTRE du galet
#define GALET_POS_IS_CENTER 0

// Transparence des galets (BMP sur fond noir)
#define GALET_BLACK_TOL  24   // noir "presque noir" accepté comme transparent (0 = noir pur seulement)
#define GALET_ROUND_MASK 1    // 1 = tout ce qui est hors du disque (les angles) devient transparent

// Icône play.bmp : noir "presque noir" accepté comme transparent (0 = noir pur seulement)
#define PLAY_BLACK_TOL 24

// VU-mètre stéréo (zone x60 y280 262x36) : 1 = activé
#define VU_ENABLED    1
#define VU_FULL_SCALE 0   // 0 = gain réglé automatiquement ; sinon valeur max renvoyée par getVUlevel()
#define VU_SHOW_OFF   1   // 1 = segments éteints visibles (gris foncé), 0 = seuls les segments allumés

// Affichage artiste / titre / progression : 1 = activé
#define SHOW_INFO 1

// Pochette d'album : 1 = activée
#define COVER_ENABLED 1

// Couleurs
#define COL_FARTIST TFT_DARKGREY
#define COL_FTITLE  TFT_DARKGREY
#define COL_FBAR    TFT_DARKGREY
// ======================================================

#define SCR_W 480                         // paysage en permanence (rotation 1)
#define SCR_H 320

TFT_eSPI tft = TFT_eSPI();
U8g2_for_TFT_eSPI u8f;
Audio audio;
Preferences prefs;

enum Page { PAGE_LIST, PAGE_PLAYER };
volatile Page page = PAGE_LIST;

// ---- Liste des fichiers (PSRAM) ----
const int MAX_REC = 400;
RecEntry* recList = nullptr;
int       recCount = 0;
bool      sdOk = false;
SPIClass  sdSPI(FSPI);                    // l'écran utilise HSPI (USE_HSPI_PORT)

int topIdx = 0;
int volume = 6;                           // 0..21
uint32_t volSaveAt = 0;                   // sauvegarde différée du volume

// Message temporaire sur la liste
char     toastMsg[48] = "";
uint32_t toastUntil = 0;

// ---- Mise en page : liste (paysage) ----
const int HEADER_H = 36;
const int LIST_Y   = 38;
const int ROW_H    = 34;
const int ROWS     = 6;                   // 38 .. 242
const int STATUS_Y = 254;
const int BTN_Y    = 268;
const int BTN_H    = 48;
const int BTN_W    = 150;                 // 3 boutons : Up, Dn, MAJ
const int BTN_GAP  = 10;

// Appui long (suppression)
bool     pressActive = false;
uint32_t pressT0 = 0;
int      pressIdx = -1;
bool     confirmAct = false;
int      confirmIdx = -1;

// ---- Lecture d'un fichier ----
volatile bool playFileReq = false;        // demande de lecture (tâche audio)
volatile bool pauseReq    = false;        // demande pause/reprise
volatile bool stopReq     = false;        // demande d'arrêt (tâche audio)
volatile bool volDirty    = false;
volatile int  fileResult  = 0;            // 0 = en cours, 1 = ouvert, -1 = échec
volatile uint32_t audCur = 0, audDur = 0; // temps écoulé / durée, publiés par la tâche audio
volatile uint16_t vuRaw = 0;              // niveaux gauche (octet haut) / droite (octet bas), publiés par la tâche audio
char     playFilePath[128] = "";
int      fileIdx = -1;
bool     fileOpened = false, filePaused = false, fileEnded = false, fileFailShown = false;
bool     shuffleMode = false;             // lecture aléatoire
uint32_t fileRunMs = 0;
String   fArtist = "", fTitle = "";
uint32_t fpLastCur = 0xFFFFFFFFu, fpLastDur = 0xFFFFFFFFu;
bool     fpForce = true;

// ---- Fond décodé en PSRAM (paysage 480x320) ----
#define BG_W 480
#define BG_H 320
uint16_t* bgBuf = nullptr;
int bgRot = 0;                            // 0 = direct, 1 = rotation horaire, 2 = anti-horaire
int bgSrcW = 0, bgSrcH = 0;               // dimensions du fichier JPEG

Scroller scFArtist, scFTitle;

// ======================================================================
//                              POLICES / TEXTES
// ======================================================================
#define FONT_N 3
const uint8_t* const FONTS[FONT_N] = {
  u8g2_font_profont22_tf,
  u8g2_font_profont17_tf,
  u8g2_font_profont15_tf
};

void chopUtf8(String& s) {
  if (!s.length()) return;
  int i = s.length() - 1;
  while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
  s.remove(i);
}

// Texte fixe (réduit puis tronqué)
void drawTextFit(const String& text, int x, int yc, int maxW, uint16_t color, int firstFont) {
  String t = text;
  u8f.setForegroundColor(color);
  int f = firstFont;
  for (; f < FONT_N; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(t.c_str()) <= maxW) break;
  }
  if (f >= FONT_N) {
    u8f.setFont(FONTS[FONT_N - 1]);
    while (t.length() > 1 && u8f.getUTF8Width((t + "~").c_str()) > maxW) chopUtf8(t);
    t += "~";
  }
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(x, yc + (asc + desc) / 2);
  u8f.print(t);
}

// ---------------- Texte défilant ----------------
void scrollInit(Scroller& s, int x, int y, int w, int h, uint16_t color, uint16_t bg, int fFirst, int fLast) {
  s.x = x; s.y = y; s.w = w; s.h = h; s.color = color; s.bg = bg;
  s.fFirst = fFirst; s.fLast = fLast;
}

void scrollRender(Scroller& s) {
  tft.setViewport(s.x, s.y, s.w, s.h);
  if (s.bgImg && bgBuf) {                 // fond image restauré ligne par ligne
    for (int r = 0; r < s.h; r++)
      tft.pushImage(0, r, s.w, 1, bgBuf + (s.y + r) * BG_W + s.x);
  } else {
    tft.fillRect(0, 0, s.w, s.h, s.bg);
  }
  u8f.setFont(FONTS[s.font]);
  u8f.setFontMode(1);                     // 1 = fond transparent (pas de rectangle noir derrière les lettres)
  u8f.setForegroundColor(s.color);
  int asc = u8f.getFontAscent(), desc = u8f.getFontDescent();
  u8f.setCursor(s.scrolling ? -s.offset : 0, s.h / 2 + (asc + desc) / 2);
  u8f.print(s.text);
  tft.resetViewport();
}

void scrollSet(Scroller& s, const String& text) {
  s.text = text.length() ? text : String("-");
  int f = s.fFirst;
  for (; f <= s.fLast; f++) {
    u8f.setFont(FONTS[f]);
    if (u8f.getUTF8Width(s.text.c_str()) <= s.w) break;
  }
  if (f > s.fLast) { f = s.fLast; u8f.setFont(FONTS[f]); s.scrolling = true; }
  else s.scrolling = false;
  s.font = f;
  s.textW = u8f.getUTF8Width(s.text.c_str());
  s.offset = 0; s.dir = 1;
  s.pauseUntil = millis() + 1500;
  s.lastStep = millis();
  scrollRender(s);
}

void scrollTick(Scroller& s) {
  if (!s.scrolling) return;
  uint32_t now = millis();
  if (now < s.pauseUntil || now - s.lastStep < 35) return;
  s.lastStep = now;
  int maxOff = s.textW - s.w + 8;
  s.offset += s.dir * 2;
  if (s.offset >= maxOff) { s.offset = maxOff; s.dir = -1; s.pauseUntil = now + 1200; }
  else if (s.offset <= 0) { s.offset = 0;      s.dir = 1;  s.pauseUntil = now + 1200; }
  scrollRender(s);
}

// ======================================================================
//                              CARTE SD
// ======================================================================
bool sdInit() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI, SD_FREQ);
  Serial.println(sdOk ? "SD OK" : "SD absente ou erreur");
  return sdOk;
}

bool isAudioName(const char* n) {
  size_t l = strlen(n);
  if (l < 5) return false;
  const char* e = n + l - 4;
  return !strcasecmp(e, ".mp3") || !strcasecmp(e, ".aac") || !strcasecmp(e, ".ogg");
}

int cmpRec(const void* a, const void* b) {
  const RecEntry* x = (const RecEntry*)a;
  const RecEntry* y = (const RecEntry*)b;
  if (x->t != y->t) return (x->t > y->t) ? -1 : 1;        // plus récent d'abord
  return strcasecmp(x->name, y->name);
}

// À n'appeler que lorsque l'audio est arrêté (la SD n'est pas partagée entre tâches)
void recScan() {
  recCount = 0;
  if (!recList) return;
  if (!sdOk && !sdInit()) return;

  File dir = SD.open("/rec");
  if (!dir) { Serial.println("/rec absent"); return; }
  if (!dir.isDirectory()) { dir.close(); return; }

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory() && recCount < MAX_REC) {
      const char* full = f.name();
      const char* nm = strrchr(full, '/');
      nm = nm ? nm + 1 : full;
      if (nm[0] != '_' && nm[0] != '.' && isAudioName(nm)) {
        RecEntry& e = recList[recCount++];
        strlcpy(e.name, nm, sizeof(e.name));
        e.size = (uint32_t)f.size();
        e.t = (uint32_t)f.getLastWrite();
      }
    }
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  qsort(recList, recCount, sizeof(RecEntry), cmpRec);
  Serial.printf("%d fichiers\n", recCount);
}

void recDelete(int idx) {
  if (idx < 0 || idx >= recCount) return;
  String p = String("/rec/") + recList[idx].name;
  bool ok = SD.remove(p.c_str());
  Serial.printf("Suppression %s : %s\n", p.c_str(), ok ? "OK" : "echec");
  recScan();
}

// Nom de fichier -> "Artiste" / "Titre" (sans extension)
void fileNameParts(const char* name, String& artist, String& title) {
  char base[100];
  strlcpy(base, name, sizeof(base));
  char* d = strrchr(base, '.');
  if (d) *d = 0;
  String s(base);
  int p = s.indexOf(" - ");
  if (p > 0) { artist = s.substring(0, p); title = s.substring(p + 3); }
  else       { artist = ""; title = s; }
}

void fmtSize(char* out, size_t n, uint32_t sz) {
  if (sz >= 1048576UL) snprintf(out, n, "%.1f Mo", sz / 1048576.0f);
  else                 snprintf(out, n, "%u Ko", (unsigned)(sz / 1024));
}

void fmtTime(char* out, size_t n, uint32_t s) {
  if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  else           snprintf(out, n, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void toast(const char* m) {
  strlcpy(toastMsg, m, sizeof(toastMsg));
  toastUntil = millis() + 3000;
}

// Fichier au hasard, différent de celui en cours
int randomIdx() {
  if (recCount <= 1) return 0;
  int i;
  do { i = (int)(esp_random() % (uint32_t)recCount); } while (i == fileIdx);
  return i;
}

// ======================================================================
//                    AUDIO (tâche dédiée, coeur 1)
// ======================================================================
void coverExtract(const char* path);       // défini plus bas (pochette)

void audioTask(void*) {
  for (;;) {
    if (stopReq) {
      audio.stopSong();
      audCur = 0; audDur = 0;
      stopReq = false;                                  // signale à l'interface : arrêt effectué
    }
    if (playFileReq) {
      playFileReq = false;
      audCur = 0; audDur = 0;
      audio.stopSong();
      audio.setVolume(volume);
      coverExtract(playFilePath);                       // pochette ID3 (lue ici, dans la tâche audio)
      fileResult = audio.connecttoFS(SD, playFilePath) ? 1 : -1;
    }
    if (pauseReq) { pauseReq = false; audio.pauseResume(); }
    if (volDirty) { volDirty = false; audio.setVolume(volume); }

    // Temps de lecture : mesuré ici, dans la tâche audio, qui seule touche au fichier.
    // L'interface lit simplement audCur / audDur (aucun accès SD depuis l'interface).
    static uint32_t lastInfo = 0;
    if (fileResult == 1 && millis() - lastInfo >= 500) {
      lastInfo = millis();
      audCur = audio.getAudioCurrentTime();
      audDur = audio.getAudioFileDuration();
    }

#if VU_ENABLED
    // Niveau des deux canaux : lu ici (tâche audio) et publié pour l'interface
    static uint32_t lastVU = 0;
    if (fileResult == 1 && millis() - lastVU >= 30) {
      lastVU = millis();
        vuRaw = audio.getVUlevel();
    }
#endif

    audio.loop();
    vTaskDelay(1);
  }
}

// ======================================================================
//                              TACTILE
// ======================================================================
void touchInit() {
  if (TP_RST >= 0) {
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);  delay(10);
    digitalWrite(TP_RST, HIGH); delay(100);
  }
  Wire.begin(TP_SDA, TP_SCL, 400000);
}

// Retourne le nombre de doigts (0, 1 ou 2) ; x,y = premier doigt, repère paysage
int readTouch(uint16_t &x, uint16_t &y) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)TP_ADDR, (uint8_t)5) != 5) return 0;

  uint8_t n  = Wire.read() & 0x0F;
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if (n == 0 || n > 2) return 0;

  uint16_t rawX = ((xh & 0x0F) << 8) | xl;
  uint16_t rawY = ((yh & 0x0F) << 8) | yl;
  x = rawY;
  y = (PANEL_W - 1) - rawX;
  return n;
}

// ======================================================================
//                         PAGE LISTE (paysage)
// ======================================================================
void drawListHeader() {
  tft.fillRect(0, 0, SCR_W, HEADER_H, TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("Lecteur SD", 10, HEADER_H / 2, 4);
  char v[16];
  snprintf(v, sizeof(v), "Vol %d", volume);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(v, SCR_W - 10, HEADER_H / 2, 2);
}

void drawList() {
  tft.fillRect(0, LIST_Y, SCR_W, BTN_Y - LIST_Y, TFT_BLACK);
  tft.setTextSize(1);

  if (recCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(sdOk ? "Aucun fichier" : "Carte SD absente", SCR_W / 2, LIST_Y + 3 * ROW_H, 4);
    return;
  }

  for (int r = 0; r < ROWS; r++) {
    int idx = topIdx + r;
    if (idx >= recCount) break;
    int y = LIST_Y + r * ROW_H;
    tft.drawFastHLine(0, y + ROW_H - 1, SCR_W, TFT_DARKGREY);

    char base[100];
    strlcpy(base, recList[idx].name, sizeof(base));
    char* dot = strrchr(base, '.');
    if (dot) *dot = 0;
    drawTextFit(String(base), 10, y + ROW_H / 2, SCR_W - 100, TFT_WHITE, 1);

    char sz[16];
    fmtSize(sz, sizeof(sz), recList[idx].size);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(MR_DATUM);
    tft.drawString(sz, SCR_W - 8, y + ROW_H / 2, 2);
  }

  // Ligne d'état
  String ctx = "Appui long : supprimer";
  uint16_t ctxCol = TFT_LIGHTGREY;
  if (millis() < toastUntil) { ctx = toastMsg; ctxCol = TFT_ORANGE; }
  drawTextFit(ctx, 8, STATUS_Y, 330, ctxCol, 2);

  char buf[32];
  snprintf(buf, sizeof(buf), "%d-%d / %d", topIdx + 1, min(recCount, topIdx + ROWS), recCount);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(buf, SCR_W - 8, STATUS_Y, 2);
}

void drawListButtons() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(1);
  const char* lbl[3] = { "Up", "Dn", "MAJ" };
  for (int k = 0; k < 3; k++) {
    int x = BTN_GAP + k * (BTN_W + BTN_GAP);
    tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 8, TFT_DARKCYAN);
    tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
    tft.drawString(lbl[k], x + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
  }
}

void redrawList() {
  tft.fillScreen(TFT_BLACK);
  drawListHeader();
  drawList();
  drawListButtons();
}

void drawConfirm() {
  tft.fillRoundRect(40, 70, 400, 180, 10, TFT_BLACK);
  tft.drawRoundRect(40, 70, 400, 180, 10, TFT_WHITE);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Supprimer ce fichier ?", 240, 100, 4);
  char base[100];
  strlcpy(base, recList[confirmIdx].name, sizeof(base));
  char* dot = strrchr(base, '.');
  if (dot) *dot = 0;
  drawTextFit(String(base), 60, 140, 360, TFT_CYAN, 1);

  tft.fillRoundRect(90, 190, 140, 50, 8, TFT_RED);
  tft.fillRoundRect(250, 190, 140, 50, 8, TFT_DARKCYAN);
  tft.setTextColor(TFT_WHITE, TFT_RED);
  tft.drawString("Oui", 160, 215, 4);
  tft.setTextColor(TFT_WHITE, TFT_DARKCYAN);
  tft.drawString("Non", 320, 215, 4);
}

// ======================================================================
//        PAGE LECTEUR (paysage 480x320, fond + galets, SPIFFS)
// ======================================================================
// Zones tactiles : colonne de droite, 105x37
const Zone Z_NEXT   = { 370,  28, 105, 40 };   // suivant (au hasard en mode aléatoire)
const Zone Z_PLAY   = { 370,  70, 105, 40 };   // lecture / pause (+ icône play.bmp)
const Zone Z_PREV   = { 370, 113, 105, 40 };   // retour : morceau précédent
const Zone Z_VPLUS  = { 370, 153, 105, 40 };
const Zone Z_VMOINS = { 370, 190, 105, 40 };
const Zone Z_LIST   = { 370, 227, 105, 40 };   // retour à la liste
const Zone Z_RAND   = { 370, 263, 105, 40 };   // musique : lecture aléatoire

// Zone texte (artiste + titre) : x40 y58 300x46, deux lignes de 23 px
const Zone Z_ARTIST = {  40,  58, 300, 23 };
const Zone Z_TITLE  = {  40,  81, 300, 23 };

// Zone de progression : x29 y192 323x22 -> temps écoulé | barre | durée
const Zone Z_PROG   = {  28, 192, 318, 22 };
#define PROG_TXT_W 60                          // largeur réservée à chaque temps
#define PROG_BAR_H 10                          // hauteur de la barre

#define F_FOND   "/fond.jpg"
#define F_GAUCHE "/galetGauche.bmp"
#define F_DROIT  "/galetDroit.bmp"
#define F_PLAY   "/play.bmp"

// Galets 69x69
#define G_SIZE 69
#define GAL_L_X 71
#define GAL_L_Y 114
#define GAL_R_X 236
#define GAL_R_Y 114
#if GALET_POS_IS_CENTER
  #define GL_X GAL_L_X
  #define GL_Y GAL_L_Y
  #define GR_X GAL_R_X
  #define GR_Y GAL_R_Y
#else
  #define GL_X (GAL_L_X + G_SIZE / 2)
  #define GL_Y (GAL_L_Y + G_SIZE / 2)
  #define GR_X (GAL_R_X + G_SIZE / 2)
  #define GR_Y (GAL_R_Y + G_SIZE / 2)
#endif
const float VIT_GAUCHE = 3.0f;           // degrés par image
const float VIT_DROIT  = 5.0f;           // un peu plus vite
// Tampon de recomposition : juste la boîte du galet (disque inscrit dans 69x69),
// pour ne jamais effacer le texte (y<=104) ni la progression (y>=192)
#define TAMP_W 72
#define TAMP_H 72

// Icône play : 105x37 sur la zone Lecture, fond d'origine restauré en pause
#define PLAY_X 370
#define PLAY_Y 70
#define PLAY_W 105
#define PLAY_H 40

TFT_eSprite galetG = TFT_eSprite(&tft);
TFT_eSprite galetD = TFT_eSprite(&tft);
TFT_eSprite tampon = TFT_eSprite(&tft);
TFT_eSprite spritePlay = TFT_eSprite(&tft);
TFT_eSprite tamponPlay = TFT_eSprite(&tft);
bool uiOk = false;
bool playOk = false;                     // play.bmp chargé
bool playIconShown = false;
float angG = 0, angD = 0;

// ---- VU-mètre ----
const Zone Z_VU = { 60, 280, 262, 36 };
#define VU_SEGS 32                       // nombre de segments par canal
TFT_eSprite spriteVU = TFT_eSprite(&tft);
bool vuOk = false;
bool vuIdleShown = false;                // zone au repos déjà dessinée
uint32_t vuLast = 0;
float vuL = 0, vuR = 0;                  // niveaux lissés 0..1
float vuRefL = 30, vuRefR = 30;          // référence du gain automatique
int vuPkL = 0, vuPkR = 0, vuHoldL = 0, vuHoldR = 0;   // marqueurs de crête
uint16_t vuGreen, vuYellow, vuRed, vuOff;

// --- Décodage de fond.jpg dans la PSRAM, avec rotation éventuelle (paysage) ---
bool bgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!bgBuf) return 0;
  if (bgRot == 0) {
    for (int r = 0; r < h; r++) {
      int yy = y + r;
      if (yy < 0 || yy >= BG_H) continue;
      int cw = w;
      if (x + cw > BG_W) cw = BG_W - x;
      if (cw <= 0) continue;
      memcpy(bgBuf + yy * BG_W + x, bitmap + r * w, cw * 2);
    }
  } else {
    for (int r = 0; r < h; r++) {
      for (int c = 0; c < w; c++) {
        int sx = x + c, sy = y + r;
        int dx, dy;
        if (bgRot == 1) { dx = bgSrcH - 1 - sy; dy = sx; }          // 90° horaire
        else            { dx = sy;              dy = bgSrcW - 1 - sx; }   // 90° anti-horaire
        if (dx >= 0 && dx < BG_W && dy >= 0 && dy < BG_H)
          bgBuf[dy * BG_W + dx] = bitmap[r * w + c];
      }
    }
  }
  return 1;
}

// --- BMP 24/32 bits (SPIFFS) -> TFT_eSprite 16 bits ---
// tol > 0 : les pixels dont r, g et b sont tous <= tol sont forcés au noir pur (transparent)
bool loadBmpSprite(TFT_eSprite& s, const char* path, int tol) {
  File f = SPIFFS.open(path, "r");
  if (!f) { Serial.printf("BMP introuvable (SPIFFS) : %s\n", path); return false; }
  uint8_t h[54];
  if (f.read(h, 54) != 54 || h[0] != 'B' || h[1] != 'M') { f.close(); return false; }
  uint32_t off; int32_t w, ht; uint16_t bpp; uint32_t comp;
  memcpy(&off, h + 10, 4); memcpy(&w, h + 18, 4); memcpy(&ht, h + 22, 4);
  memcpy(&bpp, h + 28, 2); memcpy(&comp, h + 30, 4);
  bool bottomUp = ht > 0;
  int absH = abs(ht);
  if (w <= 0 || w > 128 || absH <= 0 || absH > 128 ||
      !(bpp == 24 || bpp == 32) || !(comp == 0 || comp == 3)) {
    Serial.printf("BMP non gere : %s (%dx%d, %u bits)\n", path, (int)w, (int)ht, (unsigned)bpp);
    f.close();
    return false;
  }
  s.setColorDepth(16);
  if (!s.createSprite(w, absH)) { f.close(); return false; }
  s.fillSprite(0);
  int bp = bpp / 8;
  uint32_t rowSize = ((bpp * w + 31) / 32) * 4;
  uint8_t row[128 * 4 + 4];
  for (int y = 0; y < absH; y++) {
    int srcRow = bottomUp ? (absH - 1 - y) : y;
    f.seek(off + (uint32_t)srcRow * rowSize);
    if (f.read(row, rowSize) != (int)rowSize) break;
    for (int x = 0; x < w; x++) {
      uint8_t b = row[x * bp], g = row[x * bp + 1], r = row[x * bp + 2];
      uint16_t c = tft.color565(r, g, b);
      if (tol > 0 && r <= tol && g <= tol && b <= tol) c = 0;   // presque noir -> transparent
      s.drawPixel(x, y, c);
    }
  }
  f.close();
  return true;
}

// Tout ce qui est hors du disque inscrit (les angles) devient transparent
void roundMask(TFT_eSprite& s) {
  int cw = s.width(), ch = s.height();
  float cx = (cw - 1) / 2.0f, cy = (ch - 1) / 2.0f;
  float rr = (cw < ch ? cw : ch) / 2.0f;
  for (int yy = 0; yy < ch; yy++)
    for (int xx = 0; xx < cw; xx++) {
      float dx = xx - cx, dy = yy - cy;
      if (dx * dx + dy * dy > rr * rr) s.drawPixel(xx, yy, 0);
    }
}

// --- Chargement unique du fond, des galets et de l'icône play (SPIFFS) ---
bool uiLoad() {
  if (uiOk) return true;

  if (!bgBuf) bgBuf = (uint16_t*)heap_caps_malloc(BG_W * BG_H * 2, MALLOC_CAP_SPIRAM);
  if (!bgBuf) { Serial.println("PSRAM insuffisante pour le fond"); return false; }

  File f = SPIFFS.open(F_FOND, "r");
  if (!f) { Serial.println("/fond.jpg introuvable (SPIFFS)"); return false; }
  size_t len = f.size();
  uint8_t* jb = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!jb) { f.close(); return false; }
  f.read(jb, len);
  f.close();

  memset(bgBuf, 0, BG_W * BG_H * 2);
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, jb, len);
  bgSrcW = w; bgSrcH = h;
  if (w == 320 && h == 480)      bgRot = BG_ROT_DIR;      // portrait : tourné pour le paysage
  else                           bgRot = 0;               // déjà en paysage (480x320)
  if (!(w == 480 && h == 320) && !(w == 320 && h == 480))
    Serial.printf("fond.jpg : %dx%d (attendu 480x320 ou 320x480)\n", w, h);
  TJpgDec.drawJpg(0, 0, jb, len);
  free(jb);

  if (!loadBmpSprite(galetG, F_GAUCHE, GALET_BLACK_TOL)) return false;
  if (!loadBmpSprite(galetD, F_DROIT, GALET_BLACK_TOL))  return false;
#if GALET_ROUND_MASK
  roundMask(galetG);
  roundMask(galetD);
#endif
  galetG.setPivot(galetG.width() / 2, galetG.height() / 2);
  galetD.setPivot(galetD.width() / 2, galetD.height() / 2);
  tampon.setColorDepth(16);
  if (!tampon.createSprite(TAMP_W, TAMP_H)) return false;

  // Icône play : non bloquante si absente
  playOk = loadBmpSprite(spritePlay, F_PLAY, PLAY_BLACK_TOL);
  if (playOk) {
    int cw = spritePlay.width(), ch = spritePlay.height();
    spritePlay.setPivot(cw / 2, ch / 2);
    tamponPlay.setColorDepth(16);
    playOk = tamponPlay.createSprite(cw, ch);
    if (playOk) tamponPlay.setPivot(cw / 2, ch / 2);
  }
  if (!playOk) Serial.println("play.bmp absent : pas d'icone de lecture");

#if VU_ENABLED
  vuGreen  = tft.color565(0, 220, 60);
  vuYellow = tft.color565(240, 220, 0);
  vuRed    = tft.color565(240, 40, 20);
  vuOff    = tft.color565(30, 30, 30);
  spriteVU.setColorDepth(16);
  vuOk = spriteVU.createSprite(Z_VU.w, Z_VU.h);
  if (!vuOk) Serial.println("Sprite VU impossible : pas de vu-metre");
#endif

  uiOk = true;
  Serial.println("Interface lecteur chargee (SPIFFS)");
  return true;
}

// Remet le fond (image) sur un rectangle
void restoreBg(int x, int y, int w, int h) {
  if (!bgBuf) { tft.fillRect(x, y, w, h, TFT_BLACK); return; }
  for (int r = 0; r < h; r++) tft.pushImage(x, y + r, w, 1, bgBuf + (y + r) * BG_W + x);
}

// ======================================================================
//                    POCHETTE (APIC ID3) 140x55 @ (115,220)
// ======================================================================
#define COVER_X 115
#define COVER_Y 220
#define COVER_W 140
#define COVER_H 55
#define COVER_MAX (700 * 1024)            // JPEG plus gros : ignoré (lecture SD trop longue)

#if COVER_ENABLED
uint8_t*  coverBuf = nullptr;             // JPEG brut (PSRAM), écrit par la tâche audio
volatile size_t coverLen = 0;
volatile bool   coverReady = false;       // tâche audio -> interface : nouveau JPEG disponible
SemaphoreHandle_t coverMux = nullptr;     // protège coverBuf pendant le décodage

uint16_t* coverPix = nullptr;             // image décodée RGB565 (PSRAM)
int  coverDW = 0, coverDH = 0;            // taille finale affichée
int  coverSW = 1, coverSH = 1;            // taille après échelle JPEG
bool coverValid = false;

// --- Extraction de la pochette : lecture du tag ID3v2 par nos soins (tâche audio, avant connecttoFS) ---
static uint32_t id3Synchsafe(const uint8_t* p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7)  |  (uint32_t)(p[3] & 0x7F);
}
static uint32_t id3Be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void coverExtract(const char* path) {
  if (!coverBuf || !coverMux) { Serial.println("Pochette : tampon PSRAM absent"); return; }
  File f = SD.open(path, "r");
  if (!f) { Serial.println("Pochette : ouverture impossible"); return; }

  uint8_t h[10];
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) {
    Serial.println("Pochette : pas de tag ID3v2 dans ce fichier");
    f.close(); return;
  }
  uint8_t ver = h[3], flags = h[5];
  uint32_t tagEnd = 10 + id3Synchsafe(h + 6);
  uint32_t pos = 10;
  Serial.printf("ID3v2.%u, tag de %u octets, flags 0x%02X\n", ver, (unsigned)tagEnd, flags);
  if (ver < 2 || ver > 4) { f.close(); return; }

  if ((flags & 0x40) && ver >= 3) {                       // en-tête étendu : on le saute
    uint8_t e[4];
    f.seek(pos);
    if (f.read(e, 4) == 4) pos += (ver == 4) ? id3Synchsafe(e) : (id3Be32(e) + 4);
  }
  bool unsyncAll = (flags & 0x80) != 0;
  const int hl = (ver == 2) ? 6 : 10;
  bool done = false;

  while (!done && pos + hl <= tagEnd) {
    uint8_t fh[10];
    f.seek(pos);
    if (f.read(fh, hl) != hl || fh[0] == 0) break;        // fin ou bourrage (padding)
    uint32_t fs; uint16_t ff = 0;
    if (ver == 2)      fs = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
    else if (ver == 3) fs = id3Be32(fh + 4);
    else               fs = id3Synchsafe(fh + 4);
    if (ver >= 3) ff = ((uint16_t)fh[8] << 8) | fh[9];
    if (fs == 0 || pos + hl + fs > tagEnd) break;

    bool isPic = (ver == 2) ? (memcmp(fh, "PIC", 3) == 0) : (memcmp(fh, "APIC", 4) == 0);
    if (isPic) {
      done = true;
      Serial.printf("Pochette : trame image de %u octets\n", (unsigned)fs);
      bool packed = (ver == 3) ? (ff & 0x00C0) : (ver == 4 ? (ff & 0x000C) : false);   // compression / chiffrement
      if (packed) { Serial.println("Pochette : trame compressee/chiffree, ignoree"); break; }
      if (fs < 200 || fs > COVER_MAX) { Serial.println("Pochette : taille hors limites, ignoree"); break; }
      if (xSemaphoreTake(coverMux, pdMS_TO_TICKS(300)) != pdTRUE) break;

      f.seek(pos + hl);
      size_t n = f.read(coverBuf, fs);
      bool ok = (n == fs);
      if (ok) {
        if (unsyncAll || (ver == 4 && (ff & 0x0002))) {   // désynchronisation : FF 00 -> FF
          size_t j = 0;
          for (size_t i = 0; i < n; i++) {
            coverBuf[j++] = coverBuf[i];
            if (coverBuf[i] == 0xFF && i + 1 < n && coverBuf[i + 1] == 0x00) i++;
          }
          n = j;
        }
        size_t start = 0; bool found = false, png = false;
        size_t lim = (n < 300) ? n : 300;
        for (size_t i = 0; i + 3 < lim; i++) {
          if (coverBuf[i] == 0xFF && coverBuf[i + 1] == 0xD8 && coverBuf[i + 2] == 0xFF) { start = i; found = true; break; }
          if (coverBuf[i] == 0x89 && coverBuf[i + 1] == 'P' && coverBuf[i + 2] == 'N' && coverBuf[i + 3] == 'G') { png = true; break; }
        }
        if (found) {
          if (start) memmove(coverBuf, coverBuf + start, n - start);
          coverLen = n - start;
          coverReady = true;
          Serial.printf("Pochette : JPEG de %u octets pret\n", (unsigned)coverLen);
        } else {
          Serial.println(png ? "Pochette : PNG (non gere)" : "Pochette : ni JPEG ni PNG reconnu");
        }
      } else {
        Serial.println("Pochette : lecture SD incomplete");
      }
      xSemaphoreGive(coverMux);
    }
    pos += hl + fs;
  }
  if (!done) Serial.println("Pochette : aucune trame APIC dans le tag");
  f.close();
}

// --- Sortie TJpgDec : réduction au plus proche voisin vers coverPix ---
bool coverOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (!coverPix) return 0;
  for (int r = 0; r < h; r++) {
    int dy = ((y + r) * coverDH) / coverSH;
    if (dy < 0 || dy >= coverDH) continue;
    for (int c = 0; c < w; c++) {
      int dx = ((x + c) * coverDW) / coverSW;
      if (dx < 0 || dx >= coverDW) continue;
      coverPix[dy * coverDW + dx] = bmp[r * w + c];
    }
  }
  return 1;
}

void drawCover() {
  if (!uiOk || page != PAGE_PLAYER || !coverValid) return;
  restoreBg(COVER_X, COVER_Y, COVER_W, COVER_H);
  int x = COVER_X + (COVER_W - coverDW) / 2;
  int y = COVER_Y + (COVER_H - coverDH) / 2;
  tft.pushImage(x, y, coverDW, coverDH, coverPix);
}

// Efface la pochette (changement de morceau)
void coverReset() {
  coverReady = false;
  coverValid = false;
  if (uiOk && page == PAGE_PLAYER) restoreBg(COVER_X, COVER_Y, COVER_W, COVER_H);
}

// Décodage (tâche interface)
void coverTick() {
  if (!coverReady || !uiOk || !coverBuf || !coverPix) return;
  coverReady = false;
  if (xSemaphoreTake(coverMux, pdMS_TO_TICKS(200)) != pdTRUE) return;

  uint16_t w = 0, h = 0;
  bool ok = false;
  if (TJpgDec.getJpgSize(&w, &h, coverBuf, coverLen) == JDR_OK && w > 0 && h > 0) {
    // Taille finale : tient dans la boîte, proportions conservées, pas d'agrandissement
    float f = min(1.0f, min((float)COVER_W / w, (float)COVER_H / h));
    coverDW = max(1, (int)(w * f));
    coverDH = max(1, (int)(h * f));

    // Plus grande échelle JPEG qui reste >= taille finale
    int s = 1;
    for (int t = 8; t >= 2; t /= 2)
      if (w / t >= coverDW && h / t >= coverDH) { s = t; break; }
    coverSW = max(1, w / s);
    coverSH = max(1, h / s);

    memset(coverPix, 0, COVER_W * COVER_H * 2);
   
    TJpgDec.setJpgScale(s);
    TJpgDec.setCallback(coverOutput);
    ok = (TJpgDec.drawJpg(0, 0, coverBuf, coverLen) == JDR_OK);
    TJpgDec.setCallback(bgOutput);                 // on remet le callback du fond
    TJpgDec.setJpgScale(1);
  }
  xSemaphoreGive(coverMux);

  coverValid = ok;
  if (!ok) Serial.println("Pochette : JPEG non decodable (progressif ?)");
  else drawCover();
}
#else
void coverExtract(const char*) {}
void drawCover() {}
void coverReset() {}
void coverTick() {}
#endif

// Icône play : visible pendant la lecture, fond d'origine en pause / fin
void updatePlayIcon(bool force) {
  if (!uiOk || !playOk) return;
  bool want = fileOpened && !filePaused && !fileEnded && fileResult != -1;
  if (!force && want == playIconShown) return;
  playIconShown = want;
  if (want) {
    // Fond copié dans un tampon, icône posée avec noir transparent, puis envoi à l'écran
    uint16_t* d = (uint16_t*)tamponPlay.getPointer();
    int tw = tamponPlay.width(), th = tamponPlay.height();
    for (int r = 0; r < th; r++)
      memcpy(d + r * tw, bgBuf + (PLAY_Y + r) * BG_W + PLAY_X, tw * 2);
    spritePlay.pushRotated(&tamponPlay, 0, 0);       // 0 = couleur transparente
    tamponPlay.pushSprite(PLAY_X, PLAY_Y);
  } else {
    restoreBg(PLAY_X, PLAY_Y, PLAY_W, PLAY_H);
  }
}

// Un galet : fond recopié dans le tampon, galet tourné (noir transparent), envoi à l'écran
void drawGalet(TFT_eSprite& g, int cx, int cy, float ang) {
  if (!uiOk) return;
  int zx = constrain(cx - TAMP_W / 2, 0, BG_W - TAMP_W);
  int zy = constrain(cy - TAMP_H / 2, 0, BG_H - TAMP_H);
  uint16_t* d = (uint16_t*)tampon.getPointer();
  for (int r = 0; r < TAMP_H; r++)
    memcpy(d + r * TAMP_W, bgBuf + (zy + r) * BG_W + zx, TAMP_W * 2);
  tampon.setPivot(cx - zx, cy - zy);
  g.pushRotated(&tampon, (int16_t)ang, 0);       // 0 = noir pur transparent
  tampon.pushSprite(zx, zy);
}

void drawGalets() {
  drawGalet(galetG, GL_X, GL_Y, angG);
  drawGalet(galetD, GR_X, GR_Y, angD);
}

#if VU_ENABLED
// Une rangée de segments (un canal) dans le sprite VU
void drawVURow(int y, int lit, int pk) {
  const int W = Z_VU.w, H = 15;
  for (int i = 0; i < VU_SEGS; i++) {
    int x0 = (i * W) / VU_SEGS;
    int x1 = ((i + 1) * W) / VU_SEGS - 1;          // 1 px d'écart entre segments
    int w = x1 - x0;
    if (w < 1) w = 1;
    uint16_t c;
    if (i < VU_SEGS * 62 / 100)      c = vuGreen;
    else if (i < VU_SEGS * 85 / 100) c = vuYellow;
    else                             c = vuRed;
    if (i < lit)                          spriteVU.fillRect(x0, y, w, H, c);
    else if (pk > 0 && i == pk - 1)       spriteVU.fillRect(x0, y, w, H, TFT_DARKGREY);   // crête
    else if (VU_SHOW_OFF)                 spriteVU.fillRect(x0, y, w, H, vuOff);
  }
}

// Mise à jour du VU-mètre (~30 images/s), appelée depuis fileLoop()
void updateVU() {
  if (!uiOk || !vuOk) return;
  uint32_t now = millis();
  if (now - vuLast < 33) return;
  vuLast = now;

  bool playing = fileOpened && fileResult == 1 && !filePaused && !fileEnded;
  uint16_t raw = playing ? vuRaw : 0;
  float tl = (float)(raw >> 8);
  float tr = (float)(raw & 0xFF);

#if VU_FULL_SCALE > 0
  vuRefL = (float)VU_FULL_SCALE;
  vuRefR = (float)VU_FULL_SCALE;
#else
  vuRefL = max(16.0f, max(vuRefL * 0.9998f, tl));   // gain automatique, décroissance lente
  vuRefR = max(16.0f, max(vuRefR * 0.9998f, tr));
#endif

  float nl = constrain(tl / vuRefL, 0.0f, 1.0f);
  float nr = constrain(tr / vuRefR, 0.0f, 1.0f);
  vuL = (nl > vuL) ? nl : max(0.0f, vuL - 0.07f);   // montée immédiate, descente progressive
  vuR = (nr > vuR) ? nr : max(0.0f, vuR - 0.07f);

  int litL = (int)(vuL * VU_SEGS + 0.5f);
  int litR = (int)(vuR * VU_SEGS + 0.5f);

  if (litL >= vuPkL) { vuPkL = litL; vuHoldL = 12; } else if (vuHoldL > 0) vuHoldL--; else if (vuPkL > 0) vuPkL--;
  if (litR >= vuPkR) { vuPkR = litR; vuHoldR = 12; } else if (vuHoldR > 0) vuHoldR--; else if (vuPkR > 0) vuPkR--;

  bool idle = (litL == 0 && litR == 0 && vuPkL == 0 && vuPkR == 0);
  if (idle && vuIdleShown) return;                  // au repos : dessiné une seule fois
  vuIdleShown = idle;

  // Fond copié dans le sprite, segments dessinés, puis envoi à l'écran d'un seul bloc
  uint16_t* d = (uint16_t*)spriteVU.getPointer();
  for (int r = 0; r < Z_VU.h; r++)
    memcpy(d + r * Z_VU.w, bgBuf + (Z_VU.y + r) * BG_W + Z_VU.x, Z_VU.w * 2);
  drawVURow(2, litL, vuPkL);
  drawVURow(19, litR, vuPkR);
  spriteVU.pushSprite(Z_VU.x, Z_VU.y);
}
#else
void updateVU() {}
#endif

void drawFileFields() {
#if SHOW_INFO
  scrollSet(scFArtist, fArtist);
  scrollSet(scFTitle, fTitle);
#endif
}

void drawFileProgress() {
#if SHOW_INFO
  // Valeurs mesurées par la tâche audio : aucun accès à la carte SD depuis ici
  uint32_t cur = (fileResult == 1) ? audCur : 0;
  uint32_t dur = (fileResult == 1) ? audDur : 0;
  if (fileEnded && dur) cur = dur;
  if (!fpForce && cur == fpLastCur && dur == fpLastDur) return;
  fpForce = false;
  fpLastCur = cur;
  fpLastDur = dur;

  float fr = dur ? min(1.0f, (float)cur / (float)dur) : 0.0f;

  // Toute la bande est restaurée depuis le fond, puis : temps | barre | durée
  restoreBg(Z_PROG.x, Z_PROG.y, Z_PROG.w, Z_PROG.h);

  int barX = Z_PROG.x + PROG_TXT_W + 4;
  int barW = Z_PROG.w - 2 * (PROG_TXT_W + 4);
  int barY = Z_PROG.y + (Z_PROG.h - PROG_BAR_H) / 2;
  int fw = (int)(barW * fr);
  if (fw > 0) tft.fillRect(barX, barY, fw, PROG_BAR_H, COL_FBAR);

  char t1[16], t2[16];
  fmtTime(t1, sizeof(t1), cur);
  if (dur) fmtTime(t2, sizeof(t2), dur); else strlcpy(t2, "--:--", sizeof(t2));
  int cy = Z_PROG.y + Z_PROG.h / 2;
  tft.setTextSize(1);
  tft.setTextColor(TFT_BLACK);               // fond transparent
  tft.setTextDatum(ML_DATUM);
  tft.drawString(t1, Z_PROG.x, cy, 2);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(t2, Z_PROG.x + Z_PROG.w, cy, 2);
#endif
}

void fileShow() {                            // éléments variables (le fond n'est pas redessiné)
  if (page != PAGE_PLAYER) return;
  drawFileFields();
  fpForce = true;
  drawFileProgress();
  drawGalets();
  updatePlayIcon(true);
  drawCover();                               // pochette (si déjà décodée)
}

void redrawFile() {                          // écran complet
  if (bgBuf) tft.pushImage(0, 0, BG_W, BG_H, bgBuf);
  else       tft.fillScreen(TFT_BLACK);
  playIconShown = false;
  vuIdleShown = false;                       // la zone VU a été effacée par le fond : à redessiner
  fileShow();
}

// Ouvre un fichier : la lecture démarre dans la tâche audio
void fileOpen(int idx) {
  if (idx < 0 || idx >= recCount) return;
  fileIdx = idx;
  coverReset();                              // efface l'ancienne pochette

  fileNameParts(recList[idx].name, fArtist, fTitle);
  snprintf(playFilePath, sizeof(playFilePath), "/rec/%s", recList[idx].name);

  fileOpened = true;
  filePaused = false;
  fileEnded = false;
  fileFailShown = false;
  fileResult = 0;
  fileRunMs = millis();
  fpForce = true;
  playFileReq = true;                        // en dernier : la tâche audio lit les variables ci-dessus
}

void goFile(int idx) {
  if (idx < 0 || idx >= recCount) return;
  if (!uiLoad()) {
    toast("Fond / galets manquants (SPIFFS)");
    drawList();
    return;
  }
  shuffleMode = false;                       // choix manuel : lecture dans l'ordre
  fileOpen(idx);
  angG = 0; angD = 0;
  page = PAGE_PLAYER;
  redrawFile();
}

// Retour à la liste : on attend l'arrêt réel de l'audio avant de toucher à la SD
void leaveFile() {
  fileOpened = false;
  filePaused = false;
  shuffleMode = false;
  playFileReq = false;
  stopReq = true;
  uint32_t t0 = millis();
  while (stopReq && millis() - t0 < 2000) delay(5);

  page = PAGE_LIST;
  redrawList();
}

// Appelé à chaque tour sur la page lecteur
void fileLoop() {
  uint32_t now = millis();

  if (fileOpened) {
    if (fileResult == -1 && !fileFailShown) {
      fileFailShown = true;
#if SHOW_INFO
      scrollSet(scFTitle, "Fichier illisible");
#endif
    } else if (fileResult == 1 && !filePaused && !fileEnded) {
      if (audio.isRunning()) fileRunMs = now;
      else if (now - fileRunMs > 2500) {                 // fin du fichier
        if (shuffleMode && recCount > 0) {               // aléatoire : on enchaîne au hasard
          fileOpen(randomIdx());
          fileShow();
        } else if (fileIdx + 1 < recCount) {
          fileOpen(fileIdx + 1);
          fileShow();
        } else {
          fileEnded = true;
          fpForce = true;
          drawFileProgress();
        }
      }
    }
  }

  updatePlayIcon(false);                                 // icône play / fond d'origine selon l'état

#if SHOW_INFO
  scrollTick(scFArtist);
  scrollTick(scFTitle);
#endif

  coverTick();                                           // pochette : décodage + affichage dès qu'elle arrive
  updateVU();                                            // vu-mètre

  // Rotation des galets : uniquement pendant la lecture
  static uint32_t lastAnim = 0;
  if (now - lastAnim >= GALET_MS) {
    lastAnim = now;
    if (fileResult == 1 && !filePaused && !fileEnded) {
      angG += VIT_GAUCHE; if (angG < 0) angG -= 360;
      angD += VIT_DROIT;  if (angD < 0) angD -= 360;
//      angG += VIT_GAUCHE; if (angG >= 360) angG -= 360;
//      angD += VIT_DROIT;  if (angD >= 360) angD -= 360;
      drawGalets();
    }
  }

  static uint32_t lastProg = 0;
  if (now - lastProg >= 500) { lastProg = now; drawFileProgress(); }
}

// ======================================================================
//                          GESTION DU TOUCHER
// ======================================================================
void volumeChanged() {
  volDirty = true;
  volSaveAt = millis() + 2000;
}

void handleTouch() {
  static uint32_t last = 0;
  static bool waitRelease = false;

  uint16_t x = 0, y = 0;
  int n = readTouch(x, y);

  // --- Boîte de confirmation ---
  if (confirmAct) {
    if (n == 0) { waitRelease = false; return; }
    if (waitRelease || n != 1) return;
    if (y >= 190 && y < 240) {
      if (x >= 90 && x < 230) {                        // Oui
        recDelete(confirmIdx);
        confirmAct = false; waitRelease = true;
        if (topIdx >= recCount) topIdx = max(0, recCount - ROWS);
        redrawList();
      } else if (x >= 250 && x < 390) {                // Non
        confirmAct = false; waitRelease = true;
        redrawList();
      }
    }
    return;
  }

  // --- Page lecteur ---
  if (page == PAGE_PLAYER) {
    if (n == 0) { waitRelease = false; return; }
    if (waitRelease) return;
    if (n >= 2) {                                      // 2 doigts : retour à la liste
      waitRelease = true;
      leaveFile();
      return;
    }
    if (millis() - last < 250) return;
    last = millis();

    if (inZone(x, y, Z_VMOINS)) {                      // volume : répétition si on maintient
      volume = max(0, volume - 1);  volumeChanged();
    } else if (inZone(x, y, Z_VPLUS)) {
      volume = min(21, volume + 1); volumeChanged();
    } else if (inZone(x, y, Z_PLAY)) {                 // lecture / pause
      waitRelease = true;
      if (fileEnded) { fileOpen(fileIdx); fileShow(); }
      else if (fileResult == 1) {
        pauseReq = true;
        filePaused = !filePaused;
      }
    } else if (inZone(x, y, Z_PREV)) {                 // retour : précédent
      waitRelease = true;
      if (fileIdx > 0) { fileOpen(fileIdx - 1); fileShow(); }
    } else if (inZone(x, y, Z_NEXT)) {                 // suivant (au hasard en mode aléatoire)
      waitRelease = true;
      if (shuffleMode && recCount > 0) { fileOpen(randomIdx()); fileShow(); }
      else if (fileIdx + 1 < recCount) { fileOpen(fileIdx + 1); fileShow(); }
    } else if (inZone(x, y, Z_RAND)) {                 // musique : lecture aléatoire
      waitRelease = true;
      if (recCount > 0) {
        shuffleMode = true;
        fileOpen(randomIdx());
        fileShow();
      }
    } else if (inZone(x, y, Z_LIST)) {                 // retour à la liste
      waitRelease = true;
      leaveFile();
    }
    return;
  }

  // --- Page liste : tap court = lecture, appui long = suppression ---
  if (n >= 2) pressActive = false;
  if (n == 0 && pressActive) {                         // relâché avant 0,7 s
    pressActive = false;
    waitRelease = false;
    if (pressIdx >= 0 && pressIdx < recCount) goFile(pressIdx);
    return;
  }
  if (n == 0) { waitRelease = false; return; }
  if (waitRelease) return;

  if (pressActive) {
    if (millis() - pressT0 >= 700) {                   // appui long détecté
      pressActive = false;
      waitRelease = true;
      confirmIdx = pressIdx;
      confirmAct = true;
      drawConfirm();
    }
    return;
  }
  if (n != 1) return;

  if (y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
    int idx = topIdx + (y - LIST_Y) / ROW_H;
    if (idx < recCount) { pressActive = true; pressT0 = millis(); pressIdx = idx; }
    return;
  }

  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    if (millis() - last < 250) return;
    last = millis();
    int k = ((int)x - BTN_GAP) / (BTN_W + BTN_GAP);
    if (x < BTN_GAP || k < 0 || k > 2) return;
    if (k == 0) topIdx = max(0, topIdx - ROWS);
    else if (k == 1) { if (topIdx + ROWS < recCount) topIdx += ROWS; }
    else { recScan(); topIdx = 0; }
    drawList();
  }
}

// ======================================================================
//                           BOUCLE INTERFACE
// ======================================================================
void uiLoop() {
  handleTouch();

  if (toastUntil && millis() >= toastUntil) {
    toastUntil = 0;
    if (page == PAGE_LIST && !confirmAct) drawList();
  }

  if (page == PAGE_PLAYER) fileLoop();

  if (volSaveAt && millis() >= volSaveAt) {            // volume mémorisé (sans écriture flash à chaque appui)
    volSaveAt = 0;
    prefs.begin("lecteur", false);
    prefs.putInt("vol", volume);
    prefs.end();
  }

  delay(10);
}

void uiTask(void*) {
  for (;;) uiLoop();
}

// ---- Écran de démarrage (SPIFFS /initial.jpg) ----
bool splashOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

void afficherSplash() {
  TJpgDec.setCallback(splashOutput);          // sortie directe à l'écran
  TJpgDec.drawFsJpg(0, 0, "/initial.jpg", SPIFFS);
  delay(5000);
//  tft.fillScreen(TFT_BLACK);
  TJpgDec.setCallback(bgOutput);              // on remet le callback du fond (PSRAM)
}

// ======================================================================
//                              SETUP / LOOP
// ======================================================================
void setup() {
  Serial.begin(115200);
  Serial.printf("PSRAM libre : %u octets\n", (unsigned)ESP.getFreePsram());

  prefs.begin("lecteur", true);
  volume = constrain(prefs.getInt("vol", 6), 0, 21);
  prefs.end();

  tft.init();
  u8f.begin(tft);
  u8f.setFontMode(1);
  tft.setRotation(1);                     // paysage en permanence (480x320)
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(bgOutput);
  touchInit();

  if (!SPIFFS.begin(true)) Serial.println("Erreur SPIFFS !");

  afficherSplash();                       // initial.jpg pendant 5 s

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Chargement...", SCR_W / 2, SCR_H / 2, 4);

  sdInit();
  
  recList = (RecEntry*)heap_caps_malloc(MAX_REC * sizeof(RecEntry), MALLOC_CAP_SPIRAM);
  if (!recList) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Erreur memoire (PSRAM)", SCR_W / 2, SCR_H / 2, 4);
    while (true) delay(1000);
  }

#if SHOW_INFO
  scrollInit(scFArtist, Z_ARTIST.x, Z_ARTIST.y, Z_ARTIST.w, Z_ARTIST.h, COL_FARTIST, TFT_BLACK, 1, 1);
  scrollInit(scFTitle,  Z_TITLE.x,  Z_TITLE.y,  Z_TITLE.w,  Z_TITLE.h,  COL_FTITLE,  TFT_BLACK, 1, 1);
  scFArtist.bgImg = true;                 // le fond est restauré depuis fond.jpg
  scFTitle.bgImg  = true;
#endif

#if COVER_ENABLED
  coverMux = xSemaphoreCreateMutex();
  coverBuf = (uint8_t*)heap_caps_malloc(COVER_MAX, MALLOC_CAP_SPIRAM);
  coverPix = (uint16_t*)heap_caps_malloc(COVER_W * COVER_H * 2, MALLOC_CAP_SPIRAM);
  if (!coverBuf || !coverPix) Serial.println("PSRAM insuffisante pour la pochette");
#endif

  uiLoad();                               // fond + galets + play (réessayé à l'ouverture d'un fichier si échec)

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(volume);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 1);

  recScan();
  page = PAGE_LIST;
  redrawList();

#if UI_ON_CORE0
  xTaskCreatePinnedToCore(uiTask, "ui", 16384, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
#if UI_ON_CORE0
  vTaskDelete(nullptr);                   // l'interface tourne dans sa propre tâche (coeur 0)
#else
  uiLoop();
#endif
}
