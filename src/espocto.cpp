/**
 * Play CHIP-8 ROMs on an esp32-2432s028r
 */ 
#define LGFX_USE_V1

#include <string.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/LGFX_Button.hpp>
#include <SPI.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <octo_emulator.h>

#include "console.h"
#include "credentials.h"

class LGFX : public lgfx::LGFX_Device
{
lgfx::Panel_ILI9341   _panel_instance;
lgfx::Bus_SPI         _bus_instance;
lgfx::Light_PWM       _light_instance;
lgfx::Touch_XPT2046   _touch_instance;

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();    // Gets the structure for bus configuration. 
      cfg.spi_host = HSPI_HOST;     // Select the SPI to use 
      cfg.spi_mode = 0;             // Set SPI communication mode (0 ~ 3) 
      cfg.freq_write = 55000000;    // SPI clock at the time of transmission (up to 80MHz, rounded to the value obtained by dividing 80MHz by an integer) // 送信時のSPIクロック (最大80MHz, 80MHzを整数で割った値に丸められます)
      cfg.freq_read  = 20000000;    // SPI clock when receiving 
      cfg.spi_3wire  = false;       // Set true when receiving with MOSI pin 
      cfg.use_lock   = true;        // Set to true when using transaction lock
      cfg.dma_channel = ILI9341_SPI_DMA_CHANNEL;          // Set the DMA channel (1 or 2. 0=disable)
      cfg.pin_sclk = ILI9341_SPI_BUS_SCLK_IO_NUM;            // Set SPI SCLK pin number 
      cfg.pin_mosi = ILI9341_SPI_BUS_MOSI_IO_NUM;            // Set SPI MOSI pin number
      cfg.pin_miso = ILI9341_SPI_BUS_MISO_IO_NUM;            // Set SPI MISO pin number (-1 = disable) 
      cfg.pin_dc   = ILI9341_SPI_CONFIG_DC_GPIO_NUM;             // Set SPI D / C pin number (-1 = disable) 

      _bus_instance.config(cfg);    // The set value is reflected on the bus.
      _panel_instance.setBus(&_bus_instance);      // Set the bus on the panel.
    }

    { // Set the display panel control.
      auto cfg = _panel_instance.config();    // Gets the structure for display panel settings.
      cfg.pin_cs           =    ILI9341_SPI_CONFIG_CS_GPIO_NUM;  // Pin number to which CS is connected (-1 = disable) 
      cfg.pin_rst          =    -1;  // Pin number to which RST is connected (-1 = disable) 
      cfg.pin_busy         =    -1;  // Pin number to which BUSY is connected (-1 = disable) 
      cfg.memory_width     =   240;  // Maximum width supported by driver IC 
      cfg.memory_height    =   320;  // Maximum height supported by driver IC 
      cfg.panel_width      =   240;  // Actually displayable width 
      cfg.panel_height     =   320;  // Actually displayable height 
      cfg.offset_x         =     0;  // Amount of X-direction offset of the panel
      cfg.offset_y         =     0;  // Amount of Y-direction offset of the panel 
      cfg.offset_rotation  =     0;  // Offset of values in the direction of rotation 0 ~ 7 (4 ~ 7 are upside down) 
      cfg.dummy_read_pixel =     8;  // Number of dummy read bits before pixel reading 
      cfg.dummy_read_bits  =     1;  // Number of bits of dummy read before reading data other than pixels 
      cfg.readable         =  true;  // Set to true if data can be read 
      cfg.invert           = false;  // Set to true if the light and darkness of the panel is reversed 
      cfg.rgb_order        = false;  // Set to true if the red and blue of the panel are swapped 
      cfg.dlen_16bit       = false;  // Set to true for panels that send data length in 16-bit units 
      cfg.bus_shared       =  true;  // If the bus is shared with the SD card, set to true (bus control is performed with drawJpgFile etc.) 

      _panel_instance.config(cfg);
    }
    
    { // Set the backlight control. (Delete if not needed
      auto cfg = _light_instance.config();    // Gets the structure for the backlight setting. 

      cfg.pin_bl = BCKL;              // Pin number to which the backlight is connected 
      cfg.invert = false;           // True if you want to invert the brightness of the backlight 
      cfg.freq   = 44100;           // Backlight PWM frequency 
      cfg.pwm_channel = 7;          // PWM channel number to use 

      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);  // Set the backlight on the panel. 
    }

    { // Set the touch screen control (XPT2046 - SPI)
      auto cfg = _touch_instance.config();

      // --- Calibração (ajuste se necessário) ---
      cfg.x_max = 200;
      cfg.x_min = 3800;
      cfg.y_min = 200;
      cfg.y_max = 3800;

      // --- Pinos ---
      cfg.pin_int  = XPT2046_TOUCH_CONFIG_INT_GPIO_NUM;   // INT não usado (pode colocar se existir)
      cfg.pin_mosi   = XPT2046_SPI_BUS_MOSI_IO_NUM;
      cfg.pin_miso   = XPT2046_SPI_BUS_MISO_IO_NUM;
      cfg.pin_sclk   = XPT2046_SPI_BUS_SCLK_IO_NUM;
      cfg.pin_cs   = XPT2046_SPI_CONFIG_CS_GPIO_NUM;   // CS do touch (CYD 2.8 normalmente GPIO33)
      cfg.bus_shared = false; // Compartilha SPI com o display

      // --- SPI ---
      cfg.spi_host = XPT2046_SPI_HOST; // MESMO SPI do display
      cfg.freq     = 1000000;  // 1 MHz (estável)

      // --- Mapeamento de rotação ---
      cfg.offset_rotation = 2;

      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance); // Set the panel to be used. 
  }
};

AsyncWebServer* server;
const char* ssid = WLAN_SSID;
const char* password = WLAN_PASS;

char** prg;
int prgCount = 0;
int prgSpace = 0;
int currPrg = 0;

int ch8Size;

bool isMonitor = false;
uint16_t monitorAddr;
uint8_t monitorNibble;

enum {
  PAGE_MAIN,
  PAGE_SAVE
} page;

const int WIDTH = LCD_HEIGHT;
const int HEIGHT = LCD_WIDTH;

static LGFX lcd;
static LGFX_Sprite sprite(&lcd);

static char lbl[20][2] = { 
  "1", "2", "3", "C", "<",
  "4", "5", "6", "D", ">",
  "7", "8", "9", "E", "G",
  "A", "0", "B", "F", "M",
};

static LGFX_Button btn[20];

octo_emulator* emu;

const std::int8_t KEY_NONE = -1;

const std::int8_t KEY_LEFT = -2;
const std::int8_t KEY_RIGHT = -3;
const std::int8_t KEY_GO = -4;
const std::int8_t KEY_MONITOR = -5;

std::int8_t hexButton(std::uint8_t i) {
  char c = lbl[i][0];

  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  else
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 0xA;
  }
  else
  if (c == '<') return KEY_LEFT;
  else
  if (c == '>') return KEY_RIGHT;
  else
  if (c == 'G') return KEY_GO;
  else
  if (c == 'M') return KEY_MONITOR;
  else return KEY_NONE;
}

void drawButtons(void) {
  for (int col = 0; col <= 4; col++) {
    for (int row = 0; row <= 3; row++) {
      int n = 5 * row + col;

      btn[n].initButton(&lcd,
        36 + col * 42,      // x
        150 + row * 42,     // y
        36,                 // w
        36,                 // h
        0xFFFFCC00u,        // outline
        0xFF996600u,        // fill
        col == 4 ? 0xFFFF6600u : 0xFFFFCC00u,
                            // textcolor
        lbl[n],             // label
        1.0,                // textsize x
        1.0                 // textsize y
      );
      btn[n].drawButton();
    }
  }
}

void showCurrPrg(octo_emulator* emu) {
  lcd.fillRect(0, 0, 240, 18, 0xFFFFCC00u);

  lcd.setTextColor(0xFF996600u, 0xFFFFCC00u);
  lcd.drawNumber(emu->options.tickrate, 2, 0, &fonts::FreeMonoBold9pt7b);
  char* p = strrchr(prg[currPrg], '.');
  *p = '\0';
  lcd.drawCenterString(prg[currPrg], 120, 0, &fonts::FreeMonoBold9pt7b);
  *p = '.';
  lcd.setTextColor(0xFFFFCC00u, 0xFF996600u);
}

bool loadPrg(char* filename, octo_emulator* emu) {
  File f = SPIFFS.open(filename);
  if (!f) {
    return false;
  }
  int size = f.size();
  char* info = (char*)malloc(size);
  f.read((uint8_t*)info, size);
  f.close();

  ch8Size = size - sizeof(octo_options);
  octo_emulator_init(emu, info + sizeof(octo_options), ch8Size, (octo_options*)info, NULL);
  free(info);

  monitorAddr = 0x200;
  monitorNibble = 0;

  showCurrPrg(emu);
  return true;
}

bool savePrg(char* filename, octo_emulator* emu) {
  File f = SPIFFS.open(filename, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.write((unsigned char*)&emu->options, sizeof(octo_options));
  f.write(emu->ram, ch8Size);
  f.close();
  return true;
}

void loadCurrPrg(octo_emulator* emu) {
  if (!prg || !prg[currPrg]) {
    console_printf("DEBUG prg=%p currPrg=%d\r\n", prg, currPrg);

    if (prg) {
      for (int i = 0; i < 5; i++) {
        console_printf("prg[%d]=%p\r\n", i, prg[i]);
      }
    }

    console_printf("Invalid program index\r\n");
    return;
  }

  char* path = (char*) malloc(10 + strlen(prg[currPrg]));
  
  strcpy(path, "/");
  strcat(path, prg[currPrg]);
  if (loadPrg(path, emu)) {
    console_printf("Loaded %s\r\n", path);
  }
  else {
    console_printf("Failed to load %s\r\n", path);
  }
  free(path);
}

bool isValidEc8(const char* name) {
  if (!name) return false;

  size_t len = strlen(name);
  if (len < 4) return false;

  // termina com ".ec8"
  return strcmp(name + len - 4, ".ec8") == 0;
}

void loadPrgInfo() {
  File d = SPIFFS.open("/");

  if (!d) {
    Serial.println("Failed to open directory!");
    return; // OU trate o erro
  }

  prg = (char**)malloc(30 * sizeof(char*));
  prgSpace = 30;

  File f = d.openNextFile();
  while (f) {
    const char* name = f.name();
    if (strcmp(name + strlen(name) - 4, ".ec8") != 0) {
      f = d.openNextFile();
      continue;
    }

    if (prgSpace < ++prgCount) {
      prgSpace += 30;
      prg = (char**)realloc(prg, prgSpace * sizeof(char*));
    }

    prg[prgCount - 1] = strdup(name);
  
    f = d.openNextFile();
  }
  console_printf("%d files read.\r\n", prgCount);
}

#if 0
#include <Arduino.h>
#include <driver/i2s.h>

#define AUDIO_FRAG_SIZE 1024
#define AUDIO_SAMPLE_RATE (4096*8)

// I2S Konfiguration
#define I2S_NUM         (0) // I2S port number
#define I2S_BCK_IO      (26) // Bit Clock Pin
#define I2S_WS_IO       (25) // Word Select Pin
#define I2S_DO_IO       (22) // Data Out Pin
#define I2S_DI_IO       (-1) // Data In Pin, nicht verwendet

void audio_pump(void *user, int16_t *stream, int len) {
  octo_emulator *emu = (octo_emulator*)user;
  double freq = 4000 * pow(2, (emu->pitch - 64) / 48.0);
  for (int z = 0; z < len; z++) {
    int ip = emu->osc;
    stream[z] = !emu->had_sound ? 0 : (emu->pattern[ip >> 3] >> ((ip & 7) ^ 7)) & 1 ? INT16_MAX * ui.volume : 0;
    emu->osc = fmod(emu->osc + (freq / AUDIO_SAMPLE_RATE), 128.0);
  }
  emu->had_sound = 0;
}

void audio_init(octo_emulator *emu) {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono-Ausgabe
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0, // Standard Interrupt Priority
    .dma_buf_count = 8,
    .dma_buf_len = AUDIO_FRAG_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = true, // Auto clear tx descriptor on underflow
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCK_IO,
    .ws_io_num = I2S_WS_IO,
    .data_out_num = I2S_DO_IO,
    .data_in_num = I2S_DI_IO
  };

  // I2S Treiber installieren und Konfiguration für I2S port setzen
  i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM, &pin_config);

  // Optional: I2S Bits pro Sample und Kanal-Format einstellen
  i2s_set_clk(I2S_NUM, AUDIO_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

// Die audio_pump Funktion muss periodisch aufgerufen werden, um das Audiobuffer zu füllen.
// Dies könnte in einem dedizierten Task oder in der main loop erfolgen, abhängig von der Struktur Ihrer Anwendung.
#endif

void notFound(AsyncWebServerRequest* request) {
  request->send(404, "text/plain", "Not found");
}

char* instr(octo_emulator* emu, uint16_t addr) {
  uint8_t hi = emu->ram[addr], lo = emu->ram[addr+1], op = hi >> 4;  
  uint16_t wd = hi;
  wd <<= 8; wd |= lo;
  static char buf[13]; 
  switch (op) {
    case 0x0:
      switch (wd) {
        case 0x00E0:
          return "cls";
        case 0x00EE:
          return "ret";
        case 0x00FF:
          return "hires";
        case 0x00FE:
          return "lores";
        case 0x00FD:
          return "exit";
        case 0x00FB:
          return "scr";
        case 0x00FC:
          return "scl";
        default:
          if (lo & 0xF0 == 0xC0) {
            snprintf(buf, 8, "scd d %X", lo & 0x0F);
          }
          else {
            buf[0] = '\0';
          }
          return buf;
      }
      break;
    case 0x1:
      snprintf(buf, 7, "jp %03X", wd & 0xFFF);
      return buf;
    case 0x2:
      snprintf(buf, 9, "call %03X", wd & 0xFFF);
      return buf;
    case 0x3:
      snprintf(buf, 9, "se v%X,%02X", hi & 0xF, lo);
      return buf;
    case 0x4:
      snprintf(buf, 10, "sne v%X,%02X", hi & 0xF, lo);
      return buf;
    case 0x5:
      snprintf(buf, 9, "se v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
      return buf;
    case 0x6:
      snprintf(buf, 9, "ld v%X,%02X", hi & 0xF, lo);
      return buf;
    case 0x7:
      snprintf(buf, 10, "add v%X,%02X", hi & 0xF, lo);
      return buf;
    case 0x8:
      switch (lo & 0xF) {
        case 0x0:
          snprintf(buf, 9, "ld v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x1:
          snprintf(buf, 9, "or v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x2:
          snprintf(buf, 10, "and v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x3:
          snprintf(buf, 10, "xor v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x4:
          snprintf(buf, 10, "add v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x5:
          snprintf(buf, 10, "sub v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x6:
          snprintf(buf, 10, "shr v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0x7:
          snprintf(buf, 11, "subn v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        case 0xE:
          snprintf(buf, 10, "shl v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
          return buf;
        default: 
          buf[0] = '\0';
          return buf;
      }
    case 0x9:
      snprintf(buf, 10, "sne v%X,v%X", hi & 0xF, lo & 0xF0 >> 4);
      return buf;
    case 0xa:
      snprintf(buf, 9, "ld i %03X", wd & 0xFFF);
      return buf;
    case 0xb:
      snprintf(buf, 9, "jp v0,%03X", wd & 0xFFF);
      return buf;
    case 0xc:
      snprintf(buf, 9, "rnd v%X,%02X", hi & 0xF, lo);
      return buf;
    case 0xd:
      snprintf(buf, 12, "drw v%X,v%X,%X", hi & 0xF, lo >> 4, lo & 0xF);
      return buf;
    case 0xe:
      switch (lo) {
        case 0x9E:
          snprintf(buf, 7, "skp v%X", hi & 0xF);
          return buf;
        case 0xA1:
          snprintf(buf, 8, "sknp v%X", hi & 0xF);
          return buf;
        default: 
          buf[0] = '\0';
          return buf;
      }
    case 0xf:
      switch (lo) {
        case 0x07:
          snprintf(buf, 9, "ld v%X,dt", hi & 0xF);
          return buf;
        case 0x0A:
          snprintf(buf, 8, "ld v%X,k", hi & 0xF);
          return buf;
        case 0x15:
          snprintf(buf, 9, "ld dt,v%X", hi & 0xF);
          return buf;
        case 0x18:
          snprintf(buf, 9, "ld st,v%X", hi & 0xF);
          return buf;
        case 0x1E:
          snprintf(buf, 9, "add i,v%X", hi & 0xF);
          return buf;
        case 0x29:
          snprintf(buf, 8, "ld f,v%X", hi & 0xF);
          return buf;
        case 0x33:
          snprintf(buf, 8, "ld b,v%X", hi & 0xF);
          return buf;
        case 0x55:
          snprintf(buf, 10, "ld [i],v%X", hi & 0xF);
          return buf;
        case 0x65:
          snprintf(buf, 10, "ld v%X,[i]", hi & 0xF);
          return buf;
        default: 
          buf[0] = '\0';
          return buf;
      }
    default: 
      buf[0] = '\0';
      return buf;
  }
}

String filesInfo(const String& var) {
  if (var == "FILELIST") {
    String html;
    File root = SPIFFS.open("/");
    File file = root.openNextFile();

    while (file) {
      String name = file.name();

      if (name.endsWith(".ec8")) {
        html += "<p>";
        html += name;
        html += " <a href=\"/delete?file=" + name + "\">[delete]</a>";
        html += "</p>";
      }
      file = root.openNextFile();
    }

    if (html.isEmpty()) {
      html = "<p><i>No .ec8 files</i></p>";
    }

    return html;
  }
  return String();
}

String webInfo(const String& var) {
  if (var == "NAME") {
    char* p = strrchr(prg[currPrg], '.');
    *p = '\0';
    String name(prg[currPrg]);
    *p = '.';
    return name;
  }
  else
  if (var == "CODE") {
    char line[26];
    String buffer;
    uint16_t addr;
    for (addr = 0x200; addr < ch8Size + 0x200; addr += 2) {
      snprintf(line, 24, "%04X: %02X%02X %s\n", addr,
        emu->ram[addr], emu->ram[addr+1],
        instr(emu, addr));
      buffer += line;
    }
    return buffer;
  }
  return String();
}

void setup(void)
{
  lcd.init();
  lcd.setRotation(2);
  lcd.setColorDepth(16);
  lcd.fillScreen(0xFF000000u);
  lcd.setFont(&fonts::FreeMonoBold12pt7b);

  Serial.begin(115200);

  while (!SPIFFS.begin(true)) {
    console_printf("SPIFFS.begin failed!\r\n");
    lcd.drawString("SPIFFS not initialized!", 0, 0, &fonts::FreeMonoBold12pt7b);
    delay(500);
  }
  lcd.fillScreen(0xFF000000u);

  loadPrgInfo();

  emu = (octo_emulator*)calloc(1, sizeof(octo_emulator));

  console_printf("Connecting...\r\n");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    console_printf("WiFi failed!\r\n");
    return;
  }
  server = new AsyncWebServer(80);
  console_printf("IP Address: %s\r\n", WiFi.localIP().toString().c_str());
  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html", false, webInfo);
  });

  server->on("/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/files.html", "text/html", false, filesInfo);
  });

  #include "esp_system.h"

  server->on(
    "/upload",
    HTTP_POST,
    [](AsyncWebServerRequest *request) {
      // nada aqui, resposta será enviada antes do reboot
      request->send(200, "text/plain", "Upload complete");
    },
    [](AsyncWebServerRequest *request,
      String filename,
      size_t index,
      uint8_t *data,
      size_t len,
      bool final) {

      static File uploadFile;
      static bool valid = false;
      static bool doReboot = false;

      // início do upload
      if (index == 0) {
        valid = filename.endsWith(".ec8");

        // lê checkbox
        doReboot = request->hasParam("reboot", true);

        if (!valid) return;

        String path = "/" + filename;
        if (SPIFFS.exists(path)) SPIFFS.remove(path);
        uploadFile = SPIFFS.open(path, FILE_WRITE);
      }

      if (!valid) return;

      if (uploadFile) {
        uploadFile.write(data, len);
      }

      if (final) {
        if (uploadFile) uploadFile.close();

        console_printf("Upload complete (%s)\r\n",
                      doReboot ? "reboot" : "no reboot");

        if (doReboot) {
          delay(150);
          esp_restart();
        }
      }
    }
  );

  server->on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->redirect("/files");
      return;
    }

    String filename = request->getParam("file")->value();

    if (filename.endsWith(".ec8") && SPIFFS.exists(filename)) {
      SPIFFS.remove(filename);
    }

    request->redirect("/files");
  });

  server->onNotFound(notFound);
  server->begin();

  loadCurrPrg(emu);
  drawButtons();

  page = PAGE_MAIN;
}

void ui_run(octo_emulator* emu) {
  // drop repaints if the display hasn't changed
  int dirty = memcmp(emu->px, emu->ppx, sizeof(emu->px)) != 0;

  if (!dirty) return;
  memcpy(emu->ppx,emu->px,sizeof(emu->ppx));

  // render chip8 display
  int w = emu->hires ? 128 : 64, h = emu->hires ? 64 : 32;
  float scale = emu->hires ? 1.5 : 3;

  static char lastRes = emu->hires;
  if (emu->hires != lastRes) {
    lastRes = emu->hires;
    lcd.fillCircle(10, 32, 4, emu->hires ? 0xFFFF6600u : 0xFF996600u);
    console_printf("%sres rot=%d w=%d h=%d scale=%f\r\n", emu->hires ? "hi" : "lo", emu->options.rotation, w, h, scale);
  }

  sprite.createSprite(w, h);
  sprite.setPivot(w / 2, 0);
  sprite.setColorDepth(4);
  sprite.setPaletteColor(0, 0xFF996600u);
  sprite.setPaletteColor(1, 0xFFFFCC00u);
  sprite.setPaletteColor(2, 0xFFFF6600u);
  sprite.setPaletteColor(3, 0xFF662200u);

  for(int y=0; y<h; y++) {
    for(int x=0; x<w; x++) {
      int c = emu->px[x + (y*w)];
      //console_printf("%d", c);
      sprite.drawPixel(x, y, c);
    }
    //console_printf("\n");
  }
  sprite.pushRotateZoom(lcd.width() / 2, 74, 0, scale * 1.3 , scale);
  sprite.deleteSprite();
}

void emu_step(octo_emulator* emu) {
  static bool flagged = false;
  if (emu->halt) {
    if (!flagged) {
        flagged = true;
        console_printf("halted\r\n");
    }
    return;
  }
  for (int z=0; z<emu->options.tickrate && !emu->halt; z++) {
    if (emu->options.q_vblank && (emu->ram[emu->pc]&0xF0) == 0xD0) {
        z=emu->options.tickrate;
    }
    //console_printf("pc=%0x", emu->pc);
    octo_emulator_instruction(emu);
  }
  if (emu->dt>0) emu->dt--;
  if (emu->st>0) emu->st--, emu->had_sound=1;
}

void showMonitor(octo_emulator* emu) {
  lcd.fillRect(0, 20, 240, 108, 0xFF996600u);

  uint16_t addr = monitorAddr - 2;

  char buf[25];
  for (int i = 0; i < 5; i++) {
    snprintf(buf, 24, "%04X:       %s", addr, instr(emu, addr));
    lcd.drawString(buf, 20, 24 + i*20, &fonts::AsciiFont8x16);

    snprintf(buf, 5, "%02X%02X", emu->ram[addr], emu->ram[addr+1]);
    char c[2];
    c[1] = '\0';
    for (int n = 0; n < 4; n++) {
      if (i == 1 && n == monitorNibble) {
        lcd.setTextColor(0xFF996600u, 0xFFFFCC00u);
      }
      else {
        lcd.setTextColor(0xFFFFCC00u, 0xFF996600u);
      }
      c[0] = buf[n];
      lcd.drawString(c, 20 + 48 + n*8, 24 + i*20, &fonts::AsciiFont8x16);
      lcd.setTextColor(0xFFFFCC00u, 0xFF996600u);
    }
    addr += 2;
  }
}

#if 0
void showSavePage(void) {
  static LGFX_Button buttons[40];
  LGFX_Button* btn;
  char lbl[4];
  int x, w;

  lcd.fillScreen(0xFF996600u);
  showCurrPrg(&emu);

  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 5; col++) {
      lbl[1] = '\0';
      x = 36 + col * 42;
      w = 36;

      if (row < 2) {
        lbl[0] = '0' + row * 5 + col;
      }
      else
      if (row < 7) {
        lbl[0] = 'A' + (row - 2) * 5 + col;
      }
      else {
        switch (col) {
          case 0:
            lbl[0] = 'Z';
            break;
          case 1:
            strcpy(lbl, "Del");
            x += 7;
            w = 50;
            break;
          case 2:
            strcpy(lbl, "Clr");
            x += 21;
            w = 50;
            break;
          case 4:
            strcpy(lbl, "OK!");
            x -= 8;
            w = 50;
            break;
          default:
            continue;
        }      
      }
      btn = &buttons[row * 5 + col];
      btn->initButton(&lcd,
        x,
        40 + row * 36,      // y
        w,
        30,     // h
        0xFFFFCC00u,        // outline
        0xFF996600u,        // fill
        0xFFFFCC00u,        // textcolor
        lbl,    // label
        1.0,    // textsize x
        1.0     // textsize y
      );
      btn->drawButton();
    }
  }
  page = PAGE_SAVE;
}
#endif

void handleTouchMain(octo_emulator* emu, int touchX, int touchY) {
  for (int i = 0; i < 20; i++) {
    if (btn[i].contains(touchX, touchY)) {
      btn[i].press(true);
      btn[i].drawButton(true);

      lcd.setTextColor(0xFF996600u, 0xFFFFCC00u);
      lcd.drawString(lbl[i], 228, 0, &fonts::FreeMonoBold9pt7b);
      lcd.setTextColor(0xFFFFCC00u, 0xFF996600u);

      std::int8_t b = hexButton(i);
  
      if (btn[i].justPressed()) {
        if (isMonitor) {
          if (b == KEY_LEFT) {
            if (monitorAddr >= 0x202) {
              monitorAddr -= 2;
              monitorNibble = 0;
              showMonitor(emu);
            }
          }
          else
          if (b == KEY_RIGHT) {
            if (monitorAddr < 4 * 1024 - 2) {
              monitorAddr += 2;
              monitorNibble = 0;
              showMonitor(emu);
            }
          }
          else
          if (b == KEY_GO) {
            console_printf("Saving...\r\n");
          }
          else
          if (b == KEY_MONITOR) {
            isMonitor = false;
            lcd.fillRect(0, 15, 240, 102, 0xFF996600u);
          }
          else {
            uint8_t* m = &emu->ram[monitorAddr];
            switch (monitorNibble) {
              case 0:
                *m = (*m & 0xF) | (b << 4);
                break;
              case 1:
                *m = (*m & 0xF0) | b;
                break;
              case 2:
                *(m+1) = (*(m+1) & 0xF) | (b << 4);
                break;
              case 3:
                *(m+1) = (*(m+1) & 0xF0) | b;
                break;
            }
            monitorNibble += 1;
            if (monitorNibble == 4) {
              monitorNibble = 0;
              monitorAddr += 2;
            }
            showMonitor(emu);
          }
        }
        else {
          // not isMonitor
          if (b == KEY_LEFT) {
            if (currPrg > 0) {
              currPrg -= 1;
              showCurrPrg(emu);
            }
          }
          else
          if (b == KEY_RIGHT) {
            if (currPrg < prgCount - 1) {
              currPrg += 1;
              showCurrPrg(emu);
            }
          }
          else
          if (b == KEY_GO) {
            loadCurrPrg(emu);
          }
          else
          if (b == KEY_MONITOR) {
            isMonitor = true;
            showMonitor(emu);
          }
        }
      }
      if (b >= 0) {
        emu->keys[b] = true;
      }
    }
  }
}

void handleUntouchMain(octo_emulator* emu) {
  for (int i = 0; i < 20; i++) {
    if (btn[i].isPressed()) {
      btn[i].press(false);
      btn[i].drawButton(false);

      std::int8_t b = hexButton(i);
      if (b >= 0) {
        emu->keys[b] = false;
      }
      lcd.fillRect(228, 0, 10, 18, 0xFFFFCC00u);
    }
  }
}

void handleTouchSave(octo_emulator* emu, int touchX, int touchY) {
}

void handleUntouchSave(octo_emulator* emu) {
}

unsigned long previousMillis = 0; // will store last time the function was called
const long interval = 22;// 33; // interval at which to call function (milliseconds)

void loop(void)
{
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= interval) {
    // save the last time the function was called
    previousMillis = currentMillis;

    bool touched;
    uint8_t gesture;
    uint16_t touchX, touchY;

    touched = lcd.getTouch(&touchX, &touchY);

    if (touched) {
      switch (page) {
        case PAGE_MAIN:
          handleTouchMain(emu, touchX, touchY);
        case PAGE_SAVE:
          handleTouchSave(emu, touchX, touchY);
      }
    }
    else {
      // not touched
      switch (page) {
        case PAGE_MAIN:
          handleUntouchMain(emu);
        case PAGE_SAVE:
          handleUntouchSave(emu);
      }
    }

    if (page == PAGE_MAIN && !isMonitor) { 
      emu_step(emu);
      ui_run(emu);
    }
  }
}

#if defined ( ESP_PLATFORM ) && !defined ( ARDUINO )
extern "C" {
int app_main(int, char**)
{
    setup();
    for (;;) {
      loop();
    }
    return 0;
}
}
#endif