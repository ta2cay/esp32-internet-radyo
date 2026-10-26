// İnternet radyosu: Wi-Fi → MP3/AAC yayını → MAX98357A (I2S) → 4Ω hoparlör
// İstasyonlar kategorilere ayrılmış olarak LittleFS'teki /stations.json'dan okunur
// (data/stations.json; yükle: pio run -t uploadfs). Dosya yoksa koddaki yedek liste çalar.
// Encoder tek kuralla çalışır: ÇEVİR = seç, BAS = tamam (uzun basma yok).
//   Çalarken: çevir = aynı kategoride istasyon değiştir (1 s durunca çalar), bas = menüyü aç.
//   Menü: ◀ Kapat, ★ Favoriye ekle/çıkar (çalan istasyon), Favoriler, Son Dinlenenler, kategoriler.
//   İstasyon listesi: ilk satır ◀ Geri; istasyona basınca çalar ve menü kapanır.
//   Menüde 10 s işlem olmazsa kapanır.
// Pot (G35) sesi ayarlar. Seri port: 'n' sonraki, 'p' önceki, 'c' sonraki kategori, 'f' favori.
// ILI9341 2.8" ekran: saat/tarih/Wi-Fi, istasyon kartı, çalan şarkı + spektrum çubukları, ses,
// iç kart (sıcaklık / nem / bugünkü dinleme süresi ve şarkı sayısı), dış kart (Open-Meteo: şu an + 3 gün tahmin).
// Fontlar src/fonts.h'de (tools/make_fonts.py üretir), Türkçe harf içerir.
// Son çalan istasyonun adresi NVS'te saklanır, açılışta oradan devam edilir.
// STATION_SCAN tanımlıysa açılışta her istasyon denenir ve çalışanlar raporlanır.
#include <algorithm>
#include <vector>
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <TFT_eSPI.h>
#include <DHT.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "soc/rtc_cntl_reg.h"
#include "secrets.h"
#include "fonts.h"

#define STATION_SCAN 0
#define USE_POT 1  // pot takılınca 1 yap; 0 iken ses VOL_FIXED'de sabit

namespace {
constexpr uint8_t I2S_BCLK = 14;
constexpr uint8_t I2S_LRC  = 13;
constexpr uint8_t I2S_DOUT = 25;  // MAX98357A DIN
constexpr uint8_t ENC_CLK  = 32;
constexpr uint8_t ENC_DT   = 33;
constexpr uint8_t ENC_SW   = 27;  // KY-040 düğmesi, basınca LOW
constexpr uint8_t POT      = 35;  // ADC1; Wi-Fi açıkken ADC2 pinleri okunamaz
constexpr uint8_t DHT_PIN  = 26;
constexpr uint8_t VOL_MAX  = 21;  // kütüphanenin üst sınırı
constexpr uint8_t VOL_FIXED = 10; // pot yokken orta seviye
constexpr uint8_t BL_CH    = 7;   // arka ışık PWM kanalı (TFT_BL = G4)

// Dış hava: Çaycuma. Düz HTTP: yayın zaten bir TLS bağlantısı tutuyor, ikincisine RAM yetmeyebilir.
constexpr char WEATHER_URL[] =
    "http://api.open-meteo.com/v1/forecast?latitude=41.4264&longitude=32.0756"
    "&current=temperature_2m,weather_code,is_day"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=4&timezone=Europe%2FIstanbul";
constexpr char TZ_TR[] = "<+03>-3";  // Türkiye: sabit UTC+3
constexpr uint8_t NIGHT_FROM = 23, NIGHT_TO = 7, BL_DAY = 255, BL_NIGHT = 40;

struct Station { String name, url, city; };  // city: isteğe bağlı, kartta gösterilir
struct Category { String name; std::vector<Station> st; uint16_t color; };
std::vector<Category> cats;

// stations.json okunamazsa çalınacak liste
const char* const FALLBACK[][2] = {
  {"Power Türk",   "https://listen.powerapp.com.tr/powerturk/mpeg/icecast.audio"},
  {"Power FM",     "https://listen.powerapp.com.tr/powerfm/mpeg/icecast.audio"},
  {"Fenomen Türk", "https://live.radyofenomen.com/fenomenturk/128/icecast.audio"},
  {"Fenomen",      "https://live.radyofenomen.com/fenomen/128/icecast.audio"},
  {"Süper FM",     "https://playerservices.streamtheworld.com/api/livestream-redirect/SUPER_FM.mp3"},
  {"Metro FM",     "https://playerservices.streamtheworld.com/api/livestream-redirect/METRO_FM.mp3"},
  {"Joy Türk",     "https://playerservices.streamtheworld.com/api/livestream-redirect/JOY_TURK.mp3"},
  {"Virgin Radio", "https://playerservices.streamtheworld.com/api/livestream-redirect/VIRGIN_RADIO.mp3"},
  {"Slow Türk",    "https://radyo.duhnet.tv/slowturk"},
  {"Swiss Jazz",   "http://stream.srg-ssr.ch/m/rsj/mp3_128"},
};

// 0xRRGGBB → RGB565
constexpr uint16_t RGB(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }
constexpr uint16_t C_BG = RGB(0x0A0E14), C_PANEL = RGB(0x151B24), C_LINE = RGB(0x232B38),
                   C_TEXT = RGB(0xE9EDF2), C_MUTED = RGB(0x8592A6), C_DIM = RGB(0x3A4556),
                   C_WARN = RGB(0xFFC145), C_ERR = RGB(0xFF5C5C), C_OK = RGB(0x3DDC97);

// Kategori rengi adından: Rock kırmızı, Slow pembe, Haber mavi... Eşleşmeyen sıradaki renkten alır.
uint16_t colorFor(String name, int i) {
  static const struct { const char* key; uint32_t rgb; } MAP[] = {
    {"haber", 0x4DA3FF}, {"rock", 0xFF5A5F}, {"jazz", 0xA78BFA}, {"klasik", 0xA78BFA},
    {"arabesk", 0xFFB020}, {"slow", 0xF472B6}, {"spor", 0x34D399}, {"sanat", 0xFB923C},
    {"halk", 0xFB923C}, {"yabanc", 0x2DD4BF}, {"pop", 0x22D3EE},
  };
  static const uint32_t SPARE[] = {0x60A5FA, 0xF87171, 0xC084FC, 0xFBBF24, 0x4ADE80, 0xF472B6};
  name.toLowerCase();
  for (auto& m : MAP)
    if (name.indexOf(m.key) >= 0) return RGB(m.rgb);
  return RGB(SPARE[i % 6]);
}

// Biçim: {"categories":[{"name":"Haber","stations":[{"name":"..","url":".."}]}]}. Boş kategoriler atlanır.
void loadStations() {
  if (LittleFS.begin(false)) {
    File f = LittleFS.open("/stations.json", "r");
    JsonDocument doc;
    if (f && !deserializeJson(doc, f)) {
      for (JsonObject c : doc["categories"].as<JsonArray>()) {
        Category cat{c["name"] | "?", {}, 0};
        for (JsonObject s : c["stations"].as<JsonArray>())
          cat.st.push_back({s["name"] | "?", s["url"] | "", s["city"] | ""});
        if (!cat.st.empty()) cats.push_back(std::move(cat));
      }
    }
    f.close();
  }
  if (cats.empty()) {
    Serial.println("[liste] stations.json yok/bozuk, yedek liste kullaniliyor");
    Category c{"Radyo", {}, 0};
    for (auto& s : FALLBACK) c.st.push_back({s[0], s[1], ""});
    cats.push_back(std::move(c));
  }
  size_t total = 0;
  for (int i = 0; i < (int)cats.size(); i++) {
    cats[i].color = colorFor(cats[i].name, i);
    total += cats[i].st.size();
  }
  Serial.printf("[liste] %u kategori, %u istasyon\n", (unsigned)cats.size(), (unsigned)total);
}

Audio audio;
TFT_eSPI tft;
TFT_eSprite sprA(&tft), sprB(&tft);  // şarkı satırları; kayan yazı titremesin diye ekran dışında çizilir
DHT dht(DHT_PIN, DHT11);
Preferences prefs;
// Çalan istasyon (cat/idx), encoder'la seçilmiş ama henüz çalmayan (selCat/selIdx).
int cat = 0, idx = 0, selCat = 0, selIdx = 0;
std::vector<int> lastIdxOfCat;  // kategoriye dönünce en son seçilen istasyondan devam
volatile bool gotFormat = false;  // audio_info'da çözücü/BitRate görülünce yayın gerçekten çözülüyor demektir
volatile bool streamFailed = false;  // bağlantı hatası ya da HTTP 4xx/5xx; tarama beklemeden geçer

// Yayın durumu: bağlanıyor → çalıyor, ya da 15 s içinde çözülemezse/hata gelirse yok.
enum class Net { Connecting, Playing, Failed };
Net stream = Net::Connecting;
uint32_t playStarted = 0;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

// ---- Favoriler ve son dinlenenler ----
// NVS "favs" ve "recent": adresler '\n' ile ayrılmış (adresle saklanır, liste düzenlenince kaymaz).
// Son dinlenenlerde en yeni başta, en fazla RECENT_MAX kayıt.
constexpr int RECENT_MAX = 10;
String favs, recents;
bool isFav(const String& url) { return favs.indexOf(url + "\n") >= 0; }
void toggleFav(const String& url) {
  if (isFav(url)) favs.replace(url + "\n", "");
  else favs += url + "\n";
  prefs.putString("favs", favs);
}

void pushRecent(const String& url) {
  const String key = url + "\n";
  if (recents.startsWith(key)) return;  // zaten en üstte; NVS'e boşuna yazma
  recents.replace(key, "");
  recents = key + recents;
  int n = 0, pos = -1;
  while ((pos = recents.indexOf('\n', pos + 1)) >= 0)
    if (++n == RECENT_MAX) { recents.remove(pos + 1); break; }
  prefs.putString("recent", recents);
}

int countLines(const String& s) {
  int n = 0;
  for (char ch : s) n += ch == '\n';
  return n;
}

// ---- Dış hava (core 0'da ayrı görev; HTTP beklerken ses ve ekran donmaz) ----
// days: yarından başlayarak 3 günlük tahmin (wday 0 = Pazar)
struct DayWx { int code, wday; float hi, lo; };
struct Outdoor { bool valid = false; float t = NAN; int code = 0; bool day = true; DayWx days[3]; int nDays = 0; };
Outdoor outdoor;
volatile uint32_t outdoorVer = 0;
portMUX_TYPE wxMux = portMUX_INITIALIZER_UNLOCKED;

void weatherTask(void*) {
  for (;;) {
    uint32_t wait = 60000;
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(8000);
      http.useHTTP10(true);  // chunked gelmesin, akış doğrudan JSON'a verilebilsin
      if (http.begin(client, WEATHER_URL) && http.GET() == HTTP_CODE_OK) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getStream())) {
          JsonObject cur = doc["current"];
          Outdoor o;
          o.t = cur["temperature_2m"] | NAN;
          o.code = cur["weather_code"] | 0;
          o.day = (cur["is_day"] | 1) == 1;
          o.valid = !isnan(o.t);
          JsonObject daily = doc["daily"];  // [0] bugün, 1..3 sonraki günler
          for (int i = 1; i <= 3; i++) {
            const char* date = daily["time"][i] | "";
            struct tm d = {};
            if (sscanf(date, "%d-%d-%d", &d.tm_year, &d.tm_mon, &d.tm_mday) != 3) break;
            d.tm_year -= 1900;
            d.tm_mon -= 1;
            d.tm_hour = 12;
            mktime(&d);  // tm_wday'i doldurur
            DayWx& w = o.days[o.nDays];
            w.code = daily["weather_code"][i] | 0;
            w.hi = daily["temperature_2m_max"][i] | NAN;
            w.lo = daily["temperature_2m_min"][i] | NAN;
            w.wday = d.tm_wday;
            if (isnan(w.hi) || isnan(w.lo)) break;
            o.nDays++;
          }
          if (o.valid) {
            portENTER_CRITICAL(&wxMux);
            outdoor = o;
            outdoorVer++;
            portEXIT_CRITICAL(&wxMux);
            wait = 15UL * 60 * 1000;
            Serial.printf("[hava] %.1f C kod %d, tahmin %d gun\n", o.t, o.code, o.nDays);
          }
        }
      } else {
        Serial.println("[hava] alinamadi");
      }
      http.end();
    }
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

// ---- Metin yardımcıları ----
// Fontta bulunan ASCII dışı harfler (tools/make_fonts.py TEXT ile aynı olmalı)
const char16_t EXTRA[] = u"ÇçĞğİıÖöŞşÜüÂâÎîÛûÉéÁáÀàÈèÄäËëÍíÑñÓóÚú°·–—‘’“”…";

bool inFont(uint32_t cp) {
  if (cp >= 0x20 && cp < 0x7F) return true;
  for (const char16_t* p = EXTRA; *p; p++)
    if (*p == cp) return true;
  return false;
}

void putUtf8(char*& o, const char* end, uint32_t cp) {
  if (cp < 0x80 && o + 1 <= end) *o++ = cp;
  else if (cp < 0x800 && o + 2 <= end) { *o++ = 0xC0 | (cp >> 6); *o++ = 0x80 | (cp & 0x3F); }
  else if (o + 3 <= end) { *o++ = 0xE0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 0x3F); *o++ = 0x80 | (cp & 0x3F); }
}

// Yayın başlıkları bazen UTF-8, bazen ISO-8859-9 (Türkçe Latin-5) geliyor; ikisi de çözülür.
// Fontta olmayan karakterler (emoji vb.) atlanır, yoksa ekranda kutu çıkar.
void cleanText(const char* in, char* out, size_t n) {
  char* o = out;
  const char* end = out + n - 1;
  const uint8_t* p = (const uint8_t*)in;
  while (*p) {
    uint32_t cp;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80) {
      p++;
      while ((*p & 0xC0) == 0x80) p++;
      continue;
    } else {
      cp = *p++;
      switch (cp) {  // Latin-5'in Latin-1'den farklı harfleri
        case 0xD0: cp = 0x11E; break; case 0xF0: cp = 0x11F; break;
        case 0xDD: cp = 0x130; break; case 0xFD: cp = 0x131; break;
        case 0xDE: cp = 0x15E; break; case 0xFE: cp = 0x15F; break;
      }
    }
    if (inFont(cp)) putUtf8(o, end, cp);
  }
  *o = 0;
}

// Genişliğe sığmayan metnin sonu "…" ile kesilir (yüklü fonta göre ölçülür).
String fitText(TFT_eSPI& d, String s, int maxW) {
  if (d.textWidth(s) <= maxW) return s;
  while (s.length() && d.textWidth(s + "…") > maxW) {
    int i = s.length() - 1;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;  // UTF-8 karakterin başına git
    s.remove(i);
  }
  s.trim();
  return s + "…";
}

// ---- Ekran ----
// Dikey 240x320. Yalnızca değişen bölge yeniden çizilir; tam ekran çizim
// audio.loop()'u geciktirip sese takılma yapar. Her döngüde en fazla bir bölge çizilir.
constexpr int SW = 240, SH = 320;
constexpr int Y_STATION = 30, Y_SONG = 114, Y_VOL = 192, Y_CARDS = 220;
constexpr int SPR_W = 224, SPR_X = 8, SPR_A_Y = 138, SPR_B_Y = 166, SCROLL_GAP = 48;

const uint8_t* curFont = nullptr;
void useFont(const uint8_t* f) {
  if (curFont == f) return;
  tft.loadFont(f);  // önceki fontu kendisi bırakır
  curFont = f;
}

void text(const String& s, int x, int y, uint16_t fg, uint16_t bg, uint8_t datum = TL_DATUM) {
  tft.setTextColor(fg, bg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, y);
}

char title[128] = "";
float tempC = NAN, hum = NAN;
int shownVol = 0;
// Kartlar 4 s'de bir sayfa değiştirir. İçeri: 0 sıcaklık, 1 nem, 2 bugünkü dinleme.
// Dışarı: 0 şu an (8 s), 1.. tahmin günleri.
int indoorPage = 0, outdoorPage = 0;
uint32_t lastIndoorFlip = 0, lastOutdoorFlip = 0;

// ---- Bugünkü dinleme: süre ve şarkı sayısı ----
// NVS "sday" (yıl*1000 + yılın günü), "ssec", "ssong". Gün değişince sıfırlanır, 5 dk'da bir kaydedilir.
uint32_t statDay = 0, listenMs = 0, songCount = 0, savedSec = 0;
char lastSong[128] = "";

void saveStats() {
  savedSec = listenMs / 1000;
  prefs.putUInt("sday", statDay);
  prefs.putUInt("ssec", savedSec);
  prefs.putUInt("ssong", songCount);
}

void checkStatDay(const struct tm& t) {
  const uint32_t day = (t.tm_year + 1900) * 1000 + t.tm_yday;
  if (day == statDay) return;
  statDay = day;
  listenMs = songCount = 0;
  lastSong[0] = 0;
  saveStats();
}

// ---- Spektrum çubukları (VU) ----
// Çözülen ses, kesim frekansları yarıya inen 7 alçak geçiren süzgeçle 8 banda ayrılır
// (~7 kHz, 2.8 kHz, 1.4 kHz ... 90 Hz). Süzgeçler tek kutuplu ve kaba ama göstermelik iş için yeterli.
// Örnekler audio_process_extern'de toplanır, ekran 50 ms'de bir ortalamayı çizer.
constexpr int VU_BANDS = 8;
int32_t vuLp[VU_BANDS - 1];
uint32_t vuAcc[VU_BANDS];
uint32_t vuN = 0;
float vuCeil[VU_BANDS], vuLevel[VU_BANDS], vuPeak[VU_BANDS];
int vuShownH[VU_BANDS], vuShownP[VU_BANDS];

void vuFeed(const int16_t* b, uint16_t frames, uint8_t ch) {
  for (uint16_t f = 0; f < frames; f++) {
    const int32_t x = (ch == 2 ? (b[2 * f] + b[2 * f + 1]) / 2 : b[f]) * 256;
    int32_t hi = x;
    for (int k = 0; k < VU_BANDS - 1; k++) {
      vuLp[k] += (x - vuLp[k]) >> (k + 1);
      vuAcc[VU_BANDS - 1 - k] += abs(hi - vuLp[k]) >> 8;  // bant k: bir önceki süzgeçle bu süzgeç arası
      hi = vuLp[k];
    }
    vuAcc[0] += abs(hi) >> 8;  // en kalın bant
  }
  vuN += frames;
}
// Menü: ana menü ya da bir listenin istasyonları; imleç listCursor.
// Ana menü satırları: 0 Kapat, 1 Favoriye ekle/çıkar, 2 Favoriler, 3 Son Dinlenenler,
// ayırıcı çizgiden sonra 4.. gerçek kategoriler.
// İstasyon listesi: satır 0 Geri, sonra items; menuCat >= 0 gerçek kategori, LIST_FAV ya da LIST_RECENT.
enum class Menu { None, Main, Stations };
Menu menu = Menu::None;
constexpr int LIST_FAV = -1, LIST_RECENT = -2;
constexpr int M_CLOSE = 0, M_FAVADD = 1, M_FAVS = 2, M_RECENT = 3, CAT_OFS = 4;
struct Ref { int c, i; };
std::vector<Ref> items;  // açık istasyon listesinin satırları (Geri hariç)
int menuCat = 0, listCursor = 0;
uint32_t listTouched = 0;
int mainEntries() { return cats.size() + CAT_OFS; }

char toastMsg[40] = "";  // istasyon kartının bilgi satırında kısa süreli mesaj
uint32_t toastUntil = 0;
char info[40] = "";
int bufPct = -1, wifiLevel = -1, shownMinute = -1;

bool dStatus = true, dStation = true, dInfo = true, dSong = true, dVol = true,
     dIndoor = true, dOutdoor = true, dMenu = false;

int rssiLevel() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  const int r = WiFi.RSSI();
  return r > -55 ? 4 : r > -65 ? 3 : r > -75 ? 2 : 1;
}

// Üst bar: solda saat, ortada tarih, sağda Wi-Fi çubukları; altında kategori renginde ince çizgi.
void drawStatus() {
  static const char* const DAYS[] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
  static const char* const MONTHS[] = {"Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran", "Temmuz",
                                       "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"};
  tft.fillRect(0, 0, SW, 26, C_BG);
  struct tm t;
  const bool synced = getLocalTime(&t, 0);
  char buf[32];
  useFont(F_BOLD);
  if (synced) snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  else strcpy(buf, "--:--");
  text(buf, 8, 5, C_TEXT, C_BG);
  if (synced) {
    snprintf(buf, sizeof(buf), "%d %s %s", t.tm_mday, MONTHS[t.tm_mon], DAYS[t.tm_wday]);
    useFont(F_SMALL);
    text(buf, SW / 2 + 6, 5, C_MUTED, C_BG, TC_DATUM);
  }
  for (int i = 0; i < 4; i++) {  // Wi-Fi gücü
    const int h = 4 + i * 3, x = 214 + i * 6;
    const uint16_t c = wifiLevel == 0 ? C_ERR : i < wifiLevel ? C_TEXT : C_DIM;
    tft.fillRect(x, 18 - h, 4, h, c);
  }
  tft.fillRect(0, 25, SW, 2, cats[selCat].color);
}

void drawStar(int cx, int cy, int r, uint16_t c) {
  float px[10], py[10];
  for (int i = 0; i < 10; i++) {
    const float a = -PI / 2 + i * PI / 5, rr = i % 2 ? r * 0.45f : r;
    px[i] = cx + cosf(a) * rr;
    py[i] = cy + sinf(a) * rr;
  }
  for (int i = 0; i < 10; i++) {
    const int j = (i + 1) % 10;
    tft.fillTriangle(cx, cy, px[i], py[i], px[j], py[j], c);
  }
}

// İstasyon kartı: solda kategori renginde şerit, büyük ad; altında kategori, sıra ve şehir.
// Seçili istasyon henüz çalmıyorsa (encoder'la gezilirken) ad sarı gösterilir.
constexpr int CARD_X = 20;
void drawStation() {
  tft.fillRect(0, Y_STATION, SW, 58, C_BG);
  const Category& c = cats[selCat];
  const Station& s = c.st[selIdx];
  const bool preview = selCat != cat || selIdx != idx;
  tft.fillRoundRect(8, Y_STATION + 6, 4, 72, 2, c.color);
  useFont(F_NAME);
  const bool fav = isFav(s.url);
  text(fitText(tft, s.name, fav ? SW - CARD_X - 30 : SW - CARD_X - 8), CARD_X, Y_STATION + 4,
       preview ? C_WARN : C_TEXT, C_BG);
  if (fav) drawStar(226, Y_STATION + 18, 8, C_WARN);
  useFont(F_SMALL);
  const String chip = fitText(tft, c.name, 92);
  const int w = tft.textWidth(chip) + 16;
  tft.drawRoundRect(CARD_X, Y_STATION + 35, w, 21, 10, c.color);
  text(chip, CARD_X + w / 2, Y_STATION + 38, c.color, C_BG, TC_DATUM);
  char buf[12];
  snprintf(buf, sizeof(buf), "%d/%d", selIdx + 1, (int)c.st.size());
  text(buf, CARD_X + w + 8, Y_STATION + 38, C_MUTED, C_BG);
  const int used = CARD_X + w + 8 + tft.textWidth(buf) + 10;
  if (s.city.length()) text(fitText(tft, s.city, SW - 10 - used), SW - 10, Y_STATION + 38, C_MUTED, C_BG, TR_DATUM);
}

// Bilgi satırı: biçim · bit hızı solda, yayın tamponu çubuğu sağda.
void drawInfo() {
  tft.fillRect(CARD_X, Y_STATION + 58, SW - CARD_X, 22, C_BG);
  const bool preview = selCat != cat || selIdx != idx;
  useFont(F_SMALL);
  if (millis() < toastUntil) {
    text(toastMsg, CARD_X, Y_STATION + 60, C_WARN, C_BG);
    return;
  }
  const char* s = preview ? "Seçiliyor…" : stream == Net::Connecting ? "Bağlanıyor…"
                : stream == Net::Failed ? "Yayın yok" : info;
  text(s, CARD_X, Y_STATION + 60, stream == Net::Failed && !preview ? C_ERR : C_MUTED, C_BG);
  if (!preview && stream == Net::Playing && bufPct >= 0) {
    constexpr int X = 196, Y = Y_STATION + 67, W = 36, H = 6;
    const uint16_t c = bufPct >= 50 ? C_OK : bufPct >= 20 ? C_WARN : C_ERR;
    tft.fillRoundRect(X, Y, W, H, 3, C_LINE);
    tft.fillRoundRect(X, Y, std::max(H, W * bufPct / 100), H, 3, c);
  }
}

// ---- Şarkı alanı ----
// Başlık "Sanatçı - Şarkı" ise iki satıra bölünür; sığmayan satır kayar.
struct Line { char s[128]; uint16_t col; int w, off; uint32_t pauseUntil; };
Line lineA, lineB;
uint8_t dots = 0;

void setLine(Line& l, TFT_eSprite& spr, const char* s, uint16_t col) {
  strncpy(l.s, s, sizeof(l.s) - 1);
  l.s[sizeof(l.s) - 1] = 0;
  l.col = col;
  l.w = spr.textWidth(l.s);
  l.off = 0;
  l.pauseUntil = millis() + 2500;
}

void renderLine(Line& l, TFT_eSprite& spr, int y) {
  spr.fillSprite(C_BG);
  spr.setTextColor(l.col, C_BG);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(l.s, -l.off, 1);
  if (l.w > SPR_W) spr.drawString(l.s, -l.off + l.w + SCROLL_GAP, 1);
  spr.pushSprite(SPR_X, y);
}

void updateSongLines() {
  if (stream == Net::Connecting) {
    char buf[24];
    snprintf(buf, sizeof(buf), "Bağlanıyor%.*s", dots % 4, "...");
    setLine(lineA, sprA, buf, C_MUTED);
    setLine(lineB, sprB, cats[cat].st[idx].name.c_str(), C_DIM);
  } else if (stream == Net::Failed) {
    setLine(lineA, sprA, "Yayın alınamıyor", C_ERR);
    setLine(lineB, sprB, "Başka bir istasyon seçin", C_MUTED);
  } else if (!title[0]) {
    setLine(lineA, sprA, cats[cat].st[idx].name.c_str(), C_TEXT);
    setLine(lineB, sprB, "Canlı yayın", C_MUTED);
  } else {
    char clean[128];
    cleanText(title, clean, sizeof(clean));
    char* dash = strstr(clean, " - ");
    if (dash) {
      *dash = 0;
      setLine(lineA, sprA, clean, C_TEXT);
      setLine(lineB, sprB, dash + 3, C_MUTED);
    } else {
      setLine(lineA, sprA, clean, C_TEXT);
      setLine(lineB, sprB, "", C_MUTED);
    }
  }
  dSong = true;
}

void drawSong() {
  tft.fillRect(0, Y_SONG, SW, Y_VOL - Y_SONG - 2, C_BG);
  useFont(F_BOLD);
  text(stream == Net::Playing ? "ŞİMDİ ÇALIYOR" : "YAYIN", 12, Y_SONG + 2, cats[cat].color, C_BG);
  renderLine(lineA, sprA, SPR_A_Y);
  renderLine(lineB, sprB, SPR_B_Y);
  for (int i = 0; i < VU_BANDS; i++) vuShownH[i] = vuShownP[i] = -1;  // alan silindi, çubuklar baştan çizilsin
}

// "ŞİMDİ ÇALIYOR" yazısının sağında 8 çubuk; solda kalın, sağda ince sesler.
// Her bant kendi son tepe değerine göre ölçeklenir (yavaşça düşer), böylece tüm çubuklar hareketli kalır.
// Çubuklar hızlı yükselir, yavaş iner; üstte tepe çizgisi bir süre asılı kalır.
constexpr int VU_X = 146, VU_Y = Y_SONG + 2, VU_H = 17, VU_W = 8, VU_GAP = 3;
constexpr float VU_RANGE_DB = 30;
uint32_t lastVu = 0, lastVuData = 0;

void tickVu() {
  const uint32_t now = millis();
  if (menu != Menu::None || now - lastVu < 50) return;
  lastVu = now;
  const bool live = stream == Net::Playing;
  if (vuN) lastVuData = now;
  const bool fresh = live && vuN && now - lastVuData < 300;
  for (int i = 0; i < VU_BANDS; i++) {
    float target = 0;
    if (fresh) {
      const float avg = (float)vuAcc[i] / vuN;
      const float db = 20 * log10f(avg + 1);
      vuCeil[i] = std::max(db, vuCeil[i] - 0.08f);
      target = constrain((db - (vuCeil[i] - VU_RANGE_DB)) / VU_RANGE_DB, 0.0f, 1.0f);
    }
    vuLevel[i] = target > vuLevel[i] ? target : vuLevel[i] * 0.82f;
    vuPeak[i] = std::max(vuLevel[i], vuPeak[i] - 0.025f);
    vuAcc[i] = 0;
  }
  vuN = 0;
  if (!live) return;  // bağlanırken/yayın yokken alan boş kalır
  const uint16_t col = cats[cat].color;
  for (int i = 0; i < VU_BANDS; i++) {
    const int h = std::max(1, (int)(vuLevel[i] * VU_H + 0.5f));
    const int p = std::min(VU_H - 1, (int)(vuPeak[i] * VU_H + 0.5f));
    if (h == vuShownH[i] && p == vuShownP[i]) continue;
    vuShownH[i] = h;
    vuShownP[i] = p;
    const int x = VU_X + i * (VU_W + VU_GAP);
    tft.fillRect(x, VU_Y, VU_W, VU_H - h, C_BG);
    tft.fillRect(x, VU_Y + VU_H - h, VU_W, h, col);
    if (p > h + 1) tft.fillRect(x, VU_Y + VU_H - p - 1, VU_W, 2, C_TEXT);
  }
}

// Kayan yazı ve "Bağlanıyor..." animasyonu; başka çizim beklerken çalışmaz.
uint32_t lastAnim = 0;
void tickSong() {
  const uint32_t now = millis();
  if (menu != Menu::None) return;
  if (stream == Net::Connecting) {
    if (now - lastAnim < 400) return;
    lastAnim = now;
    dots++;
    char buf[24];
    snprintf(buf, sizeof(buf), "Bağlanıyor%.*s", dots % 4, "...");
    setLine(lineA, sprA, buf, C_MUTED);
    renderLine(lineA, sprA, SPR_A_Y);
    return;
  }
  if (now - lastAnim < 40) return;
  lastAnim = now;
  auto step = [&](Line& l, TFT_eSprite& spr, int y) {
    if (l.w <= SPR_W || now < l.pauseUntil) return;
    l.off += 2;
    if (l.off >= l.w + SCROLL_GAP) { l.off = 0; l.pauseUntil = now + 2500; }
    renderLine(l, spr, y);
  };
  step(lineA, sprA, SPR_A_Y);
  step(lineB, sprB, SPR_B_Y);
}

// Menü satırı: simge, yazı, sağda sayı. sel ise satır vurgu renginde dolu çizilir.
enum Icon { IC_BACK, IC_STAR, IC_STAR_OFF, IC_CLOCK, IC_DOT, IC_PLAY, IC_NONE };
void drawRow(int y, bool sel, uint16_t accent, Icon ic, const String& label, const String& right = "") {
  constexpr int ROW = 19;
  const uint16_t bg = sel ? accent : C_BG, fg = sel ? C_BG : C_TEXT, ic_c = sel ? C_BG : accent;
  if (sel) tft.fillRoundRect(4, y, SW - 8, ROW, 6, accent);
  switch (ic) {
    case IC_BACK: tft.fillTriangle(18, y + 4, 18, y + 14, 11, y + 9, ic_c); break;
    case IC_STAR: drawStar(15, y + 9, 6, ic_c); break;
    case IC_STAR_OFF: drawStar(15, y + 9, 6, ic_c); tft.fillCircle(15, y + 9, 2, bg); break;
    case IC_CLOCK:
      tft.drawCircle(15, y + 9, 6, ic_c);
      tft.drawLine(15, y + 9, 15, y + 5, ic_c);
      tft.drawLine(15, y + 9, 18, y + 9, ic_c);
      break;
    case IC_DOT: tft.fillCircle(15, y + 9, 4, ic_c); break;
    case IC_PLAY: tft.fillTriangle(12, y + 5, 12, y + 14, 19, y + 9, ic_c); break;
    case IC_NONE: break;
  }
  const int rw = right.length() ? tft.textWidth(right) + 10 : 0;
  text(fitText(tft, label, SW - 40 - rw), 26, y + 1, fg, bg);
  if (rw) text(right, SW - 14, y + 1, sel ? C_BG : C_MUTED, bg, TR_DATUM);
}

String listTitle(int kind) {
  return kind == LIST_FAV ? String("Favoriler") : kind == LIST_RECENT ? String("Son Dinlenenler") : cats[kind].name;
}
uint16_t listColor(int kind) {
  return kind == LIST_FAV ? C_WARN : kind == LIST_RECENT ? C_MUTED : cats[kind].color;
}

void menuHeader(const String& title, uint16_t color, int pos, int n) {
  tft.fillRect(0, 28, SW, Y_VOL - 28 - 2, C_BG);
  useFont(F_BOLD);
  text(title, 12, 31, color, C_BG);
  useFont(F_SMALL);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d / %d", pos + 1, n);
  text(buf, SW - 10, 31, C_MUTED, C_BG, TR_DATUM);
}

void menuHint(const char* hint) {
  text(hint, SW / 2, 174, C_DIM, C_BG, TC_DATUM);
}

// Menü (ana menü ya da istasyon listesi); istasyon kartı ve şarkı alanının üstüne çizilir.
void drawMenu() {
  constexpr int Y0 = 52, ROW = 19, ROWS = 6, SEP_GAP = 7;
  const bool isMain = menu == Menu::Main;
  const int n = isMain ? mainEntries() : items.size() + 1;
  const int top = std::max(0, std::min(listCursor - ROWS / 2, n - ROWS));
  menuHeader(isMain ? String("MENÜ") : listTitle(menuCat), isMain ? C_TEXT : listColor(menuCat), listCursor, n);
  if (!isMain && items.empty())
    text(menuCat == LIST_FAV ? "Henüz favori yok" : "Henüz kayıt yok", SW / 2, 110, C_MUTED, C_BG, TC_DATUM);
  for (int r = 0; r < ROWS && top + r < n; r++) {
    const int i = top + r;
    // Ana menüde üst satırlar ile kategoriler arasında boşluk ve çizgi
    const int gap = isMain && i >= CAT_OFS && top < CAT_OFS ? SEP_GAP : 0, y = Y0 + r * ROW + gap;
    if (gap && i == CAT_OFS) tft.drawFastHLine(12, y - SEP_GAP / 2 - 1, SW - 24, C_DIM);
    const bool sel = i == listCursor;
    if (isMain) {
      if (i == M_CLOSE) drawRow(y, sel, C_MUTED, IC_BACK, "Kapat");
      else if (i == M_FAVADD) {
        const bool fav = isFav(cats[cat].st[idx].url);
        drawRow(y, sel, C_WARN, fav ? IC_STAR_OFF : IC_STAR, fav ? "Favoriden çıkar" : "Favoriye ekle");
      } else if (i == M_FAVS) drawRow(y, sel, C_WARN, IC_STAR, "Favoriler", String(countLines(favs)));
      else if (i == M_RECENT) drawRow(y, sel, C_MUTED, IC_CLOCK, "Son Dinlenenler", String(countLines(recents)));
      else {
        const int c = i - CAT_OFS;
        drawRow(y, sel, cats[c].color, c == cat ? IC_PLAY : IC_DOT, cats[c].name, String(cats[c].st.size()));
      }
    } else if (i == 0) {
      drawRow(y, sel, C_MUTED, IC_BACK, "Geri");
    } else {
      const Ref& ref = items[i - 1];
      const Station& st = cats[ref.c].st[ref.i];
      const bool playing = ref.c == cat && ref.i == idx;
      const bool fav = isFav(st.url);  // fontta ★ yok; sağda yer ayrılıp yıldız çizilir
      drawRow(y, sel, cats[ref.c].color, playing ? IC_PLAY : menuCat < 0 ? IC_DOT : IC_NONE, st.name,
              fav ? "    " : "");
      if (fav) drawStar(SW - 18, y + 9, 6, sel ? C_BG : C_WARN);
    }
  }
  menuHint("Çevir: seç   ·   Bas: tamam");
}

void drawVol() {
  tft.fillRect(0, Y_VOL, SW, 24, C_BG);
  const uint16_t ic = shownVol ? C_MUTED : C_DIM;  // hoparlör simgesi
  tft.fillRect(10, Y_VOL + 8, 5, 8, ic);
  tft.fillTriangle(14, Y_VOL + 8, 21, Y_VOL + 2, 21, Y_VOL + 21, ic);
  tft.fillTriangle(14, Y_VOL + 8, 21, Y_VOL + 21, 14, Y_VOL + 15, ic);
  constexpr int X = 32, W = 168, H = 8;
  tft.fillRoundRect(X, Y_VOL + 8, W, H, 4, C_LINE);
  if (shownVol) tft.fillRoundRect(X, Y_VOL + 8, std::max(H, W * shownVol / VOL_MAX), H, 4, cats[cat].color);
  useFont(F_BOLD);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", shownVol);
  text(buf, SW - 8, Y_VOL + 3, C_TEXT, C_BG, TR_DATUM);
}

uint16_t tempColor(float t) {
  return t < 10 ? RGB(0x60A5FA) : t < 18 ? RGB(0x22D3EE) : t < 26 ? C_OK : t < 30 ? C_WARN : C_ERR;
}

void tempText(float t, char* buf, size_t n) {
  if (isnan(t)) strcpy(buf, "--");
  else snprintf(buf, n, "%.0f°", t);
  if (!strcmp(buf, "-0°")) strcpy(buf, "0°");
}

void drawCard(int x, const char* label, const char* value, uint16_t col, const String& sub) {
  constexpr int W = 111, H = 96;
  tft.fillRoundRect(x, Y_CARDS, W, H, 10, C_PANEL);
  useFont(F_BOLD);
  text(label, x + 10, Y_CARDS + 7, C_MUTED, C_PANEL);
  useFont(F_BIG);
  text(value, x + W / 2, Y_CARDS + 28, col, C_PANEL, TC_DATUM);
  useFont(F_SMALL);
  text(fitText(tft, sub, W - 10), x + W / 2, Y_CARDS + 72, C_MUTED, C_PANEL, TC_DATUM);
}

void drawIndoor() {
  char buf[16];
  if (indoorPage == 2) {  // bugünkü dinleme: saat:dakika ve şarkı sayısı
    const uint32_t min = listenMs / 60000;
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(min / 60), (unsigned)(min % 60));
    char sub[32];
    snprintf(sub, sizeof(sub), "Bugün · %u şarkı", (unsigned)songCount);
    useFont(F_SMALL);
    if (tft.textWidth(sub) > 101) snprintf(sub, sizeof(sub), "%u şarkı", (unsigned)songCount);
    drawCard(6, "DİNLEME", buf, cats[cat].color, sub);
  } else if (isnan(tempC)) {
    drawCard(6, "İÇERİ", "--", C_DIM, "Sensör yok");
  } else if (indoorPage == 1) {
    snprintf(buf, sizeof(buf), "%%%.0f", hum);
    drawCard(6, "İÇERİ", buf, RGB(0x4DA3FF), "Nem");
  } else {
    tempText(tempC, buf, sizeof(buf));
    drawCard(6, "İÇERİ", buf, tempColor(tempC), "Sıcaklık");
  }
}

// WMO hava kodları (Open-Meteo) → simge grubu ve Türkçe açıklama
enum Wx { SUN, PARTLY, CLOUD, FOG, DRIZZLE, RAIN, SNOW, STORM };
Wx wxGroup(int c) {
  if (c == 0) return SUN;
  if (c <= 2) return PARTLY;
  if (c == 3) return CLOUD;
  if (c <= 48) return FOG;
  if (c <= 57) return DRIZZLE;
  if (c <= 67 || (c >= 80 && c <= 82)) return RAIN;
  if (c <= 77 || c == 85 || c == 86) return SNOW;
  return STORM;
}
const char* wxText(int c) {
  switch (wxGroup(c)) {
    case SUN: return "Açık";
    case PARTLY: return c == 1 ? "Az bulutlu" : "Parçalı bulutlu";
    case CLOUD: return "Kapalı";
    case FOG: return "Sisli";
    case DRIZZLE: return "Çisenti";
    case RAIN: return c >= 80 ? "Sağanak" : "Yağmurlu";
    case SNOW: return "Karlı";
    default: return "Fırtına";
  }
}

void drawCloud(int x, int y, uint16_t c) {
  tft.fillCircle(x - 6, y + 2, 5, c);
  tft.fillCircle(x + 1, y - 2, 7, c);
  tft.fillCircle(x + 8, y + 2, 5, c);
  tft.fillRect(x - 6, y + 2, 14, 6, c);
}

void drawSun(int x, int y, int r) {
  const uint16_t c = C_WARN;
  for (int a = 0; a < 8; a++) {
    const float t = a * PI / 4;
    tft.drawLine(x + cosf(t) * (r + 3), y + sinf(t) * (r + 3), x + cosf(t) * (r + 6), y + sinf(t) * (r + 6), c);
  }
  tft.fillCircle(x, y, r, c);
}

void drawWxIcon(int x, int y, int code, bool day) {
  const uint16_t cloud = RGB(0xC9D1DC), dark = RGB(0x7C8798), blue = RGB(0x4DA3FF);
  switch (wxGroup(code)) {
    case SUN:
      if (day) drawSun(x, y, 6);
      else { tft.fillCircle(x, y, 8, RGB(0xE5E7EB)); tft.fillCircle(x + 4, y - 3, 7, C_PANEL); }
      break;
    case PARTLY:
      if (day) drawSun(x - 5, y - 5, 4);
      else { tft.fillCircle(x - 5, y - 5, 6, RGB(0xE5E7EB)); tft.fillCircle(x - 2, y - 7, 5, C_PANEL); }
      drawCloud(x + 2, y + 3, cloud);
      break;
    case CLOUD: drawCloud(x, y, cloud); break;
    case FOG:
      for (int i = 0; i < 3; i++) tft.fillRoundRect(x - 10 + (i % 2) * 3, y - 6 + i * 6, 18, 3, 1, dark);
      break;
    case DRIZZLE:
    case RAIN:
      drawCloud(x, y - 4, cloud);
      for (int i = -1; i <= 1; i++) tft.drawLine(x + i * 6, y + 7, x + i * 6 - 2, y + 12, blue);
      break;
    case SNOW:
      drawCloud(x, y - 4, cloud);
      for (int i = -1; i <= 1; i++) tft.fillCircle(x + i * 6, y + 10, 1, C_TEXT);
      break;
    default:
      drawCloud(x, y - 4, dark);
      tft.fillTriangle(x + 1, y + 2, x - 4, y + 10, x, y + 9, C_WARN);
      tft.fillTriangle(x, y + 7, x + 4, y + 7, x - 2, y + 15, C_WARN);
  }
}

// Tahmin günü: üstte gün adı, solda büyük en yüksek sıcaklık, sağda simge ve altında en düşük, en altta durum.
void drawForecast(int x, const DayWx& d, bool tomorrow) {
  static const char* const DAYS_UP[] = {"PAZAR", "PAZARTESİ", "SALI", "ÇARŞAMBA", "PERŞEMBE", "CUMA", "CUMARTESİ"};
  constexpr int W = 111, H = 96;
  tft.fillRoundRect(x, Y_CARDS, W, H, 10, C_PANEL);
  useFont(F_BOLD);
  text(tomorrow ? "YARIN" : DAYS_UP[d.wday], x + 10, Y_CARDS + 7, C_MUTED, C_PANEL);
  char buf[8];
  tempText(d.hi, buf, sizeof(buf));
  useFont(F_BIG);
  text(buf, x + 8, Y_CARDS + 28, tempColor(d.hi), C_PANEL);
  drawWxIcon(x + 90, Y_CARDS + 40, d.code, true);
  tempText(d.lo, buf, sizeof(buf));
  useFont(F_SMALL);
  text(buf, x + 90, Y_CARDS + 54, RGB(0x60A5FA), C_PANEL, TC_DATUM);
  text(fitText(tft, wxText(d.code), W - 10), x + W / 2, Y_CARDS + 72, C_MUTED, C_PANEL, TC_DATUM);
}

void drawOutdoor() {
  portENTER_CRITICAL(&wxMux);
  const Outdoor o = outdoor;
  portEXIT_CRITICAL(&wxMux);
  if (outdoorPage > 0 && outdoorPage <= o.nDays) {
    drawForecast(123, o.days[outdoorPage - 1], outdoorPage == 1);
    return;
  }
  char buf[8];
  tempText(o.valid ? o.t : NAN, buf, sizeof(buf));
  drawCard(123, "DIŞARI", buf, o.valid ? tempColor(o.t) : C_DIM, o.valid ? wxText(o.code) : "Bekleniyor…");
  if (o.valid) drawWxIcon(123 + 92, Y_CARDS + 16, o.code, o.day);
}

void updateDisplay() {
  if (dStatus) { dStatus = false; drawStatus(); return; }
  if (menu != Menu::None) {
    if (dMenu) { dMenu = false; drawMenu(); return; }
  } else {
    if (dStation) { dStation = false; drawStation(); return; }
    if (dInfo)    { dInfo = false;    drawInfo();    return; }
    if (dSong)    { dSong = false;    drawSong();    return; }
  }
  if (dVol)     { dVol = false;     drawVol();     return; }
  if (dIndoor)  { dIndoor = false;  drawIndoor();  return; }
  if (dOutdoor) { dOutdoor = false; drawOutdoor(); return; }
  tickSong();
  tickVu();
}

// Saniyede bir: dakika değişti mi, Wi-Fi gücü, yayın bilgisi/tampon, gece karartma. 4 s'de bir içeri kartı.
uint32_t lastTick = 0, shownOutdoorVer = 0;
void tickStatus() {
  const uint32_t now = millis();
  if (toastUntil && now >= toastUntil) { toastUntil = 0; dInfo = true; }
  if (now - lastIndoorFlip >= 4000) {
    lastIndoorFlip = now;
    indoorPage = (indoorPage + 1) % 3;
    if (indoorPage == 1 && isnan(hum)) indoorPage = 2;  // sensör yoksa nem sayfası atlanır
    dIndoor = true;
  }
  if (now - lastOutdoorFlip >= (outdoorPage == 0 ? 8000u : 4000u)) {
    lastOutdoorFlip = now;
    portENTER_CRITICAL(&wxMux);
    const int n = outdoor.nDays;
    portEXIT_CRITICAL(&wxMux);
    const int next = (outdoorPage + 1) % (n + 1);
    if (next != outdoorPage) { outdoorPage = next; dOutdoor = true; }
  }
  if (outdoorVer != shownOutdoorVer) { shownOutdoorVer = outdoorVer; dOutdoor = true; }
  if (now - lastTick < 1000) return;
  const uint32_t dt = now - lastTick;
  lastTick = now;
  if (stream == Net::Playing && dt < 5000) listenMs += dt;
  if (listenMs / 1000 - savedSec >= 300) saveStats();
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_min != shownMinute) {
    shownMinute = t.tm_min;
    checkStatDay(t);
    if (indoorPage == 2) dIndoor = true;
    dStatus = true;
    const bool night = t.tm_hour >= NIGHT_FROM || t.tm_hour < NIGHT_TO;
    ledcWrite(BL_CH, night ? BL_NIGHT : BL_DAY);
  }
  const int lv = rssiLevel();
  if (lv != wifiLevel) { wifiLevel = lv; dStatus = true; }
  if (stream == Net::Playing) {
    char buf[sizeof(info)];
    const uint32_t br = audio.getBitRate();
    if (br) snprintf(buf, sizeof(buf), "%s · %u kbps", audio.getCodecname(), (unsigned)((br + 500) / 1000));
    else snprintf(buf, sizeof(buf), "%s", audio.getCodecname());
    const uint32_t filled = audio.inBufferFilled(), total = filled + audio.inBufferFree();
    const int pct = total ? (filled * 10 / total) * 10 : -1;  // 10'luk adımlar, gereksiz çizim olmasın
    if (strcmp(buf, info) || pct != bufPct) {
      strcpy(info, buf);
      bufPct = pct;
      dInfo = true;
    }
  }
}

// DHT11 en az 1 s aralık ister; 3 art arda hata olursa "--" gösterilir.
uint32_t lastDht = 0;
uint8_t dhtFails = 0;
void pollDht() {
  if (millis() - lastDht < 3000) return;
  lastDht = millis();
  const float h = dht.readHumidity(), t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    if (dhtFails < 3 && ++dhtFails == 3) {
      tempC = hum = NAN;
      dIndoor = true;
      Serial.println("[iklim] DHT11 okunamiyor");
    }
    return;
  }
  dhtFails = 0;
  if (t != tempC || h != hum) {
    tempC = t;
    hum = h;
    dIndoor = true;
    Serial.printf("[iklim] %.0f C  %.0f %%\n", t, h);
  }
}

int wrap(int v, int n) { return ((v % n) + n) % n; }

void setStream(Net s) {
  stream = s;
  dInfo = true;
  updateSongLines();
}

void play(int c, int i) {
  i = wrap(i, cats[c].st.size());
  cat = selCat = c;
  idx = selIdx = i;
  lastIdxOfCat[c] = i;
  gotFormat = streamFailed = false;
  title[0] = 0;
  info[0] = 0;
  bufPct = -1;
  playStarted = millis();
  dStatus = dStation = dVol = true;
  setStream(Net::Connecting);
  // connecttohost birkaç saniye bloklar; "Bağlanıyor" önce ekrana çıksın
  if (menu == Menu::None)
    while (dStatus || dStation || dInfo || dSong) updateDisplay();
  const Station& s = cats[c].st[i];
  Serial.printf("[radyo] %s %d/%d %s\n", cats[c].name.c_str(), i + 1, (int)cats[c].st.size(), s.name.c_str());
  if (prefs.getString("url") != s.url) prefs.putString("url", s.url);
  pushRecent(s.url);
  if (!audio.connecttohost(s.url.c_str())) streamFailed = true;
}

// Wi-Fi yüzünden düşen yayın, Wi-Fi geri gelince kendiliğinden yeniden başlatılır.
bool failedNoWifi = false;
void pollStream() {
  const bool online = WiFi.status() == WL_CONNECTED;
  if (stream == Net::Connecting) {
    if (gotFormat) setStream(Net::Playing);
    else if (streamFailed || millis() - playStarted > CONNECT_TIMEOUT_MS) {
      failedNoWifi = !online;
      setStream(Net::Failed);
    }
  } else if (stream == Net::Playing && (streamFailed || !online)) {
    failedNoWifi = !online;
    setStream(Net::Failed);
  } else if (stream == Net::Failed && failedNoWifi && online) {
    Serial.println("[net] Wi-Fi geri geldi, yayin yeniden baslatiliyor");
    play(cat, idx);
  }
}

#if STATION_SCAN
void scan() {
  Serial.println("[tarama] istasyonlar deneniyor...");
  for (auto& c : cats)
    for (auto& s : c.st) {
      // Wi-Fi tarama sırasında kopabiliyor; o anki sonuç istasyonun değil ağın hatası, Wi-Fi gelince tekrar denenir
      bool wifiOk = false;
      for (int attempt = 0; attempt < 3 && !wifiOk; attempt++) {
        const uint32_t w = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - w < 60000) delay(200);
        gotFormat = streamFailed = false;
        audio.connecttohost(s.url.c_str());
        const uint32_t t0 = millis();  // streamtheworld iki SSL bağlantısı kurar, 8 s yetmiyor
        while (millis() - t0 < 15000 && !gotFormat && !streamFailed) audio.loop();
        wifiOk = WiFi.status() == WL_CONNECTED;
      }
      Serial.printf("[tarama] %-12s %-22s %s\n", c.name.c_str(), s.name.c_str(),
                    gotFormat ? "CALISIYOR" : wifiOk ? "yok" : "WIFI YOK");
    }
  Serial.println("[tarama] bitti");
}
#endif

// ---- Gezinme ----
// Seçim değişince hemen bağlanılmaz; encoder 1 s durursa seçilen çalar.
// Böylece hızlı çevirirken aradaki her yayına bağlanılmaz.
constexpr uint32_t PREVIEW_MS = 1000, MENU_TIMEOUT_MS = 10000;
uint32_t selMoved = 0;

void selectStation(int c, int i) {
  if (c != selCat) dStatus = true;  // üst çizgi kategori rengini alır
  selCat = c;
  selIdx = wrap(i, cats[c].st.size());
  selMoved = millis();
  dStation = dInfo = true;
}

// Menü açılınca yarım kalmış önizleme iptal edilir, yoksa menü kapanınca istenmeden çalar.
void cancelPreview() {
  if (selCat != cat || selIdx != idx) {
    selCat = cat;
    selIdx = idx;
    dStatus = dStation = dInfo = true;
  }
}

void openMain(int cursor) {
  cancelPreview();
  menu = Menu::Main;
  listCursor = cursor;
  listTouched = millis();
  dMenu = true;
}

bool findStation(const String& url, Ref& out) {
  for (int c = 0; c < (int)cats.size(); c++)
    for (int i = 0; i < (int)cats[c].st.size(); i++)
      if (cats[c].st[i].url == url) { out = {c, i}; return true; }
  return false;
}

void buildItems(int kind) {
  items.clear();
  if (kind >= 0) {
    for (int i = 0; i < (int)cats[kind].st.size(); i++) items.push_back({kind, i});
    return;
  }
  const String& src = kind == LIST_FAV ? favs : recents;
  int from = 0, nl;
  while ((nl = src.indexOf('\n', from)) >= 0) {
    Ref r;
    if (findStation(src.substring(from, nl), r)) items.push_back(r);  // listeden çıkarılmış istasyon atlanır
    from = nl + 1;
  }
}

// Liste çalan istasyondan açılır; çalan listede yoksa ilk istasyondan (satır 0 Geri).
void openStations(int kind) {
  menu = Menu::Stations;
  menuCat = kind;
  buildItems(kind);
  listCursor = items.empty() ? 0 : 1;
  for (int i = 0; i < (int)items.size(); i++)
    if (items[i].c == cat && items[i].i == idx) listCursor = i + 1;
  listTouched = millis();
  dMenu = true;
}

void closeMenu() {
  menu = Menu::None;
  tft.fillRect(0, 28, SW, Y_VOL - 28 - 2, C_BG);
  dStation = dInfo = dSong = true;
}

void toast(const char* msg) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastUntil = millis() + 2000;
  dInfo = true;
}

void favToggle(int c, int i) {
  const Station& s = cats[c].st[i];
  toggleFav(s.url);
  const bool on = isFav(s.url);
  Serial.printf("[favori] %s %s\n", s.name.c_str(), on ? "eklendi" : "cikarildi");
  toast(on ? "Favorilere eklendi" : "Favorilerden çıkarıldı");
  dStation = true;
}

void onTurn(int steps) {
  if (menu == Menu::None) {
    selectStation(selCat, selIdx + steps);
    return;
  }
  listTouched = millis();
  dMenu = true;
  const int n = menu == Menu::Main ? mainEntries() : items.size() + 1;
  listCursor = wrap(listCursor + steps, n);
}

void nextCategory() {
  const int c = wrap(selCat + 1, cats.size());
  selectStation(c, lastIdxOfCat[c]);
}

void onPress() {
  static const char* const MENUS[] = {"calarken", "menu", "istasyonlar"};
  Serial.printf("[tus] bas (%s, satir %d)\n", MENUS[(int)menu], listCursor);
  listTouched = millis();
  switch (menu) {
    case Menu::None:
      openMain(cat + CAT_OFS);  // imleç çalan kategoride: bir bas daha = o kategorinin istasyonları
      break;
    case Menu::Main:
      if (listCursor == M_CLOSE) closeMenu();
      else if (listCursor == M_FAVADD) { closeMenu(); favToggle(cat, idx); }
      else if (listCursor == M_FAVS) openStations(LIST_FAV);
      else if (listCursor == M_RECENT) openStations(LIST_RECENT);
      else openStations(listCursor - CAT_OFS);
      break;
    case Menu::Stations:
      if (listCursor == 0) {
        openMain(menuCat == LIST_FAV ? M_FAVS : menuCat == LIST_RECENT ? M_RECENT : menuCat + CAT_OFS);
      } else {
        const Ref r = items[listCursor - 1];
        closeMenu();
        play(r.c, r.i);
      }
      break;
  }
}

// Encoder kesmeyle okunur: istasyona bağlanırken (connecttohost birkaç saniye bloklar) çevirmeler kaybolmaz.
// Ben Buxton'ın tam adım durum tablosu: kontak sıçramalarını yok sayar, her tık tam olarak bir adım.
// Yön ters gelirse ENC_REVERSE'i değiştir.
constexpr bool ENC_REVERSE = false;
DRAM_ATTR const uint8_t ENC_TABLE[7][4] = {
  {0x0, 0x2, 0x4, 0x0}, {0x3, 0x0, 0x1, 0x10}, {0x3, 0x2, 0x0, 0x0}, {0x3, 0x2, 0x1, 0x0},
  {0x6, 0x0, 0x4, 0x0}, {0x6, 0x5, 0x0, 0x20}, {0x6, 0x5, 0x4, 0x0},
};
volatile uint8_t encState = 0;
volatile int encSteps = 0;
portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR encIsr() {
  const uint8_t pins = (digitalRead(ENC_DT) << 1) | digitalRead(ENC_CLK);
  portENTER_CRITICAL_ISR(&encMux);
  encState = ENC_TABLE[encState & 0x0F][pins];
  if (encState & 0x20) encSteps++;
  else if (encState & 0x10) encSteps--;
  portEXIT_CRITICAL_ISR(&encMux);
}

void pollEncoder() {
  portENTER_CRITICAL(&encMux);
  const int n = encSteps;
  encSteps = 0;
  portEXIT_CRITICAL(&encMux);
  if (n) onTurn(ENC_REVERSE ? -n : n);
}

// Düğme bırakınca bir "bas" sayılır (ne kadar basılı tutulduğu önemli değil).
constexpr uint32_t DEBOUNCE_MS = 25;
int swRaw = HIGH, swStable = HIGH;
uint32_t swChanged = 0;
bool pollButton() {
  const uint32_t now = millis();
  const int r = digitalRead(ENC_SW);
  if (r != swRaw) { swRaw = r; swChanged = now; }
  if (now - swChanged > DEBOUNCE_MS && swRaw != swStable) {
    swStable = swRaw;
    return swStable == HIGH;
  }
  return false;
}

void pollNavigation() {
  pollEncoder();
  if (pollButton()) onPress();
  if (menu != Menu::None) {
    if (millis() - listTouched > MENU_TIMEOUT_MS) closeMenu();
  } else if ((selCat != cat || selIdx != idx) && millis() - selMoved > PREVIEW_MS) {
    play(selCat, selIdx);
  }
}

// Pot: I2S çalışırken okumaların bir kısmı sahte geliyor (hem 0 hem 4095 görüldü).
// İki yöndeki sıçramayı da elemek için 15 okumanın ortancası, sonra son 5 değerin ortancası alınır.
// Seviye ancak ham değer mevcut seviyenin aralığından HYST kadar taşınca değişir; sınırda 10↔11 titremez.
constexpr int VOL_STEP = 4096 / (VOL_MAX + 1), VOL_HYST = 70;
int lastVol = -1;
uint32_t lastPot = 0;
int hist[5] = {0}, histIdx = 0;
int median(int* v, int n) {
  std::sort(v, v + n);
  return v[n / 2];
}
void pollPot() {
  if (millis() - lastPot < 50) return;
  lastPot = millis();
  int s[15];
  for (int& r : s) r = analogRead(POT);
  hist[histIdx] = median(s, 15);
  histIdx = (histIdx + 1) % 5;
  int h[5];
  memcpy(h, hist, sizeof(h));
  const int raw = median(h, 5);
  if (lastVol >= 0 && raw >= lastVol * VOL_STEP - VOL_HYST && raw < (lastVol + 1) * VOL_STEP + VOL_HYST) return;
  const int vol = std::min<int>(raw / VOL_STEP, VOL_MAX);
  if (vol == lastVol) return;
  lastVol = vol;
  audio.setVolume(vol);
  shownVol = vol;
  dVol = true;
  Serial.printf("[ses] %d/%d\n", vol, VOL_MAX);
}
}  // namespace

void setup() {
  // Hoparlör akım darbesinde zayıf USB yüzünden brownout reseti olmasın
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);  // KY-040'ın SW pull-up'ı çoğu kartta takılı değil
  attachInterrupt(ENC_CLK, encIsr, CHANGE);
  attachInterrupt(ENC_DT, encIsr, CHANGE);
  loadStations();
  lastIdxOfCat.assign(cats.size(), 0);
  prefs.begin("radio");
  favs = prefs.getString("favs");
  recents = prefs.getString("recent");
  statDay = prefs.getUInt("sday", 0);  // gün tutmuyorsa saat gelince checkStatDay sıfırlar
  listenMs = prefs.getUInt("ssec", 0) * 1000;
  savedSec = listenMs / 1000;
  songCount = prefs.getUInt("ssong", 0);
  for (const char* k : {"tone0", "tone1", "tone2"}) if (prefs.isKey(k)) prefs.remove(k);  // ton ayarı kaldırıldı; eski kayıtları sil
  // Liste düzenlenince sıra kayar; son istasyon adresiyle aranır, bulunamazsa ilk istasyon
  const String last = prefs.getString("url");
  for (int c = 0; c < (int)cats.size(); c++)
    for (int i = 0; i < (int)cats[c].st.size(); i++)
      if (cats[c].st[i].url == last) { cat = selCat = c; idx = selIdx = i; }

  tft.init();
  tft.setRotation(2);  // dikey, J2 pinleri üstte
  tft.setTextWrap(false, false);
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(TFT_BL, BL_CH);
  ledcWrite(BL_CH, BL_DAY);
  // Şarkı satırı sprite'ları RAM parçalanmadan önce, en başta ayrılır (~22 KB)
  sprA.createSprite(SPR_W, 28);
  sprA.loadFont(F_MED);
  sprA.setTextWrap(false, false);
  sprB.createSprite(SPR_W, 22);
  sprB.loadFont(F_SMALL);
  sprB.setTextWrap(false, false);

  tft.fillScreen(C_BG);
  useFont(F_MED);
  text("Wi-Fi bağlanıyor…", SW / 2, SH / 2, C_MUTED, C_BG, MC_DATUM);
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[net] baglaniyor");
  for (int i = 0; i < 120 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " tamam" : " BASARISIZ");
  if (WiFi.status() != WL_CONNECTED) {  // modem kapalı mı, ağ görünüyor mu?
    Serial.printf("[net] durum kodu %d\n", WiFi.status());
    const int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      Serial.printf("[net] ag: %s  %d dBm  kanal %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
  }
  configTzTime(TZ_TR, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(weatherTask, "hava", 6144, nullptr, 1, nullptr, 0);

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
#if USE_POT
  pollPot();  // açılışta potun konumundan başla
#else
  audio.setVolume(VOL_FIXED);
  shownVol = VOL_FIXED;
#endif
  tft.fillScreen(C_BG);
#if STATION_SCAN
  scan();
#endif
  play(cat, idx);
  Serial.printf("[bellek] bos heap: %u\n", ESP.getFreeHeap());
}

void loop() {
  audio.loop();
  pollNavigation();
#if USE_POT
  pollPot();
#endif
  pollDht();
  pollStream();
  tickStatus();
  updateDisplay();
  if (Serial.available()) {
    const char c = Serial.read();
    if (c == 'n') play(cat, idx + 1);
    if (c == 'p') play(cat, idx - 1);
    if (c == 'c') nextCategory();
    if (c == 'f') {
      favToggle(cat, idx);
    }
  }
}

void audio_info(const char* info) {
  if (strstr(info, "BitRate") || strstr(info, "SampleRate") || strstr(info, "Decoder has been initialized"))
    gotFormat = true;
  if (strstr(info, "failed")) streamFailed = true;
  Serial.printf("[audio] %s\n", info);
}
void audio_showstreamtitle(const char* info) {
  Serial.printf("[calan] %s\n", info);
  if (!strncmp(info, "HTTP/", 5)) {  // kütüphane hata yanıtını (403 Forbidden vb.) başlık olarak bildiriyor
    streamFailed = true;
    return;
  }
  strncpy(title, info, sizeof(title) - 1);
  title[sizeof(title) - 1] = 0;
  // "Sanatçı - Şarkı" biçimindeki her yeni başlık bir şarkı sayılır (istasyon adı, reklam sayılmaz)
  if (strstr(info, " - ") && strcmp(info, lastSong)) {
    strncpy(lastSong, info, sizeof(lastSong) - 1);
    lastSong[sizeof(lastSong) - 1] = 0;
    songCount++;
    if (indoorPage == 2) dIndoor = true;
  }
  if (stream == Net::Playing) updateSongLines();
}
// Çözülen her ses bloğu hoparlöre gitmeden önce buradan geçer; spektrum çubukları için toplanır.
void audio_process_extern(int16_t* buff, uint16_t len, bool* continueI2S) {
  *continueI2S = true;
  vuFeed(buff, len, audio.getChannels());
}
