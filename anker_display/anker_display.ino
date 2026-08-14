/*
╔═════════════════════════════════════════════════════════════╗
║  Anker Solix Display – ESP32-C3 + GC9A01A 240x240 rund      ║
║                                                             ║
║  Zeigt Solarleistung, Akkustand, Akkuleistung und Netzbezug ║
║  einer Anker SOLIX Solarbank an – alle 3 Sekunden.          ║
║                                                             ║
║  Die REST-API liefert nur 5-Minuten-Cachedaten, und ihr     ║
║  verschluesselter Endpunkt (algo_ecdh) ist bis heute nicht  ║
║  nachgebaut. Die Werte kommen deshalb ueber Ankers          ║
║  MQTT-Broker: get_user_mqtt_info liefert ein Zertifikat,    ║
║  damit TLS zu aiot-mqtt-eu.anker.com, und ein Trigger       ║
║  bringt die Solarbank auf den 3-Sekunden-Takt.              ║
║                                                             ║
║  param_info-Binaerformat (Nachrichtentyp 0405):             ║
║    ff 09 | len(LE16) | 03 01 0f | 04 05 | Felder            ║
║    Feld:  tag(1) len(1) typ(1) wert(len-1)                  ║
║    typ:   00=String 01=u8 02=i16 03=u32 05=float32(LE)      ║
║                                                             ║
║  Belegte Felder – Solarbank A17C5:                          ║
║    a3      Ladestand der Kopfstation in %                   ║
║    ab, c2  Solarleistung gesamt (W)                         ║
║    ac      Akkuleistung (W), negativ = Entladen             ║
║    ad      Ausgangsleistung (W) = ab + ac                   ║
║    c6..c9  die vier MPPT-Strings, Summe = ab                ║
║  Netzzaehler SHEM3 – Werte als u32, nicht float:            ║
║    a8      Netzbezug, a9 Einspeisung (Hundertstel-Watt)     ║
║                                                             ║
║  Ladestand je Akkupack: state_info Typ 0500, Bloecke a4 ff, ║
║  Offset 36 in Zehntelprozent. Der Systemwert - den auch die ║
║  App zeigt - ist deren Mittel, nicht a3.                    ║
║                                                             ║
║  ACHTUNG: "battery" aus state_info ist NICHT der Ladestand  ║
║  – der Wert steht konstant auf 100.                         ║
║                                                             ║
║  Ausfuehrlich: docs/mqtt-protokoll.md                       ║
╚═════════════════════════════════════════════════════════════╝

  SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
  Copyright (c) 2026 Detlev Euskirchen
*/
#define FW_VERSION "1.12.0"

// Ausfuehrliche Ausgaben im seriellen Monitor.
//   1 = jede MQTT-Nachricht wird protokolliert (zum Mitlesen und Decodieren)
//   0 = nur Start, Fehler und Zustandswechsel
// Start- und Fehlermeldungen bleiben in beiden Faellen erhalten, damit sich
// ein Problem auch bei fest verbautem Geraet noch nachvollziehen laesst.
#define VERBOSE 1

#if VERBOSE
  #define LOGF(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGF(...) do{}while(0)
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "time.h"
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define AP_SSID        "Anker-Display-Setup"
#define AP_IP          "192.168.4.1"
#define FETCH_INTERVAL  30000

#define BATT_CAP_WH     2700

static const char* ANKER_HOST = "https://ankerpower-api-eu.anker.com";
static const char* SERVER_PUBKEY_HEX =
  "04c5c00c4f8d1197cc7c3167c52bf7acb054d722f0ef08dcd7e0883236e0d72a3"
  "868d9750cb47fa4619248f3d83f0f662671dadc6e2d31c2f41db0161651c7c076";

// ─────────────────────────────────────────────────────────────────────────────
// DISPLAY
// ─────────────────────────────────────────────────────────────────────────────
// ── Touch ───────────────────────────────────────────────────────────────────
// Runde GC9A01-Module mit Touch verwenden praktisch immer einen CST816S ueber
// I2C. Die Anschluesse unten gelten fuer das verbreitete Waveshare-Modul
// ESP32-C3-LCD-1.28; weicht dein Board ab, sind es genau diese vier Zeilen.
// Ohne Touch-Hardware schadet das nichts: getTouch() liefert dann einfach nie
// eine Beruehrung, und die zweite Seite bleibt unerreichbar.
#define TOUCH_SDA   4
#define TOUCH_SCL   5
#define TOUCH_INT   0
#define TOUCH_RST  -1   // teilt sich beim C3-Modul den Reset mit dem Display

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01   _panel;
  lgfx::Bus_SPI        _bus;
  lgfx::Light_PWM      _light;
  lgfx::Touch_CST816S  _touch;
public:
  LGFX() {
    { auto c=_bus.config(); c.spi_host=SPI2_HOST; c.spi_mode=0;
      c.freq_write=40000000; c.freq_read=16000000;
      c.spi_3wire=true; c.use_lock=true; c.dma_channel=SPI_DMA_CH_AUTO;
      c.pin_sclk=6; c.pin_mosi=7; c.pin_miso=-1; c.pin_dc=2;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c=_panel.config(); c.pin_cs=10; c.pin_rst=1; c.pin_busy=-1;
      c.panel_width=240; c.panel_height=240; c.invert=true; c.rgb_order=false;
      _panel.config(c); }
    { auto c=_light.config(); c.pin_bl=3; c.invert=false;
      c.freq=44100; c.pwm_channel=7;
      _light.config(c); _panel.setLight(&_light); }
    { auto c=_touch.config();
      c.x_min=0; c.x_max=239; c.y_min=0; c.y_max=239;
      c.pin_int=TOUCH_INT; c.pin_rst=TOUCH_RST;
      c.bus_shared=false; c.offset_rotation=0;
      c.i2c_port=0; c.pin_sda=TOUCH_SDA; c.pin_scl=TOUCH_SCL;
      c.freq=400000; c.i2c_addr=0x15;
      _touch.config(c); _panel.setTouch(&_touch); }
    setPanel(&_panel);
  }
};
static LGFX lcd;
static LGFX_Sprite spr(&lcd);

#define C_WHITE  lcd.color888(255,255,255)
#define C_GRAY   lcd.color888(120,120,120)
#define C_RED    lcd.color888(255, 60, 60)
#define C_GREEN  lcd.color888(  0,255,120)
#define C_YELLOW lcd.color888(255,210,  0)
#define C_BLUE   lcd.color888(  0,170,255)
#define C_BLACK  lcd.color888(  0,  0,  0)
#define C_ORANGE lcd.color888(255,140,  0)

// ─────────────────────────────────────────────────────────────────────────────
// GLOBALE OBJEKTE
// ─────────────────────────────────────────────────────────────────────────────
Preferences prefs;
WebServer   server(80);
DNSServer   dns;

struct Config {
  String wifiSsid, wifiPass, ankerEmail, ankerPass;
  String siteId, siteName;
  // Teiler fuer die Rohwerte des Netzzaehlers. 0 = automatisch nach
  // Geraetetyp; ein Wert >0 ueberschreibt die Automatik dauerhaft.
  float  gridScale = 0;
  // Nutzbare Gesamtkapazitaet aller Akkus in Wh. Nur fuer die Umrechnung
  // Prozent -> Wattstunden; laesst sich in der Weboberflaeche setzen.
  int    battWh = BATT_CAP_WH;
  // Ausrichtung der Anzeige: 0/1/2/3 entspricht 0/90/180/270 Grad.
  int    rotation = 0;
  // Standort fuer die Wettervorhersage. 0/0 = noch nicht gesetzt, dann
  // bleibt die dritte Seite leer.
  float  lat = 0, lon = 0;
};
static Config cfg;

struct AnkerData {
  float solar_w=0, battery_wh=0, battery_pct=0;
  float home_w=0, grid_w=0, batt_in_w=0, batt_out_w=0;
  bool  valid=false;
};
static AnkerData gData;

static String        gAuthToken   = "";
static String        gGtoken      = "";
static String        gUserId      = "";
static String        gSiteId      = "";
static unsigned long gTokenExpiry = 0;
static uint8_t gSharedSecret[32];   // Session-Key: client_priv * server_session_pub
static uint8_t gPasswordKey[32];    // Login-Passwort-Key: client_priv * hardcoded_pub
static uint8_t gClientPrivKey[32];  // Client Private Key (roh, 32 Bytes)
static uint8_t gClientPubKey[65];
static uint8_t gHkdfKey[32];        // HKDF-SHA256(x, info="ecdh handshake")
static bool    gEcdhReady = false;
static String  gGeoKey    = "";     // geo_key aus Login-Antwort

// ── MQTT ────────────────────────────────────────────────────────────────────
static String gMqttHost, gMqttThing;          // Broker + Client-Kennung
static String gMqttCert, gMqttKey, gMqttCa;   // PEM, echte Zeilenumbrueche
static String gMqttCertId, gMqttUserId;       // fuer die Publish-client_id
static String gDevSn, gDevPn;                 // Solarbank der gewaehlten Anlage
static String gGridSn, gGridPn;               // Netzzaehler
// Teiler fuer die Rohwerte des Zaehlers – die Einheit ist geraeteabhaengig
static float  gGridScale = 1.0f;
// Leistung der vier MPPT-Eingaenge (0xc6..0xc9). Nur fuer die Weboberflaeche;
// auf dem 240x240-Display waere dafuer kein Platz.
static float  gPvStr[4] = {0,0,0,0};
// Tagesertrag je Panel in Wh. Entsteht durch Aufsummieren von Leistung mal
// Zeit – die Solarbank liefert nur Momentanwerte, keine Zaehlerstaende je
// Panel. Wird um Mitternacht zurueckgesetzt.
static double gPvWh[4]  = {0,0,0,0};

// ── Akkupacks ───────────────────────────────────────────────────────────────
// Die state_info-Nachricht vom Typ 0500 enthaelt je Pack einen Block (a4, a5,
// a6 …) mit Zellspannungen, Temperaturen und Seriennummer. Sie kommt selten,
// etwa zweimal in fuenf Minuten – fuer Spannungen und Temperaturen reicht das.
#define MAX_PACKS 6
struct PackInfo {
  bool     valid = false;
  uint8_t  idx   = 0;
  uint16_t soc10 = 0;       // Ladestand in Zehntelprozent (Offset 36)
  uint16_t unknown12 = 0;   // Offset 12: je Pack konstant, Bedeutung offen
  uint16_t cell[5] = {0,0,0,0,0};   // Millivolt
  int16_t  temp[4] = {0,0,0,0};     // Zehntelgrad
  String   sn;
  String   raw;             // gesamter Block als Hex, fuer die Zuordnung
};
static PackInfo gPacks[MAX_PACKS];
static int      gPackCount = 0;
// Aufbau der zuletzt empfangenen 0500-Nachricht, fuer die Fehlersuche
static String   gLastStateInfo;
static int    gEnergyDay = -1;        // tm_yday, fuer den gerade gezaehlt wird
static unsigned long gLastEnergyMs = 0;
static unsigned long gLastEnergySave = 0;
static WiFiClientSecure gMqttNet;
static PubSubClient     gMqtt(gMqttNet);
static float            gOutW          = 0;   // 0xad: Ausgang der Solarbank
static uint32_t         gMqttRxCount   = 0;
static unsigned long    gMqttLastTry   = 0;
static unsigned long    gMqttConnectedAt = 0; // Startpunkt der Lauschphase
static unsigned long    gLastTrigger   = 0;
static bool             gTriggerArmed  = false;

struct SiteEntry { String id; String name; };
static SiteEntry gSiteList[10];
static int       gSiteCount = 0;

static bool gFixMode = false;
static int      gPage        = 0;   // 0 = Messwerte, 1 = Netz/PV, 2 = Wetter
#define PAGES 3

// ── Wettervorhersage ────────────────────────────────────────────────────────
// Quelle: open-meteo.com - kostenlos, ohne Anmeldung und ohne Schluessel.
// Das ist der Grund fuer die Wahl: wer das Projekt nachbaut, muss sich
// nirgends registrieren.
struct WxDay { float tmax=0, tmin=0, sunH=0, cloud=0, rain=0; };
static WxDay         gWx[2];
static bool          gWxValid = false;
static unsigned long gWxLast  = 0;

// ─────────────────────────────────────────────────────────────────────────────
// HILFSFUNKTIONEN
// ─────────────────────────────────────────────────────────────────────────────
static String bytesToHex(const uint8_t* d, size_t n) {
  String s; s.reserve(n*2);
  for(size_t i=0;i<n;i++){char b[3];sprintf(b,"%02x",d[i]);s+=b;}
  return s;
}
static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
  if(strlen(hex)!=n*2) return false;
  for(size_t i=0;i<n;i++){char t[3]={hex[i*2],hex[i*2+1],0};out[i]=(uint8_t)strtol(t,nullptr,16);}
  return true;
}
static String md5Hex(const String& s) {
  uint8_t h[16];
  const mbedtls_md_info_t* info=mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
  mbedtls_md(info,(const uint8_t*)s.c_str(),s.length(),h);
  return bytesToHex(h,16);
}
static String b64Encode(const uint8_t* d, size_t n) {
  size_t len=0; mbedtls_base64_encode(nullptr,0,&len,d,n);
  uint8_t* buf=(uint8_t*)malloc(len+1); if(!buf) return "";
  mbedtls_base64_encode(buf,len,&len,d,n); buf[len]=0;
  String r=(char*)buf; free(buf); return r;
}

// HKDF-SHA256 mit salt=NULL, Ausgabe 32 Bytes (= eine Expand-Runde).
// RFC 5869: PRK = HMAC(salt=0x00*32, ikm); OKM = HMAC(PRK, info || 0x01)
static void hkdfSha256(const uint8_t* ikm, size_t ikmLen,
                       const char* info, uint8_t* out32) {
  const mbedtls_md_info_t* md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t salt[32]; memset(salt,0,32);
  uint8_t prk[32];
  mbedtls_md_hmac(md,salt,32,ikm,ikmLen,prk);
  size_t iLen=strlen(info);
  uint8_t* t=(uint8_t*)malloc(iLen+1);
  if(!t){memset(out32,0,32);return;}
  memcpy(t,info,iLen); t[iLen]=0x01;
  mbedtls_md_hmac(md,prk,32,t,iLen+1,out32);
  free(t);
}
static float jF(JsonVariant v) {
  if(v.isNull()) return 0;
  if(v.is<float>()) return v.as<float>();
  if(v.is<int>())   return (float)v.as<int>();
  if(v.is<const char*>()) {
    String s=v.as<String>(); s.replace("W",""); s.trim();
    return s.length()?s.toFloat():0;
  }
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// ECDH + VERSCHLUESSELUNG
// ─────────────────────────────────────────────────────────────────────────────
bool ecdhInit() {
  mbedtls_entropy_context  entropy;
  mbedtls_ctr_drbg_context rng;
  mbedtls_ecp_group        grp;
  mbedtls_mpi              privKey;
  mbedtls_ecp_point        pubKey,serverPt,sharedPt;
  mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng);
  mbedtls_ecp_group_init(&grp);   mbedtls_mpi_init(&privKey);
  mbedtls_ecp_point_init(&pubKey);mbedtls_ecp_point_init(&serverPt);
  mbedtls_ecp_point_init(&sharedPt);
  bool ok=false;
  do {
    if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(const uint8_t*)"anker",5)!=0) break;
    if(mbedtls_ecp_group_load(&grp,MBEDTLS_ECP_DP_SECP256R1)!=0) break;
    if(mbedtls_ecp_gen_keypair(&grp,&privKey,&pubKey,mbedtls_ctr_drbg_random,&rng)!=0) break;
    size_t len=0;
    if(mbedtls_ecp_point_write_binary(&grp,&pubKey,MBEDTLS_ECP_PF_UNCOMPRESSED,&len,gClientPubKey,65)!=0||len!=65) break;
    // Client Private Key sichern fuer spaetere ecdhUpdateShared()
    if(mbedtls_mpi_write_binary(&privKey,gClientPrivKey,32)!=0) break;
    // Passwort-Key aus hardcodiertem Server-Public-Key berechnen
    uint8_t serverPub[65];
    if(!hexToBytes(SERVER_PUBKEY_HEX,serverPub,65)) break;
    if(mbedtls_ecp_point_read_binary(&grp,&serverPt,serverPub,65)!=0) break;
    if(mbedtls_ecp_mul(&grp,&sharedPt,&privKey,&serverPt,mbedtls_ctr_drbg_random,&rng)!=0) break;
    uint8_t buf[65]; size_t blen=0;
    if(mbedtls_ecp_point_write_binary(&grp,&sharedPt,MBEDTLS_ECP_PF_UNCOMPRESSED,&blen,buf,65)!=0||blen!=65) break;
    memcpy(gPasswordKey,buf+1,32);
    memcpy(gSharedSecret,gPasswordKey,32);  // Platzhalter bis Login den echten Key liefert
    ok=true;
  } while(false);
  mbedtls_ecp_point_free(&sharedPt); mbedtls_ecp_point_free(&serverPt);
  mbedtls_ecp_point_free(&pubKey);   mbedtls_mpi_free(&privKey);
  mbedtls_ecp_group_free(&grp);      mbedtls_ctr_drbg_free(&rng);
  mbedtls_entropy_free(&entropy);
  gEcdhReady=ok;
  if(ok){
    Serial.printf("[ECDH] PrivKey  =%s\n",bytesToHex(gClientPrivKey,32).c_str());
    Serial.printf("[ECDH] ClientPub=%s\n",bytesToHex(gClientPubKey,65).c_str());
    Serial.printf("[ECDH] PwdKey   =%s\n",bytesToHex(gPasswordKey,32).c_str());
  } else Serial.println("[ECDH] FAILED");
  return ok;
}

// ECDH Shared Secret mit Server-Key aus Login-Response neu berechnen
// RNG-Callback fuer mbedtls_ecp_mul (benoetigt auf ESP32)
static int espRng(void*, unsigned char* buf, size_t len) {
  esp_fill_random(buf, len); return 0;
}

// ECDH Shared Secret mit Server-Key aus Login-Response neu berechnen
bool ecdhUpdateShared(const String& serverPubHex) {
  if(serverPubHex.length()!=130){
    Serial.printf("[ECDH] Falscher Server-Key len=%u\n",(unsigned)serverPubHex.length());
    return false;
  }
  mbedtls_ecp_group   grp;
  mbedtls_mpi         privKey;
  mbedtls_ecp_point   serverPt,sharedPt;
  mbedtls_ecp_group_init(&grp);  mbedtls_mpi_init(&privKey);
  mbedtls_ecp_point_init(&serverPt); mbedtls_ecp_point_init(&sharedPt);
  bool ok=false;
  int step=0;
  do {
    if(mbedtls_ecp_group_load(&grp,MBEDTLS_ECP_DP_SECP256R1)!=0){step=1;break;}
    if(mbedtls_mpi_read_binary(&privKey,gClientPrivKey,32)!=0){step=2;break;}
    uint8_t serverPub[65];
    if(!hexToBytes(serverPubHex.c_str(),serverPub,65)){step=3;break;}
    if(mbedtls_ecp_point_read_binary(&grp,&serverPt,serverPub,65)!=0){step=4;break;}
    if(mbedtls_ecp_mul(&grp,&sharedPt,&privKey,&serverPt,espRng,NULL)!=0){step=5;break;}
    uint8_t buf[65]; size_t blen=0;
    if(mbedtls_ecp_point_write_binary(&grp,&sharedPt,MBEDTLS_ECP_PF_UNCOMPRESSED,&blen,buf,65)!=0||blen!=65){step=6;break;}
    // Alle Key-Varianten fuer Python-Verifikation ausgeben
    Serial.printf("[ECDH] PrivKey=%s\n",    bytesToHex(gClientPrivKey,32).c_str());
    Serial.printf("[ECDH] xCoord=%s\n",     bytesToHex(buf+1,32).c_str());
    Serial.printf("[ECDH] yCoord=%s\n",     bytesToHex(buf+33,32).c_str());
    // Variante A: roh x-Koordinate
    uint8_t keyA[32]; memcpy(keyA,buf+1,32);
    // Variante B: SHA256(x)
    uint8_t keyB[32]; mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),buf+1,32,keyB);
    // Variante C: SHA256(x||y) – beide Koordinaten
    uint8_t keyC[32]; mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),buf+1,64,keyC);
    Serial.printf("[ECDH] KeyA(raw_x)  =%s\n",bytesToHex(keyA,32).c_str());
    Serial.printf("[ECDH] KeyB(sha256_x)=%s\n",bytesToHex(keyB,32).c_str());
    Serial.printf("[ECDH] KeyC(sha256_xy)=%s\n",bytesToHex(keyC,32).c_str());
    // Variante D: HKDF-SHA256(x, info="ecdh handshake") – wie anker-solix-api
    hkdfSha256(buf+1,32,"ecdh handshake",gHkdfKey);
    Serial.printf("[ECDH] KeyD(hkdf)   =%s\n",bytesToHex(gHkdfKey,32).c_str());
    memcpy(gSharedSecret,keyA,32);
    ok=true;
  } while(false);
  mbedtls_ecp_point_free(&sharedPt); mbedtls_ecp_point_free(&serverPt);
  mbedtls_mpi_free(&privKey);         mbedtls_ecp_group_free(&grp);
  if(ok) Serial.printf("[ECDH] SessionKey[:8]=%s\n",bytesToHex(gSharedSecret,8).c_str());
  else   Serial.printf("[ECDH] SessionKey FAILED step=%d\n",step);
  return ok;
}

// AES-256-CBC verschluesseln (fixer IV aus Secret) – nur fuer Passwort in Login
static String aesEncrypt(const String& plain) {
  if(!gEcdhReady) return "";
  size_t pwLen=plain.length(), padByte=16-(pwLen%16), total=pwLen+padByte;
  uint8_t* padded=(uint8_t*)malloc(total);
  uint8_t* enc   =(uint8_t*)malloc(total);
  if(!padded||!enc){free(padded);free(enc);return "";}
  memcpy(padded,plain.c_str(),pwLen);
  memset(padded+pwLen,(uint8_t)padByte,padByte);
  uint8_t iv[16]; memcpy(iv,gPasswordKey,16);
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes,gPasswordKey,256);
  mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_ENCRYPT,total,iv,padded,enc);
  mbedtls_aes_free(&aes); free(padded);
  String r=b64Encode(enc,total); free(enc); return r;
}

// AES-256-CBC verschluesseln fuer API-Body: zufaelliger IV, vorne angehaengt
// Ausgabe: Base64(IV[16] + Ciphertext)
static String aesEncryptBody(const String& plain) {
  if(!gEcdhReady) return "";
  size_t pwLen=plain.length(), padByte=16-(pwLen%16), total=pwLen+padByte;
  uint8_t iv[16]; esp_fill_random(iv,16);
  uint8_t* padded  =(uint8_t*)malloc(total);
  uint8_t* combined=(uint8_t*)malloc(16+total);
  if(!padded||!combined){free(padded);free(combined);return "";}
  memcpy(padded,plain.c_str(),pwLen);
  memset(padded+pwLen,(uint8_t)padByte,padByte);
  memcpy(combined,iv,16);
  // Echten IV VOR AES-Call drucken (aes_crypt_cbc ueberschreibt iv!)
  Serial.printf("[ENC] IV=%s\n",     bytesToHex(iv,16).c_str());
  Serial.printf("[ENC] Key[:8]=%s (HKDF/KeyD)\n", bytesToHex(gHkdfKey,8).c_str());
  Serial.printf("[ENC] Plain=%s\n",  plain.c_str());
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes,gHkdfKey,256);
  mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_ENCRYPT,total,iv,padded,combined+16);
  mbedtls_aes_free(&aes); free(padded);
  String r=b64Encode(combined,16+total); free(combined);
  Serial.printf("[ENC] Body=%s\n",   r.c_str());
  return r;
}

// AES-256-CBC entschluesseln: IV steht in den ersten 16 Bytes der Daten
static String aesDecrypt(const String& b64) {
  if(!gEcdhReady||b64.isEmpty()) return "";
  size_t bufLen=0;
  mbedtls_base64_decode(nullptr,0,&bufLen,(const uint8_t*)b64.c_str(),b64.length());
  if(bufLen<32) return "";
  uint8_t* combined=(uint8_t*)malloc(bufLen);
  uint8_t* dec     =(uint8_t*)malloc(bufLen);
  if(!combined||!dec){free(combined);free(dec);return "";}
  size_t actualLen=0;
  mbedtls_base64_decode(combined,bufLen,&actualLen,(const uint8_t*)b64.c_str(),b64.length());
  if(actualLen<32||(actualLen-16)%16!=0){
    Serial.printf("[Decrypt] Bad len %u\n",(unsigned)actualLen);
    free(combined);free(dec);return "";
  }
  uint8_t iv[16]; memcpy(iv,combined,16);
  size_t cipherLen=actualLen-16;
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes,gHkdfKey,256);
  mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_DECRYPT,cipherLen,iv,combined+16,dec);
  mbedtls_aes_free(&aes); free(combined);
  uint8_t padByte=dec[cipherLen-1];
  if(padByte==0||padByte>16) padByte=0;
  size_t plainLen=cipherLen-padByte;
  dec[plainLen]=0;
  String result=(char*)dec; free(dec);
  Serial.printf("[Decrypt] %u->%u bytes\n",(unsigned)actualLen,(unsigned)plainLen);
  return result;
}

// Extrahiert "data"-Wert aus {"trace_id":"...","data":"<base64>"}
static String extractDataField(const String& json) {
  const char* key="\"data\":\"";
  int idx=json.indexOf(key);
  if(idx<0) return "";
  idx+=strlen(key);
  int end=json.indexOf('"',idx);
  if(end<0) return "";
  return json.substring(idx,end);
}

// ─────────────────────────────────────────────────────────────────────────────
// KONFIG
// ─────────────────────────────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("anker",true);
  cfg.wifiSsid  =prefs.getString("wssid",""); cfg.wifiPass  =prefs.getString("wpass","");
  cfg.ankerEmail=prefs.getString("email",""); cfg.ankerPass =prefs.getString("apass","");
  cfg.siteId    =prefs.getString("siteid","");
  cfg.siteName  =prefs.getString("sitename","");
  cfg.gridScale =prefs.getFloat("gridscale",0);
  cfg.battWh    =prefs.getInt("battwh",BATT_CAP_WH);
  cfg.rotation  =prefs.getInt("rot",0);
  cfg.lat       =prefs.getFloat("lat",0);
  cfg.lon       =prefs.getFloat("lon",0);
  prefs.end();
  Serial.printf("[Prefs] SSID=%s Email=%s Site=%s BattCap=%dWh\n",
    cfg.wifiSsid.c_str(),cfg.ankerEmail.c_str(),cfg.siteName.c_str(),cfg.battWh);
}
void saveConfig() {
  prefs.begin("anker",false);
  prefs.putString("wssid",cfg.wifiSsid); prefs.putString("wpass",cfg.wifiPass);
  prefs.putString("email",cfg.ankerEmail); prefs.putString("apass",cfg.ankerPass);
  prefs.putString("siteid",cfg.siteId); prefs.putString("sitename",cfg.siteName);
  prefs.putFloat("gridscale",cfg.gridScale);
  prefs.putInt("battwh",cfg.battWh);
  prefs.putInt("rot",cfg.rotation);
  prefs.putFloat("lat",cfg.lat);
  prefs.putFloat("lon",cfg.lon);
  prefs.end(); Serial.println("[Prefs] OK");
}
void clearConfig(){prefs.begin("anker",false);prefs.clear();prefs.end();}
bool configComplete(){return cfg.wifiSsid.length()>0&&cfg.ankerEmail.length()>0;}
bool siteSelected()  {return cfg.siteId.length()>0;}

// ─────────────────────────────────────────────────────────────────────────────
// DISPLAY HILFE
// ─────────────────────────────────────────────────────────────────────────────
void dispCenter(int y,const char* txt,uint32_t col,const lgfx::IFont* font){
  lcd.setFont(font);lcd.setTextColor(col,C_BLACK);
  lcd.setTextDatum(lgfx::TC_DATUM);lcd.drawString(txt,120,y);
}
void dispMsg(const char* l1,const char* l2="",uint32_t c1=0,uint32_t c2=0){
  lcd.fillScreen(C_BLACK); if(!c1)c1=C_WHITE; if(!c2)c2=C_GRAY;
  dispCenter(95,l1,c1,&fonts::FreeSansBold12pt7b);
  if(strlen(l2))dispCenter(130,l2,c2,&fonts::FreeSans9pt7b);
}

// ─────────────────────────────────────────────────────────────────────────────
// CONFIG-PORTAL HTML
// ─────────────────────────────────────────────────────────────────────────────
String urlDecode(const String& s){
  String out; out.reserve(s.length());
  for(int i=0;i<(int)s.length();i++){
    char c=s[i];
    if(c=='+')out+=' ';
    else if(c=='%'&&i+2<(int)s.length()){
      auto h=[](char x)->int{if(x>='0'&&x<='9')return x-'0';if(x>='A'&&x<='F')return x-'A'+10;if(x>='a'&&x<='f')return x-'a'+10;return 0;};
      out+=(char)(h(s[i+1])*16+h(s[i+2]));i+=2;
    }else out+=c;
  }
  return out;
}

const char HTML_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anker Display Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;display:flex;justify-content:center;padding:20px}
.card{background:#1a1a1a;border-radius:16px;padding:28px;width:100%;max-width:420px;box-shadow:0 4px 24px #0008}
h1{font-size:1.4rem;margin-bottom:6px;color:#fff}.sub{color:#888;font-size:.85rem;margin-bottom:24px}
.section{background:#111;border-radius:10px;padding:16px;margin-bottom:16px}
.section h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;color:#f0a500;margin-bottom:12px}
label{display:block;font-size:.85rem;color:#aaa;margin-bottom:4px;margin-top:10px}label:first-of-type{margin-top:0}
input{width:100%;padding:10px 12px;background:#222;border:1px solid #333;border-radius:8px;color:#fff;font-size:.95rem;outline:none}
input:focus{border-color:#f0a500}
.pw{position:relative}.pw input{padding-right:44px}
.pw .eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:36px;height:36px;
background:none;border:none;color:#888;font-size:1.1rem;cursor:pointer;padding:0;margin:0}
button{width:100%;padding:14px;background:#f0a500;border:none;border-radius:10px;color:#000;font-size:1rem;font-weight:700;cursor:pointer;margin-top:8px}
</style>
<script>
function tg(id,btn){var i=document.getElementById(id);
if(i.type==='password'){i.type='text';btn.textContent='🙈';}
else{i.type='password';btn.textContent='👁';}}
</script></head><body><div class="card">
<h1>&#9889; Anker Display Setup</h1><p class="sub">Zugangsdaten konfigurieren</p>
<form method="POST" action="/save">
  <div class="section"><h2>&#128246; WLAN</h2>
    <label>SSID</label><input name="wssid" type="text" value="__WSSID__" required>
    <label>Passwort</label>
    <div class="pw"><input id="wp" name="wpass" type="password" value="">
    <button type="button" class="eye" onclick="tg('wp',this)">&#128065;</button></div></div>
  <div class="section"><h2>&#128267; Anker Cloud</h2>
    <label>E-Mail</label><input name="email" type="email" value="__EMAIL__" required>
    <label>Passwort</label>
    <div class="pw"><input id="ap" name="apass" type="password" value="" required>
    <button type="button" class="eye" onclick="tg('ap',this)">&#128065;</button></div></div>
  <button type="submit">&#128190; Speichern &amp; Neustart</button>
</form></div></body></html>
)HTML";

const char HTML_PAGE_FIX[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="de"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anker Login korrigieren</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;display:flex;justify-content:center;padding:20px}
.card{background:#1a1a1a;border-radius:16px;padding:28px;width:100%;max-width:420px;box-shadow:0 4px 24px #0008}
h1{font-size:1.4rem;margin-bottom:6px;color:#fff}.sub{color:#f66;font-size:.85rem;margin-bottom:24px}
.section{background:#111;border-radius:10px;padding:16px;margin-bottom:16px}
.section h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.1em;color:#f0a500;margin-bottom:12px}
label{display:block;font-size:.85rem;color:#aaa;margin-bottom:4px;margin-top:10px}label:first-of-type{margin-top:0}
input{width:100%;padding:10px 12px;background:#222;border:1px solid #333;border-radius:8px;color:#fff;font-size:.95rem;outline:none}
input:focus{border-color:#f0a500}
.pw{position:relative}.pw input{padding-right:44px}
.pw .eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:36px;height:36px;
background:none;border:none;color:#888;font-size:1.1rem;cursor:pointer;padding:0;margin:0}
button{width:100%;padding:14px;background:#f0a500;border:none;border-radius:10px;color:#000;font-size:1rem;font-weight:700;cursor:pointer;margin-top:8px}
</style>
<script>
function tg(id,btn){var i=document.getElementById(id);
if(i.type==='password'){i.type='text';btn.textContent='🙈';}
else{i.type='password';btn.textContent='👁';}}
</script></head><body><div class="card">
<h1>&#9889; Anker Login korrigieren</h1>
<p class="sub">Login fehlgeschlagen &ndash; bitte Zugangsdaten pruefen</p>
<form method="POST" action="/save">
  <div class="section"><h2>&#128267; Anker Cloud</h2>
    <label>E-Mail</label><input name="email" type="email" value="__EMAIL__" required>
    <label>Passwort</label>
    <div class="pw"><input id="ap" name="apass" type="password" value="" required>
    <button type="button" class="eye" onclick="tg('ap',this)">&#128065;</button></div></div>
  <button type="submit">&#128190; Speichern &amp; Verbinden</button>
</form></div></body></html>
)HTML";

const char HTML_SAVED[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="UTF-8">
<style>body{font-family:sans-serif;background:#0a0a0a;color:#eee;display:flex;justify-content:center;align-items:center;height:100vh}
.c{background:#1a3a1a;border:1px solid #2a6a2a;border-radius:16px;padding:40px;text-align:center;max-width:340px}
h1{color:#4caf50;font-size:2rem;margin-bottom:12px}p{color:#aaa}</style>
</head><body><div class="c"><h1>&#10004;</h1><h2>Gespeichert!</h2><p>ESP startet in 3s neu.</p></div></body></html>
)HTML";

String buildSiteSelectPage() {
  String p = F("<!DOCTYPE html><html lang='de'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Anlage waehlen</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;display:flex;justify-content:center;padding:20px}"
    ".card{background:#1a1a1a;border-radius:16px;padding:28px;width:100%;max-width:420px;box-shadow:0 4px 24px #0008}"
    "h1{font-size:1.4rem;margin-bottom:6px;color:#fff}.sub{color:#888;font-size:.85rem;margin-bottom:24px}"
    ".site-btn{display:block;width:100%;padding:16px;background:#111;border:1px solid #2a2a3a;"
    "border-radius:12px;color:#fff;text-decoration:none;margin-bottom:12px;text-align:left;"
    "cursor:pointer;transition:border-color .2s}"
    ".site-btn:hover{border-color:#f0a500}"
    ".site-name{font-size:1rem;font-weight:600;color:#fff}"
    ".site-id{font-size:.72rem;color:#555;margin-top:4px;word-break:break-all}"
    "</style></head><body><div class='card'>"
    "<h1>&#128267; Anlage waehlen</h1>"
    "<p class='sub'>Welche Anlage soll angezeigt werden?</p>");
  for(int i=0;i<gSiteCount;i++){
    p += "<a class='site-btn' href='/selectsite?id=";
    p += gSiteList[i].id;
    p += "'><div class='site-name'>&#9889; ";
    p += gSiteList[i].name;
    p += "</div><div class='site-id'>";
    p += gSiteList[i].id;
    p += "</div></a>";
  }
  if(gSiteCount==0)
    p += "<p style='color:#888'>Keine Anlagen gefunden. Zugangsdaten pruefen.</p>";
  p += "</div></body></html>";
  return p;
}

void handleRoot(){
  if(gFixMode || WiFi.status()==WL_CONNECTED){
    String p=FPSTR(HTML_PAGE_FIX); p.replace("__EMAIL__",cfg.ankerEmail);
    server.send(200,"text/html; charset=utf-8",p); return;
  }
  String p=FPSTR(HTML_PAGE);
  p.replace("__WSSID__",cfg.wifiSsid); p.replace("__EMAIL__",cfg.ankerEmail);
  server.send(200,"text/html; charset=utf-8",p);
}
// Vorwaertsdeklarationen – werden von den Handlern weiter unten gebraucht,
// sind aber erst spaeter im Sketch definiert.
String httpsPost(const String& path, const String& body,
                 const String& token="", const String& gtoken="",
                 bool encrypt=false);
static int rawPost(const String& path, const String& body, String& outResp);
void drawDisplay();
bool fetchWeather();
static void applyGridScale();
void handleSave();

// Holt die Anlagenliste frisch von Anker. Im Normalbetrieb ist gSiteList
// leer, weil sie sonst nur beim Einrichten gefuellt wird.
bool loadSiteList(){
  String resp=httpsPost("power_service/v1/site/get_site_list",
                        "{\"page\":1,\"size\":10}",gAuthToken,gGtoken,false);
  if(resp.isEmpty()) return false;
  DynamicJsonDocument doc(4096);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok) return false;
  gSiteCount=0;
  for(auto s:doc["data"]["site_list"].as<JsonArray>()){
    if(gSiteCount>=10) break;
    gSiteList[gSiteCount].id  =s["site_id"].as<String>();
    gSiteList[gSiteCount].name=s["site_name"].as<String>();
    gSiteCount++;
  }
  Serial.printf("[Web] %d Anlagen geladen\n",gSiteCount);
  return gSiteCount>0;
}

void handleSites(){
  if(gSiteCount==0) loadSiteList();     // im Normalbetrieb erst nachladen
  server.send(200,"text/html; charset=utf-8",buildSiteSelectPage());
}
void handleSelectSite(){
  String id=server.arg("id"); String name="";
  for(int i=0;i<gSiteCount;i++) if(gSiteList[i].id==id){name=gSiteList[i].name;break;}
  if(id.length()==0){server.sendHeader("Location","/sites");server.send(302);return;}
  cfg.siteId=id; cfg.siteName=name; saveConfig();
  server.send(200,"text/html; charset=utf-8",FPSTR(HTML_SAVED));
  delay(2500); ESP.restart();
}

// Startseite im Normalbetrieb: Messwerte plus Zugang zur Anlagenauswahl.
// Im Einrichtungsmodus zeigt "/" dagegen das Zugangsdaten-Formular.
void handleStatus(){
  // Grosszuegig bemessen: die Vorlage samt CSS liegt bei rund 1500 Zeichen,
  // dazu Anlagenname und Messwerte. snprintf wuerde sonst kommentarlos kuerzen.
  // static, nicht auf dem Stack: der Task-Stack ist knapp bemessen, und der
  // Webserver ruft die Funktion ohnehin nie verschachtelt auf.
  static char b[5120];
  const char* mq = gMqtt.connected() ? "verbunden" : "getrennt";
  snprintf(b,sizeof(b),
    "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta http-equiv='refresh' content='10'>"
    "<title>%s &ndash; Anker Display</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;"
    "display:flex;justify-content:center;padding:20px}"
    ".card{background:#1a1a1a;border-radius:16px;padding:26px;width:100%%;max-width:420px}"
    "h1{font-size:1.3rem;margin-bottom:2px}.sub{color:#888;font-size:.85rem;margin-bottom:22px}"
    "table{width:100%%;border-collapse:collapse;margin-bottom:22px}"
    "td{padding:9px 0;border-bottom:1px solid #262626;font-size:.95rem}"
    "td:last-child{text-align:right;font-weight:600}"
    "tr:last-child td{border-bottom:none}"
    ".w{color:#f0a500}.g{color:#4caf50}.r{color:#f66}"
    "td.m{text-align:right;color:#888;font-weight:400;width:5.5em}"
    "a.btn{display:block;padding:13px;background:#f0a500;color:#000;text-align:center;"
    "border-radius:10px;text-decoration:none;font-weight:700;margin-bottom:10px}"
    "a.sec{display:block;padding:11px;background:#222;color:#aaa;text-align:center;"
    "border-radius:10px;text-decoration:none;font-size:.9rem}"
    "</style></head><body><div class='card'>"
    "<h1>&#9889; %s</h1><p class='sub'>MQTT %s &middot; Firmware %s</p>"
    "<table>"
    "<tr><td>Solar</td><td class='w'>%.0f W</td></tr>"
    "<tr><td>Akku</td><td>%.0f %%</td></tr>"
    "<tr><td>Akkuleistung</td><td class='%s'>%.0f W</td></tr>"
    "<tr><td>Netz</td><td class='%s'>%.0f W</td></tr>"
    "<tr><td>Hausverbrauch</td><td>%.0f W</td></tr>"
    "</table>"
    "<h2 style='font-size:.72rem;text-transform:uppercase;letter-spacing:.09em;"
    "color:#f0a500;margin-bottom:8px'>Einzelne Panels &middot; heute</h2>"
    "<table style='margin-bottom:18px'>"
    "<tr><td>Panel 1</td><td class='m'>%.0f W</td><td class='w'>%.2f kWh</td></tr>"
    "<tr><td>Panel 2</td><td class='m'>%.0f W</td><td class='w'>%.2f kWh</td></tr>"
    "<tr><td>Panel 3</td><td class='m'>%.0f W</td><td class='w'>%.2f kWh</td></tr>"
    "<tr><td>Panel 4</td><td class='m'>%.0f W</td><td class='w'>%.2f kWh</td></tr>"
    "<tr><td><b>Summe</b></td><td class='m'>%.0f W</td>"
    "<td class='w'><b>%.2f kWh</b></td></tr>"
    "</table>"
    "<p style='color:#888;font-size:.8rem;margin-bottom:8px'>"
    "Netzwert falsch? Teiler f&uuml;r %s: "
    "<a style='color:#f0a500' href='/gridscale?v=1'>1</a> &middot; "
    "<a style='color:#f0a500' href='/gridscale?v=10'>10</a> &middot; "
    "<a style='color:#f0a500' href='/gridscale?v=100'>100</a> &middot; "
    "<a style='color:#f0a500' href='/gridscale?v=1000'>1000</a> "
    "(aktuell %.0f)</p>"
    "<p style='color:#888;font-size:.8rem;margin-bottom:12px'>"
    "Akkukapazit&auml;t gesamt: "
    "<form style='display:inline' action='/battwh'>"
    "<input name='v' type='number' value='%d' min='100' max='60000' step='100' "
    "style='width:6.5em;padding:4px 6px;background:#222;border:1px solid #333;"
    "border-radius:6px;color:#eee'> Wh "
    "<button style='padding:5px 10px;background:#333;border:none;border-radius:6px;"
    "color:#f0a500;cursor:pointer'>setzen</button></form></p>"
    "<p style='color:#888;font-size:.8rem;margin-bottom:12px'>"
    "Anzeige drehen: "
    "<a style='color:#f0a500' href='/rotate?v=0'>0&deg;</a> &middot; "
    "<a style='color:#f0a500' href='/rotate?v=1'>90&deg;</a> &middot; "
    "<a style='color:#f0a500' href='/rotate?v=2'>180&deg;</a> &middot; "
    "<a style='color:#f0a500' href='/rotate?v=3'>270&deg;</a> "
    "(aktuell %d&deg;)</p>"
    "<p style='color:#888;font-size:.8rem;margin-bottom:12px'>"
    "Standort f&uuml;r die Wettervorhersage: "
    "<form style='display:inline' action='/geo'>"
    "<input name='lat' type='number' step='0.0001' value='%.4f' placeholder='Breite' "
    "style='width:6.5em;padding:4px 6px;background:#222;border:1px solid #333;"
    "border-radius:6px;color:#eee'> "
    "<input name='lon' type='number' step='0.0001' value='%.4f' placeholder='Laenge' "
    "style='width:6.5em;padding:4px 6px;background:#222;border:1px solid #333;"
    "border-radius:6px;color:#eee'> "
    "<button style='padding:5px 10px;background:#333;border:none;border-radius:6px;"
    "color:#f0a500;cursor:pointer'>setzen</button></form></p>"
    "<a class='btn' href='/akkus'>Akkupacks im Detail</a>"
    "<a class='sec' href='/sites'>Anlage wechseln</a>"
    "<a class='sec' href='/setup'>Zugangsdaten &auml;ndern</a>"
    "</div></body></html>",
    cfg.siteName.c_str(), cfg.siteName.c_str(), mq, FW_VERSION,
    gData.solar_w, gData.battery_pct,
    gData.batt_out_w>0.5f?"r":"g",
    gData.batt_in_w>0.5f?gData.batt_in_w:gData.batt_out_w,
    gData.grid_w>0.5f?"r":"g", fabsf(gData.grid_w),
    gData.home_w,
    gPvStr[0], gPvWh[0]/1000.0, gPvStr[1], gPvWh[1]/1000.0,
    gPvStr[2], gPvWh[2]/1000.0, gPvStr[3], gPvWh[3]/1000.0,
    gPvStr[0]+gPvStr[1]+gPvStr[2]+gPvStr[3],
    (gPvWh[0]+gPvWh[1]+gPvWh[2]+gPvWh[3])/1000.0,
    gGridPn.length()?gGridPn.c_str():"Zaehler", gGridScale,
    cfg.battWh, cfg.rotation*90, cfg.lat, cfg.lon);
  server.send(200,"text/html; charset=utf-8",b);
}

// Detailseite der Akkupacks. Bewusst mit allen Rohdaten: welcher Wert welche
// Bedeutung hat, ist nur teilweise geklaert – der Hexblock erlaubt den
// Abgleich mit der App, ohne dass dafuer neu geflasht werden muss.
void handlePacks(){
  String h;
  h.reserve(4096);
  h += F("<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<meta http-equiv='refresh' content='30'>"
         "<title>Akkupacks</title><style>"
         "*{box-sizing:border-box;margin:0;padding:0}"
         "body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;"
         "display:flex;justify-content:center;padding:20px}"
         ".card{background:#1a1a1a;border-radius:16px;padding:24px;width:100%;max-width:560px}"
         "h1{font-size:1.25rem;margin-bottom:18px}"
         "h2{font-size:.78rem;text-transform:uppercase;letter-spacing:.09em;"
         "color:#f0a500;margin:20px 0 8px}"
         "table{width:100%;border-collapse:collapse;margin-bottom:10px}"
         "td{padding:7px 0;border-bottom:1px solid #262626;font-size:.9rem}"
         "td:last-child{text-align:right;font-weight:600}"
         "tr:last-child td{border-bottom:none}"
         ".hex{font-family:ui-monospace,Menlo,monospace;font-size:.62rem;"
         "color:#666;word-break:break-all;line-height:1.5;margin-bottom:6px}"
         ".sub{color:#888;font-size:.85rem;margin-bottom:18px;line-height:1.55}"
         "a{color:#f0a500}</style></head><body><div class='card'>"
         "<h1>&#128267; Akkupacks</h1>");

  if(gPackCount==0){
    h += F("<p class='sub'>Noch keine Daten. Die Nachricht mit den Packdaten "
           "kommt nur etwa alle zwei bis drei Minuten &ndash; nach einem "
           "Neustart dauert es also einen Moment.</p>");
  } else {
    h += F("<p class='sub'>Ladestand, Zellspannungen und Temperaturen sind "
           "gegen die Anker-App gepr&uuml;ft. Der Wert bei Offset 12 bleibt je "
           "Pack konstant und ist ungekl&auml;rt &ndash; wer eine Idee hat, "
           "meldet sich unter esp32.display@gmail.com.</p>");
    // Ueber alle Plaetze laufen: die Packs stehen nach Index einsortiert,
    // es koennen also Luecken bestehen, solange noch nicht jedes gemeldet hat.
    for(int p=0;p<MAX_PACKS;p++){
      PackInfo& q=gPacks[p];
      if(!q.valid) continue;
      h += "<h2>Pack "+String(q.idx);
      if(q.sn.length()) h += " &middot; "+q.sn;
      h += "</h2><table>";
      h += "<tr><td><b>Ladestand</b></td><td>"+String(q.soc10/10.0,1)+" %</td></tr>";
      uint32_t sum=0; uint16_t lo=65535, hi=0;
      for(int i=0;i<5;i++){
        h += "<tr><td>Zelle "+String(i+1)+"</td><td>"+String(q.cell[i])+" mV</td></tr>";
        sum+=q.cell[i];
        if(q.cell[i]<lo) lo=q.cell[i];
        if(q.cell[i]>hi) hi=q.cell[i];
      }
      h += "<tr><td>Summe</td><td>"+String(sum/1000.0,2)+" V</td></tr>";
      h += "<tr><td>Spreizung</td><td>"+String(hi-lo)+" mV</td></tr>";
      for(int i=0;i<4;i++)
        h += "<tr><td>Temperatur "+String(i+1)+"</td><td>"+String(q.temp[i]/10.0,1)+" &deg;C</td></tr>";
      h += "<tr><td>Offset 12 (konstant je Pack)</td><td>"+String(q.unknown12)+"</td></tr>";
      h += "</table><div class='hex'>"+q.raw+"</div>";
    }
  }
  h += F("<h2>Aufbau der letzten Nachricht</h2>");
  h += "<div class='hex'>" + (gLastStateInfo.length()?gLastStateInfo:String("noch keine empfangen")) + "</div>";
  h += F("<p class='sub' style='margin-top:10px'>Jeder Eintrag ist "
         "<i>tag:laenge/typ</i>. Packbloecke haben Typ 04 und sind laenger "
         "als 32 Byte.</p>");
  h += F("<p style='margin-top:18px'><a href='/'>&larr; zur&uuml;ck</a></p>"
         "</div></body></html>");
  server.send(200,"text/html; charset=utf-8",h);
}

// Teiler des Netzzaehlers von Hand setzen. Noetig, weil die Einheit je
// Geraet verschieden ist und wir nicht jeden Zaehler kennen koennen.
// Ausrichtung der Anzeige drehen. Wirkt sofort - das Sprite ist quadratisch,
// die Abmessungen aendern sich also nicht und es muss nichts neu angelegt
// werden. Ein Neustart waere nur laestig.
void handleRotate(){
  int v=server.arg("v").toInt();
  if(v>=0 && v<=3){
    cfg.rotation=v; saveConfig();
    lcd.setRotation(v);
    lcd.fillScreen(C_BLACK);
    drawDisplay();
    Serial.printf("[LCD] Ausrichtung %d Grad\n", v*90);
  }
  server.sendHeader("Location","/"); server.send(302);
}

// Standort fuer die Wettervorhersage setzen und sofort abrufen.
void handleGeo(){
  float la=server.arg("lat").toFloat(), lo=server.arg("lon").toFloat();
  if(la>=-90 && la<=90 && lo>=-180 && lo<=180 && (la!=0 || lo!=0)){
    cfg.lat=la; cfg.lon=lo; saveConfig();
    Serial.printf("[WX] Standort %.4f / %.4f\n",la,lo);
    fetchWeather();
    gWxLast=millis();
  }
  server.sendHeader("Location","/"); server.send(302);
}

void handleBattWh(){
  int v=server.arg("v").toInt();
  if(v>=100 && v<=60000){
    cfg.battWh=v; saveConfig();
    Serial.printf("[AKKU] Kapazitaet auf %d Wh gesetzt\n",v);
  }
  server.sendHeader("Location","/"); server.send(302);
}

void handleGridScale(){
  float v=server.arg("v").toFloat();
  if(v>0){
    cfg.gridScale=v; saveConfig(); applyGridScale();
    Serial.printf("[NETZ] Teiler von Hand auf %.0f gesetzt\n",v);
  }
  server.sendHeader("Location","/"); server.send(302);
}

// Webserver im Normalbetrieb starten, damit die Anlage ohne Reset und ohne
// neues Flashen gewechselt werden kann.
void startWebUi(){
  server.on("/",           HTTP_GET,  handleStatus);
  server.on("/setup",      HTTP_GET,  handleRoot);
  server.on("/save",       HTTP_POST, handleSave);
  server.on("/sites",      HTTP_GET,  handleSites);
  server.on("/selectsite", HTTP_GET,  handleSelectSite);
  server.on("/gridscale",  HTTP_GET,  handleGridScale);
  server.on("/akkus",      HTTP_GET,  handlePacks);
  server.on("/battwh",     HTTP_GET,  handleBattWh);
  server.on("/geo",        HTTP_GET,  handleGeo);
  server.on("/rotate",     HTTP_GET,  handleRotate);
  server.onNotFound([](){ server.sendHeader("Location","/"); server.send(302); });
  server.begin();
  Serial.printf("[Web] http://%s/\n",WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// TAGESERTRAG JE PANEL
// Die Solarbank liefert nur Momentanleistungen, deshalb wird selbst summiert:
// Energie += Leistung * verstrichene Zeit. Zwischenstand alle 10 Minuten ins
// NVS, damit ein Neustart am Nachmittag nicht den ganzen Vormittag verwirft.
// ─────────────────────────────────────────────────────────────────────────────
static void saveEnergy(){
  prefs.begin("energy",false);
  prefs.putInt("day",gEnergyDay);
  for(int i=0;i<4;i++){
    char k[4]; snprintf(k,sizeof(k),"p%d",i);
    prefs.putFloat(k,(float)gPvWh[i]);
  }
  prefs.end();
}

static void loadEnergy(){
  prefs.begin("energy",true);
  gEnergyDay=prefs.getInt("day",-1);
  for(int i=0;i<4;i++){
    char k[4]; snprintf(k,sizeof(k),"p%d",i);
    gPvWh[i]=prefs.getFloat(k,0);
  }
  prefs.end();
  // Gespeicherten Stand nur uebernehmen, wenn es noch derselbe Tag ist
  struct tm ti;
  if(getLocalTime(&ti) && gEnergyDay!=ti.tm_yday){
    for(int i=0;i<4;i++) gPvWh[i]=0;
    gEnergyDay=ti.tm_yday;
  }
  Serial.printf("[Wh] Tagesstand %.0f/%.0f/%.0f/%.0f Wh\n",
                gPvWh[0],gPvWh[1],gPvWh[2],gPvWh[3]);
}

static void integrateEnergy(){
  struct tm ti;
  if(!getLocalTime(&ti)) return;      // ohne Uhrzeit kein Tagesbezug moeglich
  if(gEnergyDay!=ti.tm_yday){         // Tageswechsel um Mitternacht
    Serial.printf("[Wh] Tageswechsel – Ertrag war %.0f/%.0f/%.0f/%.0f Wh\n",
                  gPvWh[0],gPvWh[1],gPvWh[2],gPvWh[3]);
    for(int i=0;i<4;i++) gPvWh[i]=0;
    gEnergyDay=ti.tm_yday;
    gLastEnergyMs=0;
    saveEnergy();
  }
  unsigned long now=millis();
  if(gLastEnergyMs){
    unsigned long dt=now-gLastEnergyMs;   // Ueberlauf ist hier unkritisch
    // Bei einer groesseren Luecke – etwa nach WLAN-Ausfall – nicht schaetzen:
    // die aktuelle Leistung ueber eine Stunde hochzurechnen waere schlicht
    // falsch. Lieber ein Loch im Zaehler als ein erfundener Wert.
    if(dt<=60000){
      double h=dt/3600000.0;
      for(int i=0;i<4;i++) gPvWh[i]+=gPvStr[i]*h;
    }
  }
  gLastEnergyMs=now;
  if(now-gLastEnergySave>=600000){     // alle 10 Minuten sichern
    gLastEnergySave=now;
    saveEnergy();
  }
}

// Vorwaertsdeklarationen (httpsPost steht schon weiter oben)
bool ankerLogin();
bool fetchMqttCreds();
bool fetchDeviceInfo();
bool mqttConnect();
void sendRealtimeTrigger(uint16_t timeoutSec);
static void printLong(const char* tag, const String& s);
bool ecdhInit();

void handleSave(){
  if(server.method()==HTTP_POST){
    if(server.hasArg("wssid")&&server.arg("wssid").length()>0){
      cfg.wifiSsid=urlDecode(server.arg("wssid"));
      cfg.wifiPass=urlDecode(server.arg("wpass"));
    }
    cfg.ankerEmail=urlDecode(server.arg("email"));
    cfg.ankerPass =urlDecode(server.arg("apass"));
    saveConfig();
    if(gFixMode){
      server.send(200,"text/html; charset=utf-8",
        F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
          "<style>body{font-family:sans-serif;background:#0a0a0a;color:#eee;"
          "display:flex;justify-content:center;align-items:center;height:100vh;padding:20px}"
          ".c{text-align:center;max-width:360px}h2{color:#f0a500}p{color:#aaa;margin-top:12px;line-height:1.5}"
          "a{color:#4caf50;font-size:1.1rem;font-weight:700}"
          "</style></head><body><div class='c'>"
          "<h2>&#8987; Pruefe Anker-Login...</h2>"
          "<p>Warte ca. 10 Sekunden:</p>"
          "<p><a href='/sites'>Anlage auswaehlen</a></p>"
          "</div></body></html>"));
    } else {
      server.send(200,"text/html; charset=utf-8",
        F("<!DOCTYPE html><html><head><meta charset='UTF-8'>"
          "<style>body{font-family:sans-serif;background:#0a0a0a;color:#eee;"
          "display:flex;justify-content:center;align-items:center;height:100vh;padding:20px}"
          ".c{text-align:center;max-width:360px}h2{color:#f0a500}p{color:#aaa;margin-top:12px;line-height:1.5}"
          "</style></head><body><div class='c'>"
          "<h2>&#8987; Verbinde mit Internet...</h2>"
          "<p>Schau aufs Display – dort erscheint die IP.</p>"
          "</div></body></html>"));
    }
    delay(300);
    if(WiFi.status()!=WL_CONNECTED){
      WiFi.mode(WIFI_AP_STA); delay(200);
      WiFi.begin(cfg.wifiSsid.c_str(),cfg.wifiPass.c_str());
      int tries=0;
      while(WiFi.status()!=WL_CONNECTED&&tries<40){delay(500);Serial.print(".");tries++;}
      Serial.println();
    }
    if(WiFi.status()!=WL_CONNECTED){
      dispMsg("WLAN-Fehler!","Neu starten & Setup wiederholen",C_RED,C_YELLOW); return;
    }
    String myIp=WiFi.localIP().toString();
    lcd.fillScreen(C_BLACK);
    dispCenter( 95,"Melde bei Anker an...",C_GREEN,&fonts::FreeSansBold12pt7b);
    dispCenter(125,cfg.ankerEmail.c_str(), C_GRAY, &fonts::FreeSans9pt7b);
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3","pool.ntp.org","1.de.pool.ntp.org");
    delay(1000);
    ecdhInit();
    if(ankerLogin()){
      // Unverschluesselt – sonst 463 und die Anlagenauswahl bleibt leer
      String resp=httpsPost("power_service/v1/site/get_site_list",
                            "{\"page\":1,\"size\":10}",gAuthToken,gGtoken,false);
      DynamicJsonDocument doc(4096);
      if(resp.length()&&deserializeJson(doc,resp)==DeserializationError::Ok){
        auto sites=doc["data"]["site_list"];
        gSiteCount=0;
        for(auto s:sites.as<JsonArray>()){
          if(gSiteCount>=10) break;
          gSiteList[gSiteCount].id  =s["site_id"].as<String>();
          gSiteList[gSiteCount].name=s["site_name"].as<String>();
          gSiteCount++;
        }
        Serial.printf("[Save] %d Sites\n",gSiteCount);
        // Erste Anlage automatisch uebernehmen – keine Auswahlseite mehr.
        // Ueber /sites laesst sich das nachtraeglich aendern.
        // Automatisch uebernehmen nur bei genau EINER Anlage. Bei mehreren
        // muss der Mensch entscheiden: Jede Regel - "die erste", "die erste
        // mit erreichbarem Geraet" - ist beliebig, und schlimmer noch, ihr
        // Ergebnis kann sich spaeter von allein aendern, wenn ein Geraet in
        // einer anderen Anlage online geht. Genau das ist passiert.
        if(gSiteCount==1){
          cfg.siteId  =gSiteList[0].id;
          cfg.siteName=gSiteList[0].name;
          saveConfig();
          Serial.printf("[Save] Einzige Anlage: %s\n",cfg.siteName.c_str());
          lcd.fillScreen(C_BLACK);
          dispCenter( 80,"Anlage:",           C_GREEN, &fonts::FreeSansBold12pt7b);
          dispCenter(115,cfg.siteName.c_str(),C_YELLOW,&fonts::FreeSans9pt7b);
          dispCenter(160,"Neustart in 3s...", C_GRAY,  &fonts::FreeSans9pt7b);
          delay(3000);
          ESP.restart();
        } else if(gSiteCount>1){
          Serial.printf("[Save] %d Anlagen – Auswahl noetig\n",gSiteCount);
          lcd.fillScreen(C_BLACK);
          dispCenter( 55,"Anlage waehlen",     C_ORANGE,&fonts::FreeSansBold12pt7b);
          dispCenter( 90,"Im Browser oeffnen:",C_GRAY,  &fonts::FreeSans9pt7b);
          dispCenter(120,myIp.c_str(),         C_YELLOW,&fonts::FreeSansBold12pt7b);
          dispCenter(150,"/sites",             C_YELLOW,&fonts::FreeSans9pt7b);
          dispCenter(185,"(im Heimnetz)",      C_GRAY,  &fonts::FreeSans9pt7b);
        }
      }
    } else {
      lcd.fillScreen(C_BLACK);
      dispCenter( 55,"Anker-Login",           C_RED,   &fonts::FreeSansBold12pt7b);
      dispCenter( 82,"falsch!",               C_RED,   &fonts::FreeSansBold12pt7b);
      dispCenter(118,"Im Browser oeffnen:",   C_GRAY,  &fonts::FreeSans9pt7b);
      dispCenter(140,myIp.c_str(),            C_YELLOW,&fonts::FreeSansBold12pt7b);
      dispCenter(172,"und Daten neu eingeben",C_GRAY,  &fonts::FreeSans9pt7b);
    }
  } else { server.sendHeader("Location","/"); server.send(302); }
}
// Anmeldeseite fuer unbekannte Adressen.
// Ein blosses 302 ohne Rumpf werten manche Systeme nicht als Anmeldeseite,
// deshalb kommt hier eine echte Seite mit Code 200 zurueck. Die Umleitung
// uebernimmt das enthaltene <meta refresh> plus ein sichtbarer Link fuer
// den Fall, dass das Fenster die Weiterleitung unterdrueckt.
void handleNotFound(){
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html",
    F("<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='0; url=http://192.168.4.1/'>"
      "<title>Anker Display Setup</title></head>"
      "<body style='font-family:-apple-system,sans-serif;background:#0a0a0a;"
      "color:#eee;text-align:center;padding:60px 20px'>"
      "<h2 style='color:#f0a500'>Anker Display Setup</h2>"
      "<p>Weiterleitung zur Einrichtung&hellip;</p>"
      "<p style='margin-top:24px'><a style='color:#f0a500;font-size:1.2rem'"
      " href='http://192.168.4.1/'>Hier tippen, falls nichts passiert</a></p>"
      "</body></html>"));
}

// Adressen, mit denen Betriebssysteme pruefen, ob ein Netz ins Internet fuehrt.
// Antwortet der ESP32 darauf statt mit der erwarteten Erfolgsmeldung, oeffnet
// das Geraet die Anmeldeseite. Ohne diese Handler landen die Anfragen zwar
// ebenfalls bei handleNotFound, aber explizit ist es verlaesslicher.
void registerCaptiveProbes(){
  const char* probes[] = {
    "/hotspot-detect.html",           // iOS, macOS
    "/library/test/success.html",     // iOS, aeltere Fassungen
    "/generate_204",                  // Android
    "/gen_204",                       // Android
    "/connecttest.txt",               // Windows
    "/ncsi.txt",                      // Windows
    "/canonical.html",                // Firefox
    "/success.txt",                   // Firefox, Ubuntu
    "/chat",                          // einige Android-Fassungen
  };
  for(const char* p : probes) server.on(p, HTTP_GET, handleNotFound);
}

void startConfigPortal(){
  lcd.fillScreen(C_BLACK);
  dispCenter( 50,"Setup-Modus",    C_ORANGE,&fonts::FreeSansBold12pt7b);
  dispCenter( 80,"WLAN verbinden:",C_GRAY,  &fonts::FreeSans9pt7b);
  dispCenter(100,AP_SSID,          C_WHITE, &fonts::FreeSans9pt7b);
  dispCenter(125,"Dann Browser:",  C_GRAY,  &fonts::FreeSans9pt7b);
  dispCenter(145,AP_IP,            C_YELLOW,&fonts::FreeSans9pt7b);
  // Reihenfolge ist entscheidend: erst konfigurieren, dann starten.
  // Umgekehrt laeuft der DHCP-Server kurz mit Standardwerten und verteilt
  // moeglicherweise nicht 192.168.4.1 als DNS – dann wird der Platzhalter-DNS
  // unten nie gefragt und die Anmeldeseite oeffnet sich nicht von selbst.
  IPAddress ip(192,168,4,1),gw(192,168,4,1),sn(255,255,255,0);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ip,gw,sn);
  WiFi.softAP(AP_SSID);
  delay(200);                     // AP kurz hochkommen lassen
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53,"*",ip);
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/save",      HTTP_POST, handleSave);
  server.on("/sites",     HTTP_GET,  handleSites);
  server.on("/selectsite",HTTP_GET,  handleSelectSite);
  registerCaptiveProbes();
  server.onNotFound(handleNotFound); server.begin();
  while(true){dns.processNextRequest();server.handleClient();delay(5);}
}

void startFixPortal(){
  gFixMode=true;
  String myIp=WiFi.localIP().toString();
  lcd.fillScreen(C_BLACK);
  dispCenter( 55,"Anker-Login",           C_RED,   &fonts::FreeSansBold12pt7b);
  dispCenter( 82,"falsch!",               C_RED,   &fonts::FreeSansBold12pt7b);
  dispCenter(118,"Im Browser oeffnen:",   C_GRAY,  &fonts::FreeSans9pt7b);
  dispCenter(140,myIp.c_str(),            C_YELLOW,&fonts::FreeSansBold12pt7b);
  dispCenter(172,"und Daten neu eingeben",C_GRAY,  &fonts::FreeSans9pt7b);
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/save",      HTTP_POST, handleSave);
  server.on("/sites",     HTTP_GET,  handleSites);
  server.on("/selectsite",HTTP_GET,  handleSelectSite);
  server.onNotFound([](){server.sendHeader("Location","/");server.send(302);});
  server.begin();
  while(true){server.handleClient();delay(5);}
}

// ─────────────────────────────────────────────────────────────────────────────
// ANKER HTTP
// encrypt=true: Body AES-verschluesselt als JSON-String, Antwort entschluesselt
// ─────────────────────────────────────────────────────────────────────────────
String httpsPost(const String& path, const String& body,
                 const String& token, const String& gtoken,
                 bool encrypt) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, String(ANKER_HOST)+"/"+path);
  http.addHeader("content-type",      "application/json");
  http.addHeader("Accept",            "application/json");
  http.addHeader("app-name",          "anker_power");
  http.addHeader("Os-type",           "iOS");
  http.addHeader("os_type",           "ios");
  http.addHeader("country",           "DE");
  http.addHeader("User-Agent",        "ktor-client");
  http.addHeader("Cache-Control",     "no-cache");
  http.addHeader("App-version",       "3.21.1");
  http.addHeader("app_version",       "3.21.1");
  http.addHeader("model-type",        "PHONE");
  http.addHeader("model_type",        "PHONE");
  http.addHeader("language",          "de");
  http.addHeader("Accept-Charset",    "UTF-8");
  http.addHeader("Accept-Language",   "de-DE,de;q=0.9");
  http.addHeader("X-Client-Credential","");
  http.addHeader("Client-id",         "");
  char tsStr[12]; snprintf(tsStr,sizeof(tsStr),"%lu",(unsigned long)time(nullptr));
  http.addHeader("X-Request-Ts", tsStr);
  // X-Request-Once bei JEDEM Request – der Key-Exchange lehnt sonst mit
  // 'field "X-Request-Once" is not set' ab. Muss eindeutig sein: zwei
  // identische Nonces hintereinander quittiert der Server mit 462 (Replay).
  uint8_t once[16]; esp_fill_random(once,16);
  String nonceHex = bytesToHex(once,16);
  http.addHeader("X-Request-Once", nonceHex);
  // X-Key-Ident = MD5(timestamp + auth_token)  [Formel aus anker-solix-api]
  String keyIdent;
  if(gEcdhReady){
    keyIdent = md5Hex(String(tsStr) + gAuthToken);
    http.addHeader("X-Key-Ident", keyIdent);
  }
  if(token.length()){
    http.addHeader("X-Auth-Token", token);
    http.addHeader("gtoken", gtoken.length()?gtoken:token);
    if(gUserId.length()) http.addHeader("uid", gUserId);
  }
  http.setTimeout(15000);

  String sendBody;
  if(encrypt && gEcdhReady) {
    http.addHeader("X-Encryption-Info",    "algo_ecdh");
    http.addHeader("ENCRYPT_APP_PUBLICKEY", "");
    http.addHeader("X-Replay-Info", "replay");
    Serial.printf("[ENC] ts=%s keyIdent=%s once=%s\n",
                  tsStr, keyIdent.c_str(), nonceHex.c_str());
    // Body: rohes Base64(IV[16]+Cipher) – kein JSON-Wrapper
    sendBody = aesEncryptBody(body);
    // Signatur: SHA256(ts + once + keyIdent + body) – Variante aus anker-solix-api
    uint8_t sigBuf[32];
    String sigInput = String(tsStr) + nonceHex + keyIdent + sendBody;
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)sigInput.c_str(), sigInput.length(), sigBuf);
    http.addHeader("X-Signature", bytesToHex(sigBuf, 32));
  } else {
    sendBody = body;
  }

  int code=http.POST(sendBody);
  String resp=http.getString();
  Serial.printf("[API] %d /%s (enc=%d)\n",code,path.c_str(),(int)encrypt);
  // Immer mindestens 300 Zeichen der Antwort ausgeben (auch bei Fehlern)
  if(code!=200){
    Serial.printf("[API-ERR] %.300s\n",resp.c_str());
    http.end(); return "";
  }
  http.end();

  if(encrypt && gEcdhReady){
    String encData=extractDataField(resp);
    if(encData.isEmpty()){
      Serial.println("[API] Kein data-Feld – Fallback Klartext");
      return resp;
    }
    return aesDecrypt(encData);
  }
  return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// LOGIN
// ─────────────────────────────────────────────────────────────────────────────
bool ankerLogin(){
  Serial.println("[Auth] Login...");
  if(!gEcdhReady){Serial.println("[Auth] ECDH not ready");return false;}
  String encPw=aesEncrypt(cfg.ankerPass);
  if(encPw.isEmpty()){Serial.println("[Auth] Encrypt failed");return false;}
  char tsMs[21];
  snprintf(tsMs,sizeof(tsMs),"%llu",(unsigned long long)time(nullptr)*1000ULL);
  time_t now=time(nullptr); struct tm tiL,tiU;
  localtime_r(&now,&tiL); gmtime_r(&now,&tiU);
  long tzMs=(long)(difftime(mktime(&tiL),mktime(&tiU))*1000.0);
  DynamicJsonDocument doc(512);
  doc["ab"]="DE"; doc["enc"]=0;
  doc["email"]=cfg.ankerEmail; doc["password"]=encPw;
  doc["time_zone"]=tzMs; doc["transaction"]=String(tsMs);
  doc.createNestedObject("client_secret_info")["public_key"]=bytesToHex(gClientPubKey,65);
  String b; serializeJson(doc,b);
  String resp=httpsPost("passport/login",b);
  if(resp.isEmpty()){Serial.println("[Auth] No response");return false;}
  DynamicJsonDocument rd(4096);
  if(deserializeJson(rd,resp)!=DeserializationError::Ok){Serial.println("[Auth] JSON error");return false;}
  int code=rd["code"]|-1;
  if(code!=0){Serial.printf("[Auth] Error %d: %s\n",code,rd["msg"].as<const char*>());return false;}
  gAuthToken=rd["data"]["auth_token"].as<String>();
  gUserId   =rd["data"]["user_id"].as<String>();
  gGtoken   =md5Hex(gUserId);
  gGeoKey   =rd["data"]["geo_key"].as<String>();
  gTokenExpiry=millis()+23UL*3600*1000;
  Serial.printf("[Auth] geo_key=%s\n",gGeoKey.c_str());
  // Server-Session-Key fuer API-Verschluesselung aus Login-Response lesen
  String srvPub=rd["data"]["server_secret_info"]["public_key"].as<String>();
  if(srvPub.length()==130){
    ecdhUpdateShared(srvPub);
  } else {
    Serial.printf("[ECDH] Kein server_secret_info (len=%u) – Fallback auf Passwort-Key\n",(unsigned)srvPub.length());
  }
  Serial.println("[Auth] OK");
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT-INFO-TEST
// get_user_mqtt_info ist ein normaler Endpunkt ohne algo_ecdh. Liefert er
// Zertifikate, brauchen wir die Body-Verschluesselung ueberhaupt nicht:
// ueber MQTT pusht das Geraet Echtzeitdaten im 3-Sekunden-Takt.
// ─────────────────────────────────────────────────────────────────────────────
static void printLong(const char* tag, const String& s) {
  const size_t CHUNK=180;
  for(size_t i=0;i<s.length();i+=CHUNK){
    Serial.printf("[%s] %s\n",tag,s.substring(i,i+CHUNK).c_str());
    delay(5);   // Serial-Puffer nicht ueberrennen
  }
}

// Roher POST: liefert HTTP-Code, schreibt volle Antwort nach outResp
static int rawPost(const String& path, const String& body, String& outResp) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, String(ANKER_HOST)+"/"+path);
  http.addHeader("content-type",  "application/json");
  http.addHeader("Accept",        "application/json");
  http.addHeader("app-name",      "anker_power");
  http.addHeader("Os-type",       "iOS");
  http.addHeader("os_type",       "ios");
  http.addHeader("country",       "DE");
  http.addHeader("User-Agent",    "ktor-client");
  http.addHeader("App-version",   "3.21.1");
  http.addHeader("app_version",   "3.21.1");
  http.addHeader("model-type",    "PHONE");
  http.addHeader("model_type",    "PHONE");
  http.addHeader("language",      "de");
  http.addHeader("X-Auth-Token",  gAuthToken);
  http.addHeader("gtoken",        gGtoken);
  if(gUserId.length()) http.addHeader("uid", gUserId);
  // Dieselben Pflicht-Header wie httpsPost – ohne X-Request-Once
  // antwortet der Key-Exchange mit 400.
  char tsStr[12]; snprintf(tsStr,sizeof(tsStr),"%lu",(unsigned long)time(nullptr));
  http.addHeader("X-Request-Ts", tsStr);
  uint8_t once[16]; esp_fill_random(once,16);
  http.addHeader("X-Request-Once", bytesToHex(once,16));
  if(gEcdhReady) http.addHeader("X-Key-Ident", md5Hex(String(tsStr)+gAuthToken));
  http.setTimeout(15000);
  int code=http.POST(body);
  outResp=http.getString();
  http.end();
  return code;
}

// Holt einen JSON-Stringwert per Textsuche – spart den Speicher, den ein
// DynamicJsonDocument fuer die 8 KB grosse MQTT-Antwort braeuchte.
static String jsonStr(const String& json, const char* key) {
  String pat=String("\"")+key+"\":\"";
  int i=json.indexOf(pat);
  if(i<0) return "";
  i+=pat.length();
  int e=i;
  while(e<(int)json.length()){
    if(json[e]=='"'&&json[e-1]!='\\') break;
    e++;
  }
  return json.substring(i,e);
}

// JSON transportiert "\n" als zwei Zeichen – mbedtls braucht echte Umbrueche
static String unescapePem(const String& s) {
  String r; r.reserve(s.length());
  for(size_t i=0;i<s.length();i++){
    if(s[i]=='\\'&&i+1<s.length()&&s[i+1]=='n'){ r+='\n'; i++; }
    else r+=s[i];
  }
  return r;
}

bool fetchMqttCreds(){
  Serial.println();
  Serial.println("=== MQTT-ZUGANGSDATEN ===");
  String resp;
  int code=rawPost("app/devicemanage/get_user_mqtt_info","{}",resp);
  Serial.printf("[MQTT] HTTP %d, %u Bytes\n",code,(unsigned)resp.length());
  if(code!=200){
    Serial.println("[MQTT] Fehlgeschlagen");
    if(resp.length()) printLong("MQTT",resp);
    return false;
  }
  gMqttHost = jsonStr(resp,"endpoint_addr");
  gMqttThing= jsonStr(resp,"thing_name");
  gMqttCertId=jsonStr(resp,"certificate_id");
  gMqttUserId=jsonStr(resp,"user_id");
  gMqttCert = unescapePem(jsonStr(resp,"certificate_pem"));
  gMqttKey  = unescapePem(jsonStr(resp,"private_key"));
  gMqttCa   = unescapePem(jsonStr(resp,"aws_root_ca1_pem"));
  // Zertifikatsinhalte bewusst NICHT ins Log – enthaelt den privaten Schluessel
  Serial.printf("[MQTT] endpoint = %s\n",gMqttHost.c_str());
  Serial.printf("[MQTT] thing    = %s\n",gMqttThing.c_str());
  Serial.printf("[MQTT] cert=%uB key=%uB ca=%uB\n",
    (unsigned)gMqttCert.length(),(unsigned)gMqttKey.length(),(unsigned)gMqttCa.length());
  bool ok = gMqttHost.length()&&gMqttCert.length()&&gMqttKey.length()&&gMqttCa.length();
  Serial.println(ok?"=== MQTT-ZUGANGSDATEN OK ===":"=== MQTT-ZUGANGSDATEN UNVOLLSTAENDIG ===");
  return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// GERAETESUCHE – device_sn + product_code fuers MQTT-Topic:
//   dt/{app_name}/{product_code}/{device_sn}/
// ─────────────────────────────────────────────────────────────────────────────
// Solarbank der gewaehlten Anlage bestimmen. get_site_detail liefert genau
// die Geraete dieser site_id – solarbank_list[0] ist die Solarbank.
bool fetchDeviceInfo(){
  Serial.println("=== GERAET ===");
  String resp;
  int code=rawPost("power_service/v1/site/get_site_detail",
                   String("{\"site_id\":\"")+gSiteId+"\"}",resp);
  if(code!=200){Serial.printf("[DEV] HTTP %d\n",code);return false;}
  // Innerhalb solarbank_list suchen, damit nicht der Shelly erwischt wird
  int sb=resp.indexOf("\"solarbank_list\":");
  if(sb<0){Serial.println("[DEV] keine solarbank_list");return false;}
  String tail=resp.substring(sb);
  gDevPn=jsonStr(tail,"device_pn");
  gDevSn=jsonStr(tail,"device_sn");
  Serial.printf("[DEV] %s  %s  (%s)\n",
                gDevPn.c_str(),gDevSn.c_str(),
                jsonStr(tail,"device_name").c_str());
  // Netzzaehler aus grid_list – der misst den Netzbezug, nicht die Solarbank
  int gl=resp.indexOf("\"grid_list\":");
  if(gl>=0){
    String g=resp.substring(gl);
    gGridPn=jsonStr(g,"device_pn");
    gGridSn=jsonStr(g,"device_sn");
    if(gGridSn.length())
      Serial.printf("[DEV] %s  %s  (%s)\n",
                    gGridPn.c_str(),gGridSn.c_str(),
                    jsonStr(g,"device_name").c_str());
  }
  applyGridScale();   // Teiler haengt vom erkannten Zaehlertyp ab
  return gDevSn.length()&&gDevPn.length();
}

// ─────────────────────────────────────────────────────────────────────────────
// MQTT – Broker aiot-mqtt-eu.anker.com:8883, gegenseitige TLS-Authentifizierung
// ─────────────────────────────────────────────────────────────────────────────
// Entschaerft \" und \\ im als String eingebetteten payload-JSON
static String unescapeJson(const String& s){
  String r; r.reserve(s.length());
  for(size_t i=0;i<s.length();i++){
    if(s[i]=='\\'&&i+1<s.length()){ r+=s[i+1]; i++; }
    else r+=s[i];
  }
  return r;
}

#if VERBOSE
static void hexDump(const uint8_t* d, unsigned len){
  const unsigned MAXDUMP=512;
  unsigned n=len<MAXDUMP?len:MAXDUMP;
  for(unsigned i=0;i<n;i+=16){
    char line[80]; int p=0;
    p+=snprintf(line+p,sizeof(line)-p,"%04u  ",i);
    for(unsigned j=0;j<16;j++){
      if(i+j<n) p+=snprintf(line+p,sizeof(line)-p,"%02x ",d[i+j]);
      else      p+=snprintf(line+p,sizeof(line)-p,"   ");
    }
    p+=snprintf(line+p,sizeof(line)-p," ");
    for(unsigned j=0;j<16&&i+j<n;j++){
      uint8_t c=d[i+j];
      p+=snprintf(line+p,sizeof(line)-p,"%c",(c>=32&&c<127)?c:'.');
    }
    Serial.println(line); delay(2);
  }
  if(len>MAXDUMP) Serial.printf("... (%u Bytes gekuerzt)\n",len-MAXDUMP);
}
#endif

// Ein Akkublock aus der Nachricht vom Typ 0500.
// Feste Offsets, an drei Packs abgeglichen:
//   0      laufender Index (1, 2, 3 …)
//   12..13 je Pack konstant, Bedeutung offen (89 / 70 / 60 im Testgeraet)
//   14..23 fuenf Zellspannungen, je u16 in Millivolt
//   24..31 vier Temperaturen, je i16 in Zehntelgrad
//   36..37 Ladestand in Zehntelprozent
// Der Ladestand ist gegen die App geprueft: 252/260/258 entsprachen dort
// 25/26/26 %. Offset 34 liegt nur wenige Zehntel daneben und haette bei zwei
// von drei Packs falsch gerundet - deshalb ausdruecklich 36.
// Die Seriennummer wird gesucht statt fest adressiert: sie steht am Ende,
// aber die Blocklaenge unterscheidet sich je Pack.
static void parsePackBlock(const uint8_t* d, uint8_t len){
  if(len<32) return;
  // Nach Index einsortieren statt anzuhaengen: die 0500-Nachrichten kommen in
  // verschiedenen Groessen und tragen offenbar nicht immer alle Packs. Wuerde
  // die Liste je Nachricht neu gefuellt, loescht eine kleine Nachricht das,
  // was eine grosse zuvor geliefert hat.
  uint8_t idx = d[0];
  if(idx<1 || idx>MAX_PACKS) return;
  PackInfo& p = gPacks[idx-1];
  p.idx = idx;
  memcpy(&p.unknown12, d+12, 2);
  if(len>=38) memcpy(&p.soc10, d+36, 2);
  for(int i=0;i<5;i++) memcpy(&p.cell[i], d+14+i*2, 2);
  for(int i=0;i<4;i++) memcpy(&p.temp[i], d+24+i*2, 2);
  // Laengste zusammenhaengende Folge druckbarer Zeichen = Seriennummer
  p.sn=""; String cur;
  for(uint8_t i=0;i<len;i++){
    char c=(char)d[i];
    if((c>='0'&&c<='9')||(c>='A'&&c<='Z')){ cur+=c; }
    else { if(cur.length()>p.sn.length()) p.sn=cur; cur=""; }
  }
  if(cur.length()>p.sn.length()) p.sn=cur;
  if(p.sn.length()<8) p.sn="";        // zu kurz, um eine Seriennummer zu sein
  p.raw = bytesToHex(d,len);
  p.valid = true;
  gPackCount=0;
  for(int i=0;i<MAX_PACKS;i++) if(gPacks[i].valid) gPackCount++;
  LOGF("[PACK] %u  %.1f%%  Zellen %u/%u/%u/%u/%u mV  Temp %.1f/%.1f/%.1f/%.1f C  %s\n",
       p.idx, p.soc10/10.0, p.cell[0],p.cell[1],p.cell[2],p.cell[3],p.cell[4],
       p.temp[0]/10.0, p.temp[1]/10.0, p.temp[2]/10.0, p.temp[3]/10.0,
       p.sn.c_str());
}

// Dekodiert die param_info-Nutzlast und fuellt gData.
// Rahmen und Feldkodierung siehe Kopfkommentar.
static bool parseParamInfo(const String& b64){
  size_t need=0;
  mbedtls_base64_decode(nullptr,0,&need,(const uint8_t*)b64.c_str(),b64.length());
  if(need<16) return false;
  uint8_t* b=(uint8_t*)malloc(need);
  if(!b) return false;
  size_t got=0;
  mbedtls_base64_decode(b,need,&got,(const uint8_t*)b64.c_str(),b64.length());
  if(got<16||b[0]!=0xff||b[1]!=0x09){ free(b); return false; }

  // Nachrichtentyp steht in Byte 7/8. Die Feldnummern bedeuten je Typ etwas
  // anderes, deshalb muss hier getrennt werden: 0405 traegt die Leistungen,
  // 0500 die Daten der einzelnen Akkupacks.
  if(b[7]==0x05 && b[8]==0x00){
    // Aufbau der Nachricht mitschreiben. Nur so laesst sich unterscheiden, ob
    // weitere Packbloecke gar nicht ankommen oder ob der Filter sie uebergeht.
    String map = "len=" + String((unsigned)got) + "  ";
    size_t i=9;
    bool desync=false;
    while(i+1<got){
      uint8_t tag=b[i], ln=b[i+1];
      if(i+2+(size_t)ln>got){ desync=true; break; }
      uint8_t ty = ln ? b[i+2] : 0;
      char t[24]; snprintf(t,sizeof(t),"%02x:%u/t%02x ",tag,ln,ty);
      map += t;
      // a4 aufwaerts sind die Packbloecke, erkennbar am Typ 04 und der Laenge
      if(tag>=0xa4 && ln>32 && ty==0x04)
        parsePackBlock(b+i+3, ln-1);
      i+=2+ln;
    }
    if(desync) map += "<< ABBRUCH: Laenge passt nicht";
    else if(i<got) map += "<< Rest " + String((unsigned)(got-i)) + " B";
    gLastStateInfo = map;
    LOGF("[0500] %s\n", map.c_str());
    free(b);
    return false;   // keine Leistungswerte in dieser Nachricht
  }

  bool  haveSolar=false;
  float solar=0, battW=0, outW=0, str[4]={0,0,0,0};
  int   soc=-1;
  String ints;                      // Kandidatenliste fuer den Ladestand
  size_t i=9;                       // 9 Byte Rahmen, dann Felder
  while(i+1<got){
    uint8_t tag=b[i], ln=b[i+1];
    if(i+2+(size_t)ln>got) break;
    const uint8_t* d=b+i+2;
    if(ln==5 && d[0]==0x05){        // float32, little endian
      float v; memcpy(&v,d+1,4);
      switch(tag){
        case 0xab: solar=v; haveSolar=true; break;
        case 0xac: battW=v; break;  // negativ = Entladen (vom Nutzer bestaetigt)
        case 0xad: outW =v; break;  // Ausgangsleistung: ab + |ac| ergibt genau ad
        case 0xc6: str[0]=v; break;
        case 0xc7: str[1]=v; break;
        case 0xc8: str[2]=v; break;
        case 0xc9: str[3]=v; break;
      }
    }
    // Alle 1-Byte-Werte 0..100 sammeln – einer davon ist der Ladestand
    if(ln==2 && d[0]==0x01 && d[1]<=100){
      char t[16]; snprintf(t,sizeof(t),"%02x=%u ",tag,d[1]);
      ints+=t;
      // 0xa3 ist der Ladestand der KOPFSTATION, nicht des Gesamtsystems.
      // Bei mehreren Speichern weicht er von dem ab, was die App zeigt.
      // Liegen Packdaten vor, wird er weiter unten ueberschrieben; bis dahin
      // ist er die beste verfuegbare Angabe.
      if(tag==0xa3) soc=d[1];
    }
    i+=2+ln;
  }
  free(b);
  if(!haveSolar){
    // 425-Byte-Variante ohne 0xab – nur einmal melden, nicht bei jeder Nachricht
    static bool once=false;
    if(!once){ once=true; Serial.println("[BANK] Nebennachricht ohne 0xab – ignoriert"); }
    return false;
  }

  // Ladestand des Gesamtsystems statt nur der Kopfstation. 0xa3 meldet nur
  // die Haupteinheit; die App zeigt den Systemwert. Solange die Packdaten
  // noch nicht eingetroffen sind, bleibt 0xa3 die beste verfuegbare Angabe.
  //
  // Gemittelt wird ungewichtet. Die Kapazitaeten der einzelnen Packs stehen
  // nicht im Datenstrom, und solange sie aehnlich weit geladen sind - der
  // Normalfall bei einem ausbalancierten System - liegt der Unterschied zur
  // kapazitaetsgewichteten Rechnung unter einem Prozentpunkt.
  {
    int n=0; float sum=0;
    for(int k=0;k<MAX_PACKS;k++)
      if(gPacks[k].valid && gPacks[k].soc10){ sum+=gPacks[k].soc10/10.0f; n++; }
    if(n) soc=(int)(sum/n+0.5f);
  }
  gData.solar_w    = solar;
  gData.batt_in_w  = battW>0? battW : 0;
  gData.batt_out_w = battW<0? -battW: 0;
  gOutW            = outW;          // Netzteil rechnet den Hausverbrauch daraus
  if(soc>=0){
    gData.battery_pct= soc;
    gData.battery_wh = soc/100.0f*cfg.battWh;
  }
  gData.valid=true;
  for(int k=0;k<4;k++) gPvStr[k]=str[k];
  integrateEnergy();
  LOGF("[PV] %.0f W = %.0f+%.0f+%.0f+%.0f | Akku %.0f W | Aus %.0f W\n",
                solar,str[0],str[1],str[2],str[3],battW,outW);
  LOGF("[INT] %s\n",ints.c_str());
  return true;
}

// Nachricht des Netzzaehlers: alle float-Felder ungefiltert ausgeben.
// Welches davon der Netzbezug ist, zeigt der Abgleich mit der App.
// Teiler fuer die Rohwerte a8/a9 des Netzzaehlers. Die Einheit haengt vom
// Geraet ab: ein Shelly EM3 meldet Hundertstel-Watt (90925 = 909 W), der
// Anker-Smartmeter dagegen ganze Watt (250 = 250 W). Eine feste Konstante
// liefert deshalb bei einem Teil der Nutzer Werte um Faktor 100 daneben.
// gGridScale wird beim Erkennen des Zaehlers gesetzt; cfg.gridScale > 0
// ueberschreibt das dauerhaft und ist ueber die Weboberflaeche einstellbar.
static void applyGridScale(){
  if(cfg.gridScale>0){
    gGridScale=cfg.gridScale;
    Serial.printf("[NETZ] Teiler %.0f (manuell)\n",gGridScale);
    return;
  }
  gGridScale = gGridPn.startsWith("SHEM") ? 100.0f : 1.0f;
  Serial.printf("[NETZ] Teiler %.0f (automatisch fuer %s)\n",
                gGridScale, gGridPn.length()?gGridPn.c_str():"unbekannt");
}

static bool parseGridInfo(const String& b64){
  size_t need=0;
  mbedtls_base64_decode(nullptr,0,&need,(const uint8_t*)b64.c_str(),b64.length());
  if(need<16) return false;
  uint8_t* b=(uint8_t*)malloc(need);
  if(!b) return false;
  size_t got=0;
  mbedtls_base64_decode(b,need,&got,(const uint8_t*)b64.c_str(),b64.length());
  if(got<16||b[0]!=0xff||b[1]!=0x09){ free(b); return false; }
  // a8 = Bezug, a9 = Einspeisung. Zwei Felder, weil u32 kein Vorzeichen hat.
  uint32_t imp=0, exp_=0; bool have=false;
  size_t i=9;
  while(i+1<got){
    uint8_t tag=b[i], ln=b[i+1];
    if(i+2+(size_t)ln>got) break;
    const uint8_t* d=b+i+2;
    if(ln==5 && d[0]==0x03){
      uint32_t v; memcpy(&v,d+1,4);
      if(tag==0xa8){ imp=v; have=true; }
      if(tag==0xa9){ exp_=v; }
    }
    i+=2+ln;
  }
  free(b);
  if(!have) return false;

  float w=((float)imp-(float)exp_)/gGridScale;
  gData.grid_w=w;
  // Hausverbrauch = Anlagenausgang (Solar + Akku) + Netz.
  // gOutW kommt aus 0xad der Solarbank, nicht aus der reinen Solarleistung.
  gData.home_w=gOutW+w;
  static uint32_t skip=0;
  if((skip++ % 4)==0)               // nicht bei jeder Nachricht loggen
    LOGF("[NETZ] roh a8=%lu a9=%lu -> %.0f W %s | Haus %.0f W\n",
                  (unsigned long)imp,(unsigned long)exp_,fabsf(w),
                  w>=0?"Bezug":"Einspeisung",gData.home_w);
  return true;
}

static void mqttCallback(char* topic, uint8_t* payload, unsigned int len){
  gMqttRxCount++;
  // Nur der letzte Topic-Abschnitt interessiert (state_info, param_info, ...)
  const char* shortTopic=strrchr(topic,'/');
  shortTopic = shortTopic?shortTopic+1:topic;
  // Absender anhand der Seriennummer im Topic bestimmen
  bool fromGrid = gGridSn.length() && strstr(topic,gGridSn.c_str())!=nullptr;
  LOGF("\n[RX] #%lu %s %s (%u B)\n",
       (unsigned long)gMqttRxCount, fromGrid?"ZAEHLER":"BANK",
       shortTopic, len);

  String raw; raw.reserve(len+1);
  for(unsigned i=0;i<len;i++) raw+=(char)payload[i];

  // Aeussere Huelle ist JSON -> inneren payload-String lesbar ausgeben
  int pi=raw.indexOf("\"payload\":\"");
  if(raw.startsWith("{")&&pi>=0){
    // Die frueher hier stehende Zeile mit cmd/seq/ts ist entfallen: jsonStr
    // liest nur Werte in Anfuehrungszeichen, diese drei sind aber Zahlen –
    // sie hat also seit jeher nur Fragezeichen ausgegeben.
    int s=pi+11, e=raw.lastIndexOf("\"}");
    if(e>s){
      String inner=unescapeJson(raw.substring(s,e));
      // Leistungswerte stecken base64-kodiert im data-Feld
      String d=jsonStr(inner,"data");
      if(d.length()) { if(fromGrid) parseGridInfo(d); else parseParamInfo(d); }
#if VERBOSE
      // Klartext-Nachrichten ausgeben. Achtung: das dort enthaltene Feld
      // "battery" ist NICHT der Ladestand – es stand konstant auf 100,
      // waehrend der Akku real bei 9 % lag. Der Ladestand ist 0xa3.
      else if(inner.indexOf("\"battery\":")<0) printLong("DATA",inner);
#endif
    }
  }
#if VERBOSE
  else hexDump(payload,len);   // unbekanntes Format – nur beim Mitlesen noetig
#endif
}

// ── Echtzeit-Trigger ────────────────────────────────────────────────────────
// Binaerrahmen laut Discussion #222: ff 09 | len(LE) | 03 00 0f | typ | Felder
// Nachrichtentyp 0057 = CMD_REALTIME_TRIGGER, Felder a1/a2/a3 + fe(Zeitstempel).
// Das genaue Layout ist NICHT oeffentlich dokumentiert – bewusst geraten.
static String buildTriggerB64(uint16_t timeoutSec){
  uint8_t b[32]; int n=0;
  b[n++]=0xff; b[n++]=0x09;
  int lenPos=n; b[n++]=0x00; b[n++]=0x00;      // Laenge, unten nachgetragen
  b[n++]=0x03; b[n++]=0x00; b[n++]=0x0f;
  b[n++]=0x00; b[n++]=0x57;                    // Typ 0057
  b[n++]=0xa1; b[n++]=0x01; b[n++]=0x22;       // a1: fester Wert
  b[n++]=0xa2; b[n++]=0x01; b[n++]=0x01;       // a2: Trigger ein
  b[n++]=0xa3; b[n++]=0x02;                    // a3: Timeout, 2 Byte LE
  b[n++]=(uint8_t)(timeoutSec&0xff);
  b[n++]=(uint8_t)(timeoutSec>>8);
  uint32_t ts=(uint32_t)time(nullptr);
  b[n++]=0xfe; b[n++]=0x04;                    // fe: Zeitstempel, 4 Byte LE
  b[n++]=(uint8_t)(ts);       b[n++]=(uint8_t)(ts>>8);
  b[n++]=(uint8_t)(ts>>16);   b[n++]=(uint8_t)(ts>>24);
  b[lenPos]  =(uint8_t)(n&0xff);
  b[lenPos+1]=(uint8_t)(n>>8);
  Serial.print("[TRG] Bytes: "); Serial.println(bytesToHex(b,n));
  return b64Encode(b,n);
}

void sendRealtimeTrigger(uint16_t timeoutSec){
  if(!gMqtt.connected()) return;
  String data=buildTriggerB64(timeoutSec);
  String inner=String("{\"device_sn\":\"")+gDevSn+
               "\",\"account_id\":\""+gMqttUserId+
               "\",\"data\":\""+data+"\"}";
  // Anfuehrungszeichen des inneren JSON escapen – es reist als String mit
  String innerEsc; innerEsc.reserve(inner.length()*2);
  for(size_t i=0;i<inner.length();i++){
    if(inner[i]=='"') innerEsc+="\\\"";
    else              innerEsc+=inner[i];
  }
  String pubClientId="android-anker_power-"+gMqttUserId+"-"+gMqttCertId;
  String msg=String("{\"head\":{\"version\":\"1.0.0.1\",\"client_id\":\"")+pubClientId+
    "\",\"sess_id\":\""+String(random(1000,9999))+"-"+String(random(1000,9999))+
    "\",\"msg_seq\":1,\"seed\":1,\"timestamp\":"+String((uint32_t)time(nullptr))+
    ",\"cmd_status\":2,\"cmd\":17,\"sign_code\":1,\"device_pn\":\""+gDevPn+
    "\",\"device_sn\":\""+gDevSn+"\"},\"payload\":\""+innerEsc+"\"}";
  String topic="cmd/anker_power/"+gDevPn+"/"+gDevSn+"/req";
  Serial.printf("\n[TRG] -> %s (%u B)\n",topic.c_str(),(unsigned)msg.length());
#if VERBOSE
  printLong("TRG",msg);
#endif
  bool ok=gMqtt.publish(topic.c_str(),msg.c_str());
  Serial.printf("[TRG] publish %s\n",ok?"OK":"FEHLGESCHLAGEN");
}

bool mqttConnect(){
  if(gMqttHost.isEmpty()||gDevSn.isEmpty()){
    Serial.println("[MQTT] Zugangsdaten/Geraet fehlen");
    return false;
  }
  Serial.printf("\n[MQTT] Heap vor TLS: %u\n",(unsigned)ESP.getFreeHeap());
  gMqttNet.setCACert(gMqttCa.c_str());
  gMqttNet.setCertificate(gMqttCert.c_str());
  gMqttNet.setPrivateKey(gMqttKey.c_str());
  gMqtt.setServer(gMqttHost.c_str(),8883);
  gMqtt.setBufferSize(4096);   // Standard sind 256 Bytes – viel zu wenig
  gMqtt.setKeepAlive(60);
  gMqtt.setCallback(mqttCallback);

  String clientId=gMqttThing+"_"+String(random(10000,99999));
  Serial.printf("[MQTT] Verbinde als %s\n",clientId.c_str());
  if(!gMqtt.connect(clientId.c_str())){
    Serial.printf("[MQTT] Verbindung fehlgeschlagen, state=%d\n",gMqtt.state());
    Serial.println("[MQTT] state -2=TLS/Netzwerk  -4=Timeout  5=nicht autorisiert");
    Serial.printf("[MQTT] Heap nach Fehler: %u\n",(unsigned)ESP.getFreeHeap());
    return false;
  }
  Serial.printf("[MQTT] VERBUNDEN. Heap: %u\n",(unsigned)ESP.getFreeHeap());
  String topic="dt/anker_power/"+gDevPn+"/"+gDevSn+"/#";
  if(gMqtt.subscribe(topic.c_str())) Serial.printf("[MQTT] Abo: %s\n",topic.c_str());
  else                               Serial.printf("[MQTT] Abo FEHLGESCHLAGEN: %s\n",topic.c_str());
  // Netzzaehler separat abonnieren – die Solarbank kennt den Netzbezug nicht
  if(gGridSn.length()){
    String gt="dt/anker_power/"+gGridPn+"/"+gGridSn+"/#";
    if(gMqtt.subscribe(gt.c_str())) Serial.printf("[MQTT] Abo: %s\n",gt.c_str());
    else                            Serial.printf("[MQTT] Abo FEHLGESCHLAGEN: %s\n",gt.c_str());
  }
  gMqttConnectedAt=millis();
  gTriggerArmed=false;
  Serial.println("[MQTT] 25 s nur lauschen – was kommt von allein?");
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SITE + DATEN
// ─────────────────────────────────────────────────────────────────────────────
bool fetchSiteId(){
  // Unverschluesselt! Mit encrypt=true antwortet der Server 463 und die
  // Anlagenliste bliebe leer – die Auswahl im Einrichtungsdialog waere tot.
  String resp=httpsPost("power_service/v1/site/get_site_list",
                        "{\"page\":1,\"size\":10}",gAuthToken,gGtoken,false);
  if(resp.isEmpty()) return false;
  DynamicJsonDocument doc(4096);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok) return false;
  auto sites=doc["data"]["site_list"];
  if(!sites||sites.size()==0){Serial.println("[API] No sites");return false;}
  for(auto s:sites.as<JsonArray>())
    Serial.printf("[API] Site: %s  %s\n",s["site_id"].as<const char*>(),s["site_name"].as<const char*>());
  gSiteId=sites[0]["site_id"].as<String>();
  return true;
}

bool fetchData(){
  if(gAuthToken.isEmpty()) return false;
  if(millis()>gTokenExpiry){Serial.println("[Auth] Re-Login...");if(!ankerLogin())return false;}
  if(gSiteId.isEmpty()){
    if(cfg.siteId.length()>0) gSiteId=cfg.siteId;
    else if(!fetchSiteId()) return false;
  }
  String resp=httpsPost("power_service/v1/site/get_scen_info",
    "{\"site_id\":\""+gSiteId+"\"}",gAuthToken,gGtoken,true);
  if(resp.isEmpty()) return false;
  DynamicJsonDocument doc(24576);
  if(deserializeJson(doc,resp)!=DeserializationError::Ok){
    Serial.printf("[Data] JSON err: %.80s\n",resp.c_str());
    return false;
  }
  int apiCode=doc["code"]|-1;
  if(apiCode!=0){if(apiCode==401||apiCode==9999)gAuthToken="";return false;}
  auto sb=doc["data"]["solarbank_info"];
  auto gi=doc["data"]["grid_info"];
  float pv=jF(sb["total_photovoltaic_power"]);
  if(pv==0) pv=jF(sb["solar_power"]);
  float batt_pct=0;
  if(sb["solarbank_list"].is<JsonArray>()&&sb["solarbank_list"].size()>0)
    batt_pct=jF(sb["solarbank_list"][0]["battery_power"]);
  if(batt_pct==0) batt_pct=jF(sb["total_battery_power"]);
  float batt_in =jF(sb["total_charging_power"]);
  float batt_out=jF(sb["battery_discharge_power"]);
  if(batt_in<0){batt_out=-batt_in;batt_in=0;}
  float home=jF(sb["to_home_load"]);
  float grid=jF(gi["grid_to_home_power"])-jF(gi["photovoltaic_to_grid_power"]);
  gData={pv,(batt_pct/100.f)*cfg.battWh,batt_pct,home,
         fabsf(grid)<0.5f?0.f:grid,
         batt_in <0.5f?0.f:batt_in,
         batt_out<0.5f?0.f:batt_out,true};
  Serial.printf("[Data] PV=%.0fW SOC=%.0f%% Grid=%.0fW In=%.0fW Out=%.0fW\n",
    gData.solar_w,gData.battery_pct,gData.grid_w,gData.batt_in_w,gData.batt_out_w);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DISPLAY ZEICHNEN
// ─────────────────────────────────────────────────────────────────────────────
static void drawMain(){
  bool useSprite=spr.getBuffer()!=nullptr;
  lgfx::LovyanGFX* g=useSprite?(lgfx::LovyanGFX*)&spr:(lgfx::LovyanGFX*)&lcd;
  g->fillScreen(C_BLACK);
  g->setTextDatum(lgfx::TC_DATUM);
  char buf[16];

  struct tm ti;
  if(getLocalTime(&ti)){
    char t[6],d[11];
    strftime(t,sizeof(t),"%H:%M",&ti);
    strftime(d,sizeof(d),"%d.%m.%Y",&ti);
    g->setFont(&fonts::FreeSansBold18pt7b); g->setTextColor(C_WHITE,C_BLACK);
    g->drawString(t,120,10);
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->drawString(d,120,56);
  }

  if(!gData.valid){
    // Steht die MQTT-Verbindung, warten wir nur auf die erste Nachricht –
    // das ist der Normalfall beim Start und keine Stoerung. Rot bleibt
    // dem Fall vorbehalten, in dem tatsaechlich keine Verbindung besteht.
    bool linked = gMqtt.connected();
    g->setFont(&fonts::FreeSansBold12pt7b);
    g->setTextColor(linked?C_ORANGE:C_RED, C_BLACK);
    g->drawString(linked?"Decodiere Daten":"Keine Verbindung", 120, 108);
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->drawString(linked?"Warte auf Solarbank":"Verbinde mit Anker...", 120, 138);
    // IP-Adresse: ueber sie laeuft die Weboberflaeche
    if(WiFi.status()==WL_CONNECTED){
      g->setTextColor(C_BLUE,C_BLACK);
      g->drawString(WiFi.localIP().toString().c_str(), 120, 168);
    }
    if(useSprite) spr.pushSprite(0,0);
    return;
  }

  uint32_t battCol=gData.battery_pct<20?C_RED:gData.battery_pct<50?C_YELLOW:C_GREEN;
  uint32_t gridCol=C_BLUE; const char* gridLabel="NETZ";
  if     (gData.grid_w> 0.5f){gridCol=C_RED;  gridLabel="BEZUG";}
  else if(gData.grid_w<-0.5f){gridCol=C_GREEN;gridLabel="EINSP";}
  uint32_t flowCol=C_GRAY; const char* flowLabel="--";
  float flowVal=0; bool hasFlow=false;
  if     (gData.batt_in_w >0.5f){flowCol=C_GREEN;flowLabel="EINGANG";flowVal=gData.batt_in_w; hasFlow=true;}
  else if(gData.batt_out_w>0.5f){flowCol=C_RED;  flowLabel="AUSGANG";flowVal=gData.batt_out_w;hasFlow=true;}

  g->setFont(&fonts::FreeSans9pt7b);
  g->setTextColor(C_YELLOW,C_BLACK); g->drawString("PV",     38, 76);
  g->setTextColor(C_GRAY,  C_BLACK); g->drawString("AKKU",  120, 76);
  g->setTextColor(gridCol, C_BLACK); g->drawString(gridLabel,202, 76);

  snprintf(buf,sizeof(buf),"%.0f",gData.solar_w);
  g->setFont(&fonts::FreeSansBold12pt7b); g->setTextColor(C_WHITE,C_BLACK);
  g->drawString(buf,38,92);
  g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_YELLOW,C_BLACK);
  g->drawString("W",38,114);

  snprintf(buf,sizeof(buf),"%d%%",(int)gData.battery_pct);
  g->setFont(&fonts::FreeSansBold18pt7b); g->setTextColor(battCol,C_BLACK);
  g->drawString(buf,120,100);

  snprintf(buf,sizeof(buf),"%.0f",fabsf(gData.grid_w));
  g->setFont(&fonts::FreeSansBold12pt7b); g->setTextColor(C_WHITE,C_BLACK);
  g->drawString(buf,202,92);
  g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(gridCol,C_BLACK);
  g->drawString("W",202,114);

  const int bx=120,by=150;
  g->drawRect(bx-18,by-8,36,16,flowCol);
  g->fillRect(bx+18,by-4,5,8,flowCol);
  if(gData.batt_in_w>0.5f){
    g->fillTriangle(bx,by-20,bx-7,by-10,bx+7,by-10,flowCol);
  } else if(gData.batt_out_w>0.5f){
    g->fillTriangle(bx,by+20,bx-7,by+10,bx+7,by+10,flowCol);
  } else {
    g->drawLine(bx-6,by,bx+6,by,C_GRAY);
  }

  g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(flowCol,C_BLACK);
  g->drawString(flowLabel,120,174);
  g->setTextColor(C_GRAY,C_BLACK);
  g->drawString("wischen >",120,212);
  g->setFont(&fonts::FreeSansBold12pt7b);
  g->setTextColor(hasFlow?C_WHITE:C_GRAY,C_BLACK);
  snprintf(buf,sizeof(buf),"%.0fW",flowVal);
  g->drawString(buf,120,192);

  if(useSprite) spr.pushSprite(0,0);
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n[BOOT] Anker Display " FW_VERSION);
  lcd.init();
  { // Ausrichtung vor dem ersten Zeichnen setzen. loadConfig() kommt erst
    // spaeter, deshalb hier direkt aus dem NVS lesen.
    prefs.begin("anker",true);
    lcd.setRotation(prefs.getInt("rot",0)&3);
    prefs.end();
  }
  lcd.setBrightness(200); lcd.fillScreen(C_BLACK);
  spr.setColorDepth(8);
  if(!spr.createSprite(240,240)) Serial.println("[SPR] RAM zu wenig");
  else                           Serial.println("[SPR] OK");

  loadConfig();
  if(!configComplete()||!siteSelected()){startConfigPortal();return;}

  lcd.fillScreen(C_BLACK);
  dispCenter(100,"Verbinde WLAN...",    C_WHITE, &fonts::FreeSans9pt7b);
  dispCenter(125,cfg.wifiSsid.c_str(), C_YELLOW,&fonts::FreeSans9pt7b);
  WiFi.mode(WIFI_STA); WiFi.begin(cfg.wifiSsid.c_str(),cfg.wifiPass.c_str());
  int tries=0;
  while(WiFi.status()!=WL_CONNECTED&&tries<40){delay(500);Serial.print(".");tries++;}
  if(WiFi.status()!=WL_CONNECTED){
    dispMsg("WLAN-Fehler","Konfig pruefen...",C_RED,0);
    delay(3000); startConfigPortal(); return;
  }
  Serial.printf("\n[WiFi] %s\n",WiFi.localIP().toString().c_str());
  lcd.fillScreen(C_BLACK);
  dispCenter( 90,"WLAN verbunden",                   C_GREEN,&fonts::FreeSansBold12pt7b);
  dispCenter(120,WiFi.localIP().toString().c_str(),  C_GRAY, &fonts::FreeSans9pt7b);
  delay(1200);

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3","pool.ntp.org","1.de.pool.ntp.org");
  Serial.print("[NTP] Warte...");
  struct tm ti; int ntpTries=0;
  while(!getLocalTime(&ti)&&ntpTries<20){delay(500);Serial.print(".");ntpTries++;}
  Serial.println(ntpTries<20?" OK":" Timeout");
  loadEnergy();   // Tagesertrag fortsetzen, wenn es noch derselbe Tag ist

  dispCenter(110,"ECDH Init...",C_GRAY,&fonts::FreeSans9pt7b);
  if(!ecdhInit()){dispMsg("ECDH Fehler","Neustart...",C_RED,0);delay(3000);ESP.restart();return;}

  dispMsg("Anker Login...",cfg.siteName.length()>0?cfg.siteName.c_str():cfg.ankerEmail.c_str(),C_WHITE,C_GRAY);
  if(!ankerLogin()){startFixPortal();return;}

  gSiteId=cfg.siteId;
  if(fetchMqttCreds() && fetchDeviceInfo()) mqttConnect();
  // fetchData() entfaellt: get_scen_info liefert nur 463, die Werte
  // kommen jetzt vollstaendig ueber MQTT.
  startWebUi();
  lcd.fillScreen(C_BLACK);
  drawDisplay();
}

// Zweite Seite: Netzwerk und Zustand. Ueber die IP laeuft die Weboberflaeche,
// und genau die sucht man erfahrungsgemaess dann, wenn das Geraet schon
// irgendwo haengt und man nicht mehr an den seriellen Anschluss kommt.
static void drawInfo(){
  bool useSprite=spr.getBuffer()!=nullptr;
  lgfx::LovyanGFX* g=useSprite?(lgfx::LovyanGFX*)&spr:(lgfx::LovyanGFX*)&lcd;
  g->fillScreen(C_BLACK);
  g->setTextDatum(lgfx::TC_DATUM);
  char b[16];

  bool wifi = WiFi.status()==WL_CONNECTED;
  g->setFont(&fonts::FreeSansBold12pt7b);
  g->setTextColor(wifi?C_GREEN:C_RED,C_BLACK);
  g->drawString(wifi?WiFi.localIP().toString().c_str():"kein WLAN",120,42);

  // Beschriftung klein ueber dem Wert. Nebeneinander wuerden Label und Wert
  // in der grossen Schrift zusammenstossen - zwei Spalten lassen dafuer
  // nicht genug Breite. Die Ueberschrift "PANELS" entfaellt dadurch.
  for(int i=0;i<4;i++){
    int col=(i&1)?168:72, row=(i<2)?72:116;
    snprintf(b,sizeof(b),"PV%d",i+1);
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->drawString(b,col,row);
    snprintf(b,sizeof(b),"%.0f W",gPvStr[i]);
    g->setFont(&fonts::FreeSansBold12pt7b); g->setTextColor(C_YELLOW,C_BLACK);
    g->drawString(b,col,row+14);
  }

  g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_ORANGE,C_BLACK);
  g->drawString("AKKU",120,162);

  // Je Pack ein Wert, wie in der App. Gemittelt ueber die vier Fuehler des
  // Packs - einzeln weichen sie um bis zu ein Grad voneinander ab.
  int n=0; String line;
  for(int k=0;k<MAX_PACKS && n<3;k++){
    if(!gPacks[k].valid) continue;
    int sum=0;
    for(int t=0;t<4;t++) sum+=gPacks[k].temp[t];
    // Kein Gradzeichen: die GFX-Schriften decken nur 0x20..0x7E ab,
    // 0xB0 kaeme als Luecke heraus.
    snprintf(b,sizeof(b),"%dC", (int)(sum/40.0f+0.5f));
    if(n) line+="  ";
    line+=b; n++;
  }
  g->setFont(&fonts::FreeSansBold12pt7b);
  g->setTextColor(n?C_WHITE:C_GRAY,C_BLACK);
  g->drawString(n?line.c_str():"--",120,178);

  if(useSprite) spr.pushSprite(0,0);
}

// Dritte Seite: Vorhersage fuer heute und morgen. Zeilenweise beschriftet,
// zwei Spalten - so bleibt jede Zeile schmal genug fuer den Kreis.
static void drawWeather(){
  bool useSprite=spr.getBuffer()!=nullptr;
  lgfx::LovyanGFX* g=useSprite?(lgfx::LovyanGFX*)&spr:(lgfx::LovyanGFX*)&lcd;
  g->fillScreen(C_BLACK);
  g->setTextDatum(lgfx::TC_DATUM);
  char b[20];

  if(cfg.lat==0 && cfg.lon==0){
    g->setFont(&fonts::FreeSansBold12pt7b); g->setTextColor(C_ORANGE,C_BLACK);
    g->drawString("WETTER",120,86);
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->drawString("Standort fehlt",120,116);
    g->drawString("im Browser eintragen",120,138);
    if(useSprite) spr.pushSprite(0,0);
    return;
  }
  if(!gWxValid){
    g->setFont(&fonts::FreeSansBold12pt7b); g->setTextColor(C_ORANGE,C_BLACK);
    g->drawString("WETTER",120,96);
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->drawString("keine Daten",120,126);
    if(useSprite) spr.pushSprite(0,0);
    return;
  }

  g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_ORANGE,C_BLACK);
  g->drawString("HEUTE",104,46);
  g->drawString("MORGEN",180,46);

  const char* lbl[4]={"GRAD","SONNE","WOLKE","REGEN"};
  for(int r=0;r<4;r++){
    int y=74+r*29;
    g->setFont(&fonts::FreeSans9pt7b); g->setTextColor(C_GRAY,C_BLACK);
    g->setTextDatum(lgfx::TL_DATUM);
    g->drawString(lbl[r],24,y+3);
    g->setTextDatum(lgfx::TC_DATUM);
    g->setFont(&fonts::FreeSansBold12pt7b);
    for(int d=0;d<2;d++){
      WxDay& w=gWx[d];
      uint32_t col=C_WHITE;
      switch(r){
        case 0: snprintf(b,sizeof(b),"%.0f/%.0f",w.tmax,w.tmin); break;
        case 1: snprintf(b,sizeof(b),"%.1fh",w.sunH);  col=C_YELLOW; break;
        case 2: snprintf(b,sizeof(b),"%.0f%%",w.cloud); col=w.cloud>60?C_GRAY:C_WHITE; break;
        default:snprintf(b,sizeof(b),"%.1fmm",w.rain);  col=w.rain>0.5f?C_BLUE:C_GRAY; break;
      }
      g->setTextColor(col,C_BLACK);
      g->drawString(b, d?180:104, y);
    }
  }
  if(useSprite) spr.pushSprite(0,0);
}

void drawDisplay(){
  if(gPage==2)      drawWeather();
  else if(gPage==1) drawInfo();
  else              drawMain();
}

// ─────────────────────────────────────────────────────────────────────────────
// TOUCH
// Nur die waagerechte Wischgeste wird ausgewertet. Ein Antippen soll nichts
// ausloesen - das Geraet haengt an der Wand und wird beim Abstauben beruehrt.
// ─────────────────────────────────────────────────────────────────────────────
// Holt die Vorhersage fuer heute und morgen. Wird alle 30 Minuten
// aufgefrischt - haeufiger waere sinnlos, das Modell rechnet stuendlich.
bool fetchWeather(){
  if(cfg.lat==0 && cfg.lon==0) return false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  String url = "https://api.open-meteo.com/v1/forecast?latitude="+String(cfg.lat,4)
             + "&longitude="+String(cfg.lon,4)
             + "&daily=temperature_2m_max,temperature_2m_min,sunshine_duration,"
               "cloud_cover_mean,precipitation_sum&forecast_days=2&timezone=auto";
  h.begin(c,url);
  h.setTimeout(10000);
  int code=h.GET();
  if(code!=200){ Serial.printf("[WX] HTTP %d\n",code); h.end(); return false; }
  String body=h.getString(); h.end();

  DynamicJsonDocument doc(2048);
  if(deserializeJson(doc,body)!=DeserializationError::Ok){
    Serial.println("[WX] JSON-Fehler"); return false;
  }
  JsonObject d=doc["daily"];
  if(d.isNull()){ Serial.println("[WX] kein daily-Block"); return false; }
  for(int i=0;i<2;i++){
    gWx[i].tmax = d["temperature_2m_max"][i] | 0.0f;
    gWx[i].tmin = d["temperature_2m_min"][i] | 0.0f;
    gWx[i].sunH = (d["sunshine_duration"][i] | 0.0f) / 3600.0f;
    gWx[i].cloud= d["cloud_cover_mean"][i]   | 0.0f;
    gWx[i].rain = d["precipitation_sum"][i]  | 0.0f;
  }
  gWxValid=true;
  Serial.printf("[WX] heute %.0f/%.0f C  %.1f h Sonne  %.0f%% Wolken  %.1f mm\n",
                gWx[0].tmax,gWx[0].tmin,gWx[0].sunH,gWx[0].cloud,gWx[0].rain);
  return true;
}

static void handleTouch(){
  static bool     down=false;
  static int32_t  xMin=0,xMax=0;
  static uint32_t tStart=0, samples=0;
  int32_t x,y;

  if(lcd.getTouch(&x,&y)){
    if(!down){ down=true; xMin=xMax=x; tStart=millis(); samples=0; }
    if(x<xMin) xMin=x;
    if(x>xMax) xMax=x;
    samples++;
    return;
  }
  if(!down) return;

  down=false;
  uint32_t dur=millis()-tStart;
  int32_t  span=xMax-xMin;

  // Nur Wischen wechselt die Seite, Antippen nicht - das Geraet haengt an
  // der Wand und wird beim Abstauben beruehrt. Gemessen wird die groesste
  // Spanne waehrend der Beruehrung, nicht die Differenz zwischen erstem und
  // letztem Wert: der CST816S antwortet zeitweise gar nicht, und dann waere
  // die Differenz null, obwohl gewischt wurde.
  if(span>30 && dur<1500){
    gPage = (gPage+1) % PAGES;
    lcd.fillScreen(C_BLACK);
    drawDisplay();
  }
  LOGF("[TOUCH] Seite %d  abtastungen=%lu strecke=%ld dauer=%lums\n",
       gPage,(unsigned long)samples,(long)span,(unsigned long)dur);
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────────────────
static unsigned long lastFetch=0, lastClock=0;
void loop(){
  unsigned long now=millis();
  handleTouch();           // Wischgeste auswerten
  server.handleClient();   // Weboberflaeche bedienen
  // MQTT am Leben halten – ohne loop() kommen keine Nachrichten an
  if(gMqtt.connected()){
    gMqtt.loop();
    // Erst lauschen, dann triggern – so sieht man was ungetriggert ankommt
    if(!gTriggerArmed && now-gMqttConnectedAt>=25000){
      gTriggerArmed=true;
      Serial.println("\n[TRG] Lauschphase vorbei – Trigger wird gesendet");
      sendRealtimeTrigger(300);
      gLastTrigger=now;
    } else if(gTriggerArmed && now-gLastTrigger>=120000){
      sendRealtimeTrigger(300);
      gLastTrigger=now;
    }
  } else if(gMqttHost.length() && now-gMqttLastTry>=15000){
    gMqttLastTry=now;
    Serial.println("[MQTT] Getrennt – neuer Versuch");
    mqttConnect();
  }
  // REST-Abfrage entfaellt – die Daten kommen jetzt per MQTT.
  // Anzeige hoechstens alle 2 s neu zeichnen, sonst flackert es bei 3-s-Daten.
  if(now-lastFetch>=2000){
    if(WiFi.status()!=WL_CONNECTED){WiFi.reconnect();delay(3000);}
    drawDisplay(); lastFetch=now;
  }
  // Uhr auch ohne gueltige Daten weiterlaufen lassen
  // Wetter alle 30 Minuten - das Modell rechnet stuendlich, oefter waere sinnlos
  if(cfg.lat!=0 || cfg.lon!=0){
    if(gWxLast==0 || now-gWxLast>=1800000UL){
      gWxLast=now;
      fetchWeather();
    }
  }
  if(now-lastClock>=30000){
    drawDisplay();
    lastClock=now;
  }
  delay(10);   // kurz, damit MQTT zuegig reagiert
}
