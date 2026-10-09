// =============================================================================
//  ESP32-C3 Power Monitor v3.5.2 — монитор электропитания по пингу устройств в LAN
//  Интерфейс: Signal Board (адаптивный, светлая/тёмная тема), RU / UA / EN.
//  Плата: ESP32-C3 (Super Mini). Ядро: Arduino-ESP32 3.3.12. Библиотека: PubSubClient.
//  Основа кода: v3.0.1.1.
// =============================================================================
//
//  КАК РАБОТАЕТ УСТРОЙСТВО
//  1. Каждые N секунд (по умолчанию 10) плата пингует 1–2 устройства в локальной сети
//     (например, роутер и ещё что-то, что питается от той же сети).
//  2. Если устройства отвечают — электричество ЕСТЬ, если нет — НЕТ. Чтобы не реагировать на
//     случайную потерю пакетов, состояние меняется только после нескольких одинаковых проверок
//     подряд (по умолчанию 3 для «нет» и 2 для «есть»).
//  3. Результат: (а) показывается на веб-странице платы; (б) отправляется HTTP-запросом
//     (heartbeat, JSON) на 1–2 сервера; (в) публикуется по MQTT, в Home Assistant устройство
//     создаётся автоматически; (г) отключения считаются в статистике (хранится во флеш-памяти).
//
//  ДВА ПОТОКА ВЫПОЛНЕНИЯ (важно понимать, прежде чем править)
//  • monitorTask  — отдельная задача FreeRTOS: пинги, определение питания, отправка heartbeat.
//                   Долгие сетевые операции здесь не мешают веб-интерфейсу.  См. раздел [11].
//  • loop()       — основной поток Arduino: веб-сервер, MQTT, Wi-Fi, время, лог. См. раздел [20].
//  Обмен между ними: переменные g_* и d1_/d2_* (volatile — их можно читать из обоих потоков),
//  а настройки cfg защищены мьютексом mx. Из loop() настройки берутся через lc() (кэш),
//  из monitorTask — через configSnapshot() (копия). См. разделы [4] и [7].
//
//  КАРТА КОДА (ищите по номеру в квадратных скобках, например «[11]»)
//   [1]  Библиотеки
//   [2]  Константы сборки: Wi-Fi, логин, часовой пояс, режимы
//   [3]  Структура настроек Config
//   [4]  Глобальные объекты и общее состояние
//   [5]  Диагностический лог (Serial)
//   [6]  Статистика отключений (хранится во флеш)
//   [7]  Хранение настроек: загрузка, сохранение, проверка диапазонов
//   [8]  ICMP-ping
//   [9]  Отправка heartbeat на HTTP-серверы
//   [10] MQTT и Home Assistant
//   [11] Определение питания — задача monitorTask (главная логика)
//   [12] Вспомогательные функции веб-части
//   [13] Оформление страниц: CSS, переводы (I18N_JS), шапка и меню (head/sendPage)
//   [14] Страницы «Обзор» и «Настройки»
//   [15] API и сохранение настроек: /api/status, /api/config, /save, /mqtt/test
//   [16] Страница «Система» и системные действия: перезагрузка, сброс
//   [17] OTA-обновление прошивки
//   [18] События Wi-Fi
//   [19] setup() — запуск и таблица адресов (маршрутов)
//   [20] loop() — основной цикл
//
//  ШПАРГАЛКА: ЧТО ГДЕ МЕНЯТЬ
//  • Имя и пароль Wi-Fi ........................ [2] WIFI_SSID, WIFI_PASSWORD (затем перепрошить).
//  • Логин веб-интерфейса ....................... [2] ADMIN_USER.
//  • Пароль веб-интерфейса ...................... [2] DEFAULT_ADMIN_PASS действует только до первой
//                                                  смены в «Настройки → Безопасность»; после этого
//                                                  пароль хранится во флеш.
//  • Значения настроек по умолчанию ............. [7] loadConfig() (последний аргумент getInt/getBool/gs).
//  • Допустимые диапазоны настроек .............. [7] validateConfig(), [15] handleSave() (argInt)
//                                                  и min/max в полях формы [14] — менять во всех трёх.
//  • Экономный режим (проверки раз в 60 с,
//    пока питание отсутствует) .................. [2] ECO_CHECK_INTERVAL_SEC; логика в [11].
//  • Часовой пояс ............................... [2] TZ_UA.
//  • Состав JSON для HTTP-сервера ............... [9] sendHeartbeat().
//  • Сущности и тексты в Home Assistant ......... [10] mqttPublishDiscovery() (данные — mqttPublishState()).
//  • Цвета, размеры, тёмная/светлая тема ........ [13] CSS: переменные в самом начале (:root).
//  • Тексты интерфейса .......................... русские — внутри HTML-строк в [14] и [16];
//                                                  переводы UA/EN — таблица R в I18N_JS [13].
//  • Новая страница или адрес API ............... функция-обработчик + строка server.on(...) в [19].
//  • Отключить подробный лог в Serial ........... [2] DEBUG_DIAGNOSTICS = 0.
//  • Имя платы в сети (power-monitor.local) ..... setup(): WiFi.setHostname(...) и loop(): MDNS.begin(...).
//
//  КАК ДОБАВИТЬ НОВУЮ НАСТРОЙКУ (шесть мест)
//   1) поле в struct Config [3]            4) чтение из формы в handleSave() [15]
//   2) загрузка в loadConfig() [7]         5) отдача в браузер в handleApiConfig() [15]
//   3) запись в saveConfig() [7]           6) поле в форме handleSettings() [14] (name= тот же,
//      (+ проверка в validateConfig())        что в handleSave и handleApiConfig) и перевод подписи в [13]
//
//  ХРАНЕНИЕ ВО ФЛЕШ (NVS): пространство «pm-config» — настройки, «pm-stats» — статистика отключений.
//  ВНИМАНИЕ: в коде открыто лежат пароль Wi-Fi и данные входа — не публикуйте файл как есть.
// =============================================================================


// =============================================================================
// [1] БИБЛИОТЕКИ
// WiFi / WiFiClientSecure — Wi-Fi и защищённые (HTTPS) соединения.
// WebServer — веб-интерфейс платы (порт 80).   Update — прошивка по воздуху (OTA).
// Preferences — хранение настроек и статистики во флеш (NVS).
// HTTPClient — отправка heartbeat на HTTP/HTTPS-сервер.   PubSubClient — MQTT.
// ESPmDNS — доступ по имени power-monitor.local.   time.h — время (NTP) для статистики.
// esp_system / esp_heap_caps — причина перезапуска и данные о памяти.
// lwip/sockets — «сырые» сокеты для ICMP-ping.
// =============================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdarg.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "lwip/sockets.h"

// Прототипы setup()/loop() нужны, чтобы препроцессор Arduino на ядре 3.3.12 не путался (не
// удалять).
// Explicit prototypes avoid Arduino preprocessor conflicts on Core 3.3.12.
void setup();
void loop();


// =============================================================================
// [2] КОНСТАНТЫ СБОРКИ
// Меняются в исходнике и требуют перепрошивки. Всё остальное (интервалы, IP, серверы,
// MQTT) настраивается на веб-странице «Настройки» без перепрошивки.
// =============================================================================
#define FW_VERSION "3.5.2"  // версия прошивки: шапка страниц, heartbeat, Home Assistant
#define DEBUG_DIAGNOSTICS 0  // 1 = подробный лог в Serial (115200), 0 = выключить
#define HEALTH_INTERVAL_MS 60000UL  // как часто писать строку HEALTH в лог, мс
#define ECO_CHECK_INTERVAL_SEC 60  // интервал проверок, с, пока питание подтверждённо отсутствует
#define FW_BUILD __DATE__ " " __TIME__  // дата и время сборки (подставляет компилятор)
#define WIFI_SSID "ssid"  // имя вашей Wi-Fi сети
#define WIFI_PASSWORD "passw" // пароль вашей Wi-Fi сети
#define ADMIN_USER "123"  // логин веб-интерфейса
#define DEFAULT_ADMIN_PASS "321"  // пароль до первой смены в настройках
#define DEVICE_ID "esp32-power-01"  // имя устройства в heartbeat и Client ID MQTT по умолчанию
#define TZ_UA "EET-2EEST,M3.5.0/3,M10.5.0/4"  // часовой пояс (POSIX): Украина, автопереход на летнее время


// =============================================================================
// [3] СТРУКТУРА НАСТРОЕК Config
// Все настройки, которые меняются на странице «Настройки». Рабочая копия — глобальная cfg;
// в потоках используются копии (configSnapshot) или кэш lc(). Ключи во флеш — в [7].
//   dev1_*/dev2_*      — контрольные устройства: включено ли и их IP-адреса.
//   partial_is_on      — если отвечает только одно из двух: true = считать, что питание есть.
//   check_interval     — период проверки, с.   ping_count / ping_timeout — число попыток и
//   таймаут пинга, мс.
//   confirm_off/on_cycles — сколько одинаковых проверок подряд нужно, чтобы сменить состояние.
//   server1/2_enabled, oracle_* / backup_* — HTTP-серверы heartbeat (URL и токен «Bearer»).
//   heartbeat_interval — как часто слать heartbeat, с (смена состояния отправляется сразу).
//   admin_pass         — пароль веб-интерфейса.
//   mqtt_*, ha_discovery — параметры MQTT и автосоздание устройства в Home Assistant.
//   stats_enabled      — вести ли статистику отключений.
// =============================================================================
struct Config {
  bool dev1_enabled, dev2_enabled, partial_is_on, server1_enabled, server2_enabled, mqtt_enabled, ha_discovery, stats_enabled;
  char dev1_ip[64], dev2_ip[64];
  int check_interval, ping_count, ping_timeout, confirm_off_cycles, confirm_on_cycles, heartbeat_interval;
  char oracle_url[128], oracle_token[128], backup_url[128], backup_token[128], admin_pass[64];
  char mqtt_host[96], mqtt_user[64], mqtt_pass[96], mqtt_client_id[64], mqtt_base_topic[96];
  int mqtt_port;
  int mqtt_interval;
} cfg;

// =============================================================================
// [4] ГЛОБАЛЬНЫЕ ОБЪЕКТЫ И ОБЩЕЕ СОСТОЯНИЕ
// prefs — доступ к флеш; server — веб-сервер; mx — мьютекс, защищает cfg от одновременной
// записи/чтения из двух потоков.
// g_state — текущее подтверждённое состояние питания: ST_UNKNOWN (ещё не определено),
// ST_ON (есть), ST_OFF (нет). Пишет только monitorTask, остальные читают.
// d1_on/d2_on, d1_lat/d2_lat — результат последних пингов (в сети ли, задержка, мс).
// g_srv_* — состояние отправки на HTTP-сервер; g_check_ms — момент начала последней проверки
// (нужен странице для отсчёта «Следующая проверка»); g_state_since — когда состояние сменилось.
// cfg_ver — номер версии настроек: растёт при каждом сохранении, по нему lc() обновляет кэш.
// restart_at — «отложенная перезагрузка»: loop() перезагрузит плату, когда наступит это время.
// volatile = переменную меняет один поток, а читает другой — компилятор не должен «кэшировать» её.
// =============================================================================
Preferences prefs;
WebServer server(80);
SemaphoreHandle_t mx;
enum : uint8_t { ST_UNKNOWN,
                 ST_ON,
                 ST_OFF };
volatile uint8_t g_state = ST_UNKNOWN;
volatile bool d1_on, d2_on, g_srv_ok, g_srv_seen, gotIp;
volatile int d1_lat = -1, d2_lat = -1, g_srv_code;
volatile uint32_t cfg_ver = 1;
volatile uint32_t g_check_ms, g_state_since, g_last_outage, g_srv_ok_ms, g_srv_lat, restart_at;
bool ota_ok = false, svcUp = false;
volatile bool g_eco_mode = false;

// Состояние MQTT-клиента: подключён ли, код последней ошибки, время публикаций, счётчики для
// страницы.
// ---------- MQTT runtime ----------
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
volatile bool g_mqtt_ok = false, g_mqtt_seen = false;
volatile int g_mqtt_state = -99;
volatile uint32_t g_mqtt_ok_ms = 0, g_mqtt_pub_ms = 0;
uint32_t mqtt_last_try = 0, mqtt_last_pub = 0;
uint8_t mqtt_last_power = 0xFF;
volatile uint32_t g_mqtt_tx_count = 0, g_mqtt_reconnects = 0;
char g_mqtt_last_reason[20] = "—";

static bool validEpoch(time_t t);

// =============================================================================
// [5] ДИАГНОСТИЧЕСКИЙ ЛОГ (Serial, 115200)
// dbgLog(метка, формат, ...) — печатает строку «[время] [МЕТКА] текст». Метки: BOOT, WiFi, POWER,
// POWER-CHECK, ECO, HB (heartbeat), MQTT, MQTT-TX, HEALTH, TIME. Токены и пароли в лог не пишутся.
// Выключается значением DEBUG_DIAGNOSTICS = 0 в [2].
// =============================================================================
// ---------- RC4 light diagnostics ----------
static bool g_time_log_announced = false;
static uint32_t g_wifi_disconnects = 0;
static uint32_t g_hb_ok_count = 0, g_hb_fail_count = 0;

// Печать строки лога с меткой. Время — реальное (после синхронизации NTP) или «аптайм» с момента
// запуска.
static void dbgLog(const char* tag, const char* fmt, ...) {
#if DEBUG_DIAGNOSTICS
  char ts[32];
  time_t now = time(nullptr);
  if (validEpoch(now)) {
    struct tm lt; localtime_r(&now, &lt);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &lt);
  } else {
    uint32_t ms = millis();
    snprintf(ts, sizeof(ts), "+%02lu:%02lu:%02lu.%03lu",
             (unsigned long)(ms / 3600000UL), (unsigned long)((ms / 60000UL) % 60),
             (unsigned long)((ms / 1000UL) % 60), (unsigned long)(ms % 1000UL));
  }
  Serial.printf("[%s] [%s] ", ts, tag);
  va_list ap; va_start(ap, fmt); Serial.vprintf(fmt, ap); va_end(ap);
  Serial.println();
#else
  (void)tag; (void)fmt;
#endif
}

// =============================================================================
// [6] СТАТИСТИКА ОТКЛЮЧЕНИЙ
// Хранится во флеш в пространстве «pm-stats» (ключи: outages, last_off, last_on, last_dur,
// total_dur, max_dur). Пишется ТОЛЬКО при подтверждённой смене ON<->OFF, чтобы не изнашивать флеш.
// Длительность отключения считается, только если и начало, и конец отключения были замечены
// за один и тот же запуск платы (g_stats_off_open) и время уже синхронизировано по NTP.
// =============================================================================
// ---------- Power statistics (persistent, written only on confirmed transitions) ----------
uint32_t g_outage_count = 0;
uint32_t g_last_outage_duration = 0;
uint64_t g_total_outage_duration = 0;
uint32_t g_max_outage_duration = 0;
time_t g_last_power_off = 0;
time_t g_last_power_on = 0;
bool g_stats_off_open = false;  // RAM only: valid only in the current boot session

// Проверка, что время уже синхронизировано (после 2023 года), иначе «время» — это просто аптайм.
static bool validEpoch(time_t t) { return t > 1700000000; }
// Преобразует время в строку ISO 8601 с часовым поясом; пустая строка, если время неизвестно.
static void epochIso(time_t t, char* out, size_t n) {
  if (!validEpoch(t)) { out[0] = 0; return; }
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(out, n, "%Y-%m-%dT%H:%M:%S%z", &lt);
}
// Читает накопленную статистику из флеш при запуске (вызывается из setup()).
static void loadPowerStats() {
  Preferences st;
  st.begin("pm-stats", true);
  g_outage_count = st.getUInt("outages", 0);
  g_last_outage_duration = st.getUInt("last_dur", 0);
  g_total_outage_duration = st.getULong64("total_dur", 0);
  g_max_outage_duration = st.getUInt("max_dur", 0);
  g_stats_off_open = false;
  g_last_power_off = (time_t)st.getULong64("last_off", 0);
  g_last_power_on = (time_t)st.getULong64("last_on", 0);
  st.end();
}
// Записывает подтверждённую смену состояния. wentOff = true: питание пропало (счётчик +1,
// запоминаем время). false: питание вернулось (считаем длительность, обновляем сумму и максимум).
// Если сбор статистики выключен — ничего не пишет.
static void savePowerTransition(bool wentOff, time_t nowEpoch, bool statsEnabled) {
  if (!statsEnabled) { g_stats_off_open = false; return; }

  Preferences st;
  st.begin("pm-stats", false);
  if (wentOff) {
    ++g_outage_count;
    st.putUInt("outages", g_outage_count);
    g_stats_off_open = validEpoch(nowEpoch);   // current session only
    if (validEpoch(nowEpoch)) {
      g_last_power_off = nowEpoch;
      st.putULong64("last_off", (uint64_t)nowEpoch);
    }
  } else {
    if (validEpoch(nowEpoch)) {
      g_last_power_on = nowEpoch;
      st.putULong64("last_on", (uint64_t)nowEpoch);
    }
    // Duration is accepted only if OFF was observed in this same boot session.
    if (g_stats_off_open && validEpoch(g_last_power_off) && validEpoch(nowEpoch) && nowEpoch >= g_last_power_off) {
      uint32_t dur = (uint32_t)(nowEpoch - g_last_power_off);
      g_last_outage_duration = dur;
      g_total_outage_duration += dur;
      if (dur > g_max_outage_duration) g_max_outage_duration = dur;
      st.putUInt("last_dur", g_last_outage_duration);
      st.putULong64("total_dur", g_total_outage_duration);
      st.putUInt("max_dur", g_max_outage_duration);
    }
    g_stats_off_open = false;
  }
  st.end();
}
// Сколько секунд длится текущее состояние (для страницы и MQTT). 0, пока состояние не определено.
static uint32_t currentPowerAge() {
  if (g_state == ST_UNKNOWN) return 0;
  return (millis() - g_state_since) / 1000UL;
}


// =============================================================================
// [7] ХРАНЕНИЕ НАСТРОЕК (NVS, пространство «pm-config»)
// loadConfig() читает настройки при запуске, saveConfig() записывает после нажатия «Сохранить».
// validateConfig() приводит значения к допустимым диапазонам (защита от неверного ввода).
// configSnapshot() — безопасная копия cfg для другого потока; lc() — то же, но с кэшем
// (для loop(), где вызывается очень часто). ipStr() — IP платы строкой.
// Ключи во флеш короткие (до 15 символов), например d1_ip, chk_int, mq_host.
// =============================================================================
// ---------- конфиг ----------
// Читает строковую настройку по ключу k (значение по умолчанию d) в буфер o размером n.
static void gs(const char* k, const char* d, char* o, size_t n) {
  strlcpy(o, prefs.getString(k, d).c_str(), n);
}
// Возвращает копию настроек под мьютексом: вызывать из monitorTask и обработчиков, которым нужна
// гарантированно целая копия.
static Config configSnapshot() {
  Config c{};
  if (mx && xSemaphoreTake(mx, portMAX_DELAY) == pdTRUE) {
    c = cfg;
    xSemaphoreGive(mx);
  }
  return c;
}
// Кэш конфига для loop-задачи (MQTT): копия только при смене cfg_ver, а не 500 раз в секунду.
// Быстрый доступ к настройкам из loop()-потока: копия делается только когда cfg_ver изменился.
// Ссылку не хранить надолго.
static const Config& lc() {
  static Config c;
  static uint32_t v = 0;
  if (v != cfg_ver) {
    c = configSnapshot();
    v = cfg_ver;
  }
  return c;
}
// Записывает IP платы в o (буфер не меньше 16 байт).
static void ipStr(char* o) {
  IPAddress a = WiFi.localIP();
  snprintf(o, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}
// Ограничивает значения: интервалы, число попыток, порт и т. д. Допустимые значения mqtt_interval —
// только из списка (30 с … 24 ч).
static void validateConfig(Config& c) {
  c.check_interval = constrain(c.check_interval, 2, 3600);
  c.ping_count = constrain(c.ping_count, 1, 5);
  c.ping_timeout = constrain(c.ping_timeout, 100, 3000);
  c.confirm_off_cycles = constrain(c.confirm_off_cycles, 1, 20);
  c.confirm_on_cycles = constrain(c.confirm_on_cycles, 1, 20);
  c.heartbeat_interval = constrain(c.heartbeat_interval, 10, 86400);
  c.mqtt_port = constrain(c.mqtt_port, 1, 65535);
  switch (c.mqtt_interval) {
    case 30: case 300: case 1800: case 3600:
    case 10800: case 21600: case 43200: case 86400:
      break;
    default:
      c.mqtt_interval = 30;
  }
}
// Загрузка настроек из флеш. Второй аргумент getInt/getBool/gs — значение по умолчанию (для новой
// платы).
void loadConfig() {
  prefs.begin("pm-config", false);
  cfg.dev1_enabled = prefs.getBool("d1_en", true);
  gs("d1_ip", "192.168.1.1", cfg.dev1_ip, 64);
  cfg.dev2_enabled = prefs.getBool("d2_en", true);
  gs("d2_ip", "192.168.1.50", cfg.dev2_ip, 64);
  cfg.check_interval = prefs.getInt("chk_int", 10);
  cfg.ping_count = prefs.getInt("p_cnt", 2);
  cfg.ping_timeout = prefs.getInt("p_to", 800);
  cfg.confirm_off_cycles = prefs.getInt("c_off", 3);
  cfg.confirm_on_cycles = prefs.getInt("c_on", 2);
  cfg.partial_is_on = prefs.getBool("part_on", true);
  // Migration from v3.0.0/v3.0.1: old primary server was always enabled.
  cfg.server1_enabled = prefs.isKey("s1_en") ? prefs.getBool("s1_en", true) : true;
  cfg.server2_enabled = prefs.isKey("s2_en") ? prefs.getBool("s2_en", false) : prefs.getBool("b_en", false);
  gs("s_url", "https://server_test.com/api/v1/heartbeat", cfg.oracle_url, 128);
  gs("s_tok", "", cfg.oracle_token, 128);
  gs("b_url", "", cfg.backup_url, 128);
  gs("b_tok", "", cfg.backup_token, 128);
  cfg.heartbeat_interval = prefs.getInt("hb_int", 60);
  gs("adm_pass", DEFAULT_ADMIN_PASS, cfg.admin_pass, 64);
  cfg.mqtt_enabled = prefs.getBool("mq_en", false);
  cfg.ha_discovery = prefs.getBool("ha_disc", true);
  cfg.stats_enabled = prefs.getBool("stats_en", true);
  gs("mq_host", "", cfg.mqtt_host, sizeof cfg.mqtt_host);
  cfg.mqtt_port = prefs.getInt("mq_port", 1883);
  cfg.mqtt_interval = prefs.getInt("mq_int", 30);
  gs("mq_user", "", cfg.mqtt_user, sizeof cfg.mqtt_user);
  gs("mq_pass", "", cfg.mqtt_pass, sizeof cfg.mqtt_pass);
  gs("mq_cid", DEVICE_ID, cfg.mqtt_client_id, sizeof cfg.mqtt_client_id);
  gs("mq_base", "power-monitor", cfg.mqtt_base_topic, sizeof cfg.mqtt_base_topic);
  prefs.end();
  validateConfig(cfg);
}
// Запись всех настроек во флеш. Ключи здесь и в loadConfig() должны совпадать.
void saveConfig(const Config& c) {
  prefs.begin("pm-config", false);
  prefs.putBool("d1_en", c.dev1_enabled);
  prefs.putString("d1_ip", c.dev1_ip);
  prefs.putBool("d2_en", c.dev2_enabled);
  prefs.putString("d2_ip", c.dev2_ip);
  prefs.putInt("chk_int", c.check_interval);
  prefs.putInt("p_cnt", c.ping_count);
  prefs.putInt("p_to", c.ping_timeout);
  prefs.putInt("c_off", c.confirm_off_cycles);
  prefs.putInt("c_on", c.confirm_on_cycles);
  prefs.putBool("part_on", c.partial_is_on);
  prefs.putBool("s1_en", c.server1_enabled);
  prefs.putBool("s2_en", c.server2_enabled);
  prefs.putString("s_url", c.oracle_url);
  prefs.putString("s_tok", c.oracle_token);
  prefs.putString("b_url", c.backup_url);
  prefs.putString("b_tok", c.backup_token);
  prefs.putInt("hb_int", c.heartbeat_interval);
  prefs.putString("adm_pass", c.admin_pass);
  prefs.putBool("mq_en", c.mqtt_enabled);
  prefs.putBool("ha_disc", c.ha_discovery);
  prefs.putBool("stats_en", c.stats_enabled);
  prefs.putString("mq_host", c.mqtt_host);
  prefs.putInt("mq_port", c.mqtt_port);
  prefs.putInt("mq_int", c.mqtt_interval);
  prefs.putString("mq_user", c.mqtt_user);
  prefs.putString("mq_pass", c.mqtt_pass);
  prefs.putString("mq_cid", c.mqtt_client_id);
  prefs.putString("mq_base", c.mqtt_base_topic);
  prefs.end();
}


// =============================================================================
// [8] ICMP-PING
// Ping делается «сырым» ICMP-сокетом. Эхо-ответ принимается только от нужного IP и только
// со своим идентификатором и номером (посторонние пакеты игнорируются).
// =============================================================================
// ---------- ICMP ping ----------
// Контрольная сумма ICMP-пакета.
static uint16_t csum(const uint8_t* d, int n) {
  uint32_t s = 0;
  for (int i = 0; i + 1 < n; i += 2) s += (d[i] << 8) | d[i + 1];
  while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
  return ~s;
}
static uint16_t ping_seq = 0;
// Один ping. Возвращает задержку в мс или -1, если за таймаут to (мс) ответа не было.
static int pingOnce(IPAddress ip, int to) {
  int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (s < 0) return -1;
  struct timeval tv = { to / 1000, (to % 1000) * 1000 };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  uint8_t p[40] = { 8, 0, 0, 0, 0x50, 0x4D, 0, 0 };
  uint16_t sq = ++ping_seq;
  p[6] = sq >> 8;
  p[7] = sq & 0xFF;
  memset(p + 8, 0x55, 32);
  uint16_t c = csum(p, 40);
  p[2] = c >> 8;
  p[3] = c & 0xFF;
  struct sockaddr_in d = {};
  d.sin_family = AF_INET;
  d.sin_addr.s_addr = (uint32_t)ip;
  uint32_t t0 = millis();
  int res = -1;
  if (sendto(s, p, 40, 0, (struct sockaddr*)&d, sizeof d) >= 0) {
    uint8_t r[128];
    while ((uint32_t)(millis() - t0) < (uint32_t)to) {
      struct sockaddr_in f;
      socklen_t fl = sizeof f;
      int n = recvfrom(s, r, sizeof r, 0, (struct sockaddr*)&f, &fl);
      if (n <= 0) break;
      int ih = (r[0] & 0x0F) * 4;
      if (n < ih + 8) continue;
      if (f.sin_addr.s_addr == d.sin_addr.s_addr && r[ih] == 0 && r[ih + 4] == 0x50 && r[ih + 5] == 0x4D && r[ih + 6] == (sq >> 8) && r[ih + 7] == (sq & 0xFF)) {
        res = millis() - t0;
        break;
      }
    }
  }
  close(s);
  return res;
}
// До n попыток; успех — при первом же ответе (возвращает его задержку), иначе -1.
static int pingHost(IPAddress ip, int n, int to) {
  for (int i = 0; i < n; i++) {
    int l = pingOnce(ip, to);
    if (l >= 0) return l;
  }
  return -1;
}


// =============================================================================
// [9] HEARTBEAT НА HTTP-СЕРВЕРЫ
// Отправка JSON методом POST (HTTP или HTTPS) на сервер 1 и/или 2; при необходимости с заголовком
// Authorization: Bearer <токен>. Для HTTPS проверка сертификата отключена (setInsecure) —
// для строгой проверки используйте setCACert. Сервер 2 используется как запасной: если сервер 1
// не ответил кодом 2xx, пробуется сервер 2.
// =============================================================================
// ---------- сервер ----------
// Один POST-запрос. Таймауты: соединение 3 с, ответ 4 с. true — получен код 2xx; code возвращает
// HTTP-код (0 — не удалось начать).
static bool httpPost(const char* url, const char* tok, const char* body, int& code) {
  HTTPClient h;
  WiFiClient pc;
  WiFiClientSecure sc;
  bool ok;
  if (!strncmp(url, "https://", 8)) {
    sc.setInsecure();
    ok = h.begin(sc, url);
  } else ok = h.begin(pc, url);
  if (!ok) {
    code = 0;
    return false;
  }
  h.setConnectTimeout(3000);
  h.setTimeout(4000);
  h.setReuse(false);
  h.addHeader("Content-Type", "application/json");
  if (tok[0]) {
    String a("Bearer ");
    a += tok;
    h.addHeader("Authorization", a);
  }
  code = h.POST((uint8_t*)body, strlen(body));
  h.end();
  return code >= 200 && code < 300;
}
// Формирует JSON и отправляет на включённые серверы. ev = true — это немедленная отправка из-за
// смены состояния (поле "event":true). Для добавления поля в JSON меняйте строку snprintf ниже
// и размер буфера b[460] (худший случай сейчас ~320 байт).
static bool sendHeartbeat(const Config& c, bool ev) {
  char b[460], ipb[16];
  ipStr(ipb);
  snprintf(b, sizeof b, "{\"device_id\":\"" DEVICE_ID "\",\"power\":%s,\"state\":\"%s\",\"event\":%s,\"state_age\":%lu,\"last_outage_s\":%lu,"
                        "\"device1_online\":%s,\"device1_latency\":%d,\"device2_online\":%s,\"device2_latency\":%d,\"wifi_rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"free_heap\":%u,\"firmware\":\"" FW_VERSION "\"}",
           g_state == ST_ON ? "true" : "false", g_state == ST_ON ? "POWER_ON" : "POWER_OFF", ev ? "true" : "false", (unsigned long)((millis() - g_state_since) / 1000), (unsigned long)g_last_outage,
           d1_on ? "true" : "false", (int)d1_lat, d2_on ? "true" : "false", (int)d2_lat, WiFi.RSSI(), ipb, (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap());
  // RC4.1 diagnostics: exact heartbeat JSON sent to the server.
  // Authorization tokens are HTTP headers and are intentionally never logged.
  dbgLog("HB-TX", "event=%s payload=%s", ev ? "yes" : "no", b);

  if (!c.server1_enabled && !c.server2_enabled) {
    g_srv_ok = false;
    g_srv_code = 0;
    return false;
  }
  const char* urls[2] = { c.server1_enabled ? c.oracle_url : "", c.server2_enabled ? c.backup_url : "" };
  const char* toks[2] = { c.oracle_token, c.backup_token };
  for (int i = 0; i < 2; i++) {
    if (!urls[i][0]) continue;
    uint32_t t = millis();
    int code = 0;
    bool ok = httpPost(urls[i], toks[i], b, code);
    uint32_t elapsed = millis() - t;
    g_srv_code = code;
    if (ok) {
      ++g_hb_ok_count;
      g_srv_ok = true;
      g_srv_lat = elapsed;
      g_srv_ok_ms = millis();
      g_srv_seen = true;
      dbgLog("HB", "S%d OK code=%d time=%lums event=%s", i + 1, code, (unsigned long)elapsed, ev ? "yes" : "no");
      return true;
    }
    ++g_hb_fail_count;
    dbgLog("HB", "S%d FAIL code=%d time=%lums%s", i + 1, code, (unsigned long)elapsed, i == 0 && urls[1][0] ? " fallback=S2" : "");
  }
  g_srv_ok = false;
  return false;
}


// =============================================================================
// [10] MQTT И HOME ASSISTANT
// Топики (base = «Base topic» из настроек, по умолчанию power-monitor):
//   <base>/state         — состояние в JSON, публикуется с retain;
//   <base>/availability  — "online"/"offline" (offline выставляет брокер сам по LWT при обрыве);
//   homeassistant/device/esp32c3_power_monitor/config — автоописание устройства для Home Assistant.
// Когда публикуется состояние: при смене питания (reason=power_changed — приоритет), по таймеру
// mqtt_interval (periodic), после подключения (reconnect). При старте Home Assistant (топик
// homeassistant/status = online) описание и состояние отправляются повторно. Подключение
// повторяется каждые 10 с. Всё выполняется из loop() через mqttService().
// =============================================================================
// ---------- MQTT ----------
// Код состояния PubSubClient -> английский текст для страницы (переводится в I18N_JS, [13]).
static const char* mqttStateText(int st) {
  switch (st) {
    case MQTT_CONNECTION_TIMEOUT: return "Connection timeout";
    case MQTT_CONNECTION_LOST: return "Connection lost";
    case MQTT_CONNECT_FAILED: return "Connect failed";
    case MQTT_DISCONNECTED: return "Disconnected";
    case MQTT_CONNECTED: return "Connected";
    case MQTT_CONNECT_BAD_PROTOCOL: return "Bad protocol";
    case MQTT_CONNECT_BAD_CLIENT_ID: return "Bad client ID";
    case MQTT_CONNECT_UNAVAILABLE: return "Broker unavailable";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "Bad credentials";
    case MQTT_CONNECT_UNAUTHORIZED: return "Unauthorized";
    default: return "Unknown";
  }
}
// Склеивает топик: «base/suffix».
static void mqttTopic(char* out, size_t n, const char* base, const char* suffix) {
  snprintf(out, n, "%s/%s", base, suffix);
}
// Текущее местное время строкой ISO (пусто, если время ещё не синхронизировано).
static void mqttTimestamp(char* out, size_t n) {
  struct tm t;
  if (!getLocalTime(&t, 0)) { out[0] = 0; return; }
  strftime(out, n, "%Y-%m-%dT%H:%M:%S%z", &t);
}
// Публикует JSON состояния (retain). reason — причина отправки: power_changed / periodic /
// reconnect. Формат — snprintf ниже, буфер body[900].
static bool mqttPublishState(const Config& c, const char* reason) {
  if (!mqtt.connected()) return false;
  char topic[128];
  mqttTopic(topic, sizeof topic, c.mqtt_base_topic, "state");
  char body[900], ts[40], lastOff[40], lastOn[40];
  mqttTimestamp(ts, sizeof ts);
  epochIso(g_last_power_off, lastOff, sizeof lastOff);
  epochIso(g_last_power_on, lastOn, sizeof lastOn);
  char ip[16];
  ipStr(ip);
  uint32_t powerAge = currentPowerAge();
  snprintf(body, sizeof body,
           "{\"power\":\"%s\",\"state\":%u,\"power_age\":%lu,\"stats_enabled\":%s,\"last_power_off\":\"%s\",\"last_power_on\":\"%s\",\"last_outage_duration\":%lu,\"outage_count\":%lu,\"total_outage_duration\":%llu,\"max_outage_duration\":%lu,\"reason\":\"%s\",\"timestamp\":\"%s\",\"device1\":\"%s\",\"device1_ping\":%d,\"device2\":\"%s\",\"device2_ping\":%d,\"wifi_rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"free_heap\":%u,\"firmware\":\"%s\"}",
           g_state == ST_ON ? "ON" : g_state == ST_OFF ? "OFF" : "UNKNOWN", (unsigned)g_state,
           (unsigned long)powerAge, c.stats_enabled ? "true" : "false", lastOff, lastOn,
           (unsigned long)g_last_outage_duration, (unsigned long)g_outage_count,
           (unsigned long long)g_total_outage_duration, (unsigned long)g_max_outage_duration,
           reason, ts, d1_on ? "ON" : "OFF", (int)d1_lat, d2_on ? "ON" : "OFF", (int)d2_lat,
           WiFi.RSSI(), ip, (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), FW_VERSION);
  // RC4.1 diagnostics: exact retained state message consumed by Home Assistant.
  dbgLog("MQTT-TX", "reason=%s retain=1 topic=%s payload=%s", reason, topic, body);
  bool ok = mqtt.publish(topic, body, true);
  dbgLog("MQTT-TX", "result=%s reason=%s bytes=%u", ok ? "OK" : "FAIL", reason, (unsigned)strlen(body));
  if (ok) {
    g_mqtt_pub_ms = millis();
    mqtt_last_pub = millis();
    mqtt_last_power = g_state;
    g_mqtt_tx_count++;
    strlcpy(g_mqtt_last_reason, reason, sizeof g_mqtt_last_reason);
    dbgLog("MQTT", "PUBLISH reason=%s power=%s", reason, g_state == ST_ON ? "ON" : g_state == ST_OFF ? "OFF" : "UNKNOWN");
  }
  return ok;
}
// Публикует единое описание устройства и его сущностей для Home Assistant (MQTT Device Discovery).
// Каждая строка body += ... — одна сущность: имя, unique_id, класс, единицы и шаблон значения из
// <base>/state.
// Чтобы добавить сущность, добавьте строку по образцу и поле в mqttPublishState(). Размер:
// body.reserve(4600) и буфер MQTT 6144.
static bool mqttPublishDiscovery(const Config& c) {
  if (!mqtt.connected() || !c.ha_discovery) return false;

  char topic[160], stateTopic[128], availTopic[128];
  mqttTopic(stateTopic, sizeof stateTopic, c.mqtt_base_topic, "state");
  mqttTopic(availTopic, sizeof availTopic, c.mqtt_base_topic, "availability");
  snprintf(topic, sizeof topic, "homeassistant/device/esp32c3_power_monitor/config");

  // Device Discovery: one retained configuration message for all entities.
  // ESP32-C3 is explicitly present in device metadata and every unique_id.
  String body;
  body.reserve(4600);
  body += F("{\"dev\":{\"ids\":[\"esp32c3_power_monitor\"],\"name\":\"ESP32-C3 Power Monitor\",\"mf\":\"Espressif\",\"mdl\":\"ESP32-C3 Power Monitor\",\"hw\":\"ESP32-C3\",\"sw\":\"");
  body += FW_VERSION;
  body += F("\"},\"o\":{\"name\":\"ESP32-C3 Power Monitor\",\"sw\":\"");
  body += FW_VERSION;
  body += F("\"},\"state_topic\":\""); body += stateTopic;
  body += F("\",\"availability_topic\":\""); body += availTopic;
  body += F("\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\",\"cmps\":{");

  body += F("\"power\":{\"p\":\"binary_sensor\",\"name\":\"Электричество\",\"unique_id\":\"esp32c3_power_monitor_power\",\"device_class\":\"power\",\"value_template\":\"{{ value_json.power }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"},");
  body += F("\"device1\":{\"p\":\"binary_sensor\",\"name\":\"Устройство 1\",\"unique_id\":\"esp32c3_power_monitor_device1\",\"device_class\":\"connectivity\",\"value_template\":\"{{ value_json.device1 }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"},");
  body += F("\"device2\":{\"p\":\"binary_sensor\",\"name\":\"Устройство 2\",\"unique_id\":\"esp32c3_power_monitor_device2\",\"device_class\":\"connectivity\",\"value_template\":\"{{ value_json.device2 }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"},");
  body += F("\"ping1\":{\"p\":\"sensor\",\"name\":\"Ping устройства 1\",\"unique_id\":\"esp32c3_power_monitor_ping1\",\"unit_of_measurement\":\"ms\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ value_json.device1_ping }}\"},");
  body += F("\"ping2\":{\"p\":\"sensor\",\"name\":\"Ping устройства 2\",\"unique_id\":\"esp32c3_power_monitor_ping2\",\"unit_of_measurement\":\"ms\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ value_json.device2_ping }}\"},");
  body += F("\"rssi\":{\"p\":\"sensor\",\"name\":\"Wi-Fi RSSI\",\"unique_id\":\"esp32c3_power_monitor_wifi_rssi\",\"device_class\":\"signal_strength\",\"unit_of_measurement\":\"dBm\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ value_json.wifi_rssi }}\"},");
  body += F("\"uptime\":{\"p\":\"sensor\",\"name\":\"Uptime\",\"unique_id\":\"esp32c3_power_monitor_uptime\",\"icon\":\"mdi:timer-outline\",\"entity_category\":\"diagnostic\",\"value_template\":\"{% set s=value_json.uptime|int %}{% set d=s//86400 %}{% set h=(s%86400)//3600 %}{% set m=(s%3600)//60 %}{{ (d|string + ' д ' if d else '') + '%02d:%02d'|format(h,m) }}\"},");
  body += F("\"heap\":{\"p\":\"sensor\",\"name\":\"Free Heap\",\"unique_id\":\"esp32c3_power_monitor_free_heap\",\"device_class\":\"data_size\",\"unit_of_measurement\":\"B\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ value_json.free_heap }}\"},");
  body += F("\"power_age\":{\"p\":\"sensor\",\"name\":\"Длительность текущего состояния\",\"unique_id\":\"esp32c3_power_monitor_power_age\",\"icon\":\"mdi:timer-sand\",\"value_template\":\"{% set s=value_json.power_age|int %}{% set d=s//86400 %}{% set h=(s%86400)//3600 %}{% set m=(s%3600)//60 %}{{ (d|string + ' д ' if d else '') + '%02d:%02d'|format(h,m) }}\"},");
  body += F("\"last_off\":{\"p\":\"sensor\",\"name\":\"Крайнее отключение\",\"unique_id\":\"esp32c3_power_monitor_last_power_off\",\"device_class\":\"timestamp\",\"value_template\":\"{{ value_json.last_power_off if value_json.last_power_off else none }}\"},");
  body += F("\"last_on\":{\"p\":\"sensor\",\"name\":\"Крайнее восстановление\",\"unique_id\":\"esp32c3_power_monitor_last_power_on\",\"device_class\":\"timestamp\",\"value_template\":\"{{ value_json.last_power_on if value_json.last_power_on else none }}\"},");
  body += F("\"outages\":{\"p\":\"sensor\",\"name\":\"Счётчик отключений\",\"unique_id\":\"esp32c3_power_monitor_outage_count\",\"state_class\":\"total_increasing\",\"icon\":\"mdi:counter\",\"value_template\":\"{{ value_json.outage_count }}\"},");
  body += F("\"last_dur\":{\"p\":\"sensor\",\"name\":\"Длительность крайнего отключения\",\"unique_id\":\"esp32c3_power_monitor_last_outage_duration\",\"icon\":\"mdi:timer-outline\",\"entity_category\":\"diagnostic\",\"value_template\":\"{% set s=value_json.last_outage_duration|int %}{% set d=s//86400 %}{% set h=(s%86400)//3600 %}{% set m=(s%3600)//60 %}{{ (d|string + ' д ' if d else '') + '%02d:%02d'|format(h,m) }}\"},");
  body += F("\"total_dur\":{\"p\":\"sensor\",\"name\":\"Общее время без питания\",\"unique_id\":\"esp32c3_power_monitor_total_outage_duration\",\"icon\":\"mdi:timer-outline\",\"entity_category\":\"diagnostic\",\"value_template\":\"{% set s=value_json.total_outage_duration|int %}{% set d=s//86400 %}{% set h=(s%86400)//3600 %}{% set m=(s%3600)//60 %}{{ (d|string + ' д ' if d else '') + '%02d:%02d'|format(h,m) }}\"},");
  body += F("\"max_dur\":{\"p\":\"sensor\",\"name\":\"Максимальное отключение\",\"unique_id\":\"esp32c3_power_monitor_max_outage_duration\",\"icon\":\"mdi:timer-alert-outline\",\"entity_category\":\"diagnostic\",\"value_template\":\"{% set s=value_json.max_outage_duration|int %}{% set d=s//86400 %}{% set h=(s%86400)//3600 %}{% set m=(s%3600)//60 %}{{ (d|string + ' д ' if d else '') + '%02d:%02d'|format(h,m) }}\"},");
  body += F("\"stats_en\":{\"p\":\"binary_sensor\",\"name\":\"Сбор статистики\",\"unique_id\":\"esp32c3_power_monitor_stats_enabled\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ 'ON' if value_json.stats_enabled else 'OFF' }}\",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"},");
  body += F("\"firmware\":{\"p\":\"sensor\",\"name\":\"Firmware\",\"unique_id\":\"esp32c3_power_monitor_firmware\",\"entity_category\":\"diagnostic\",\"value_template\":\"{{ value_json.firmware }}\"}");
  body += F("}}");

  // RC4.1 diagnostics: discovery is MQTT traffic too; log the exact topic/payload once when published.
  dbgLog("MQTT-TX", "reason=ha_discovery retain=1 topic=%s payload=%s", topic, body.c_str());
  bool ok = mqtt.publish(topic, body.c_str(), true);
  dbgLog("MQTT-TX", "result=%s reason=ha_discovery bytes=%u", ok ? "OK" : "FAIL", (unsigned)body.length());
  dbgLog("MQTT", "HA DISCOVERY %s (%u bytes)", ok ? "OK" : "FAIL", (unsigned)body.length());
  return ok;
}

// Входящие сообщения: реагируем только на homeassistant/status = online (Home Assistant
// перезапустился) и повторяем discovery и состояние.
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, "homeassistant/status") != 0) return;
  if (length != 6 || memcmp(payload, "online", 6) != 0) return;

  const Config& c = lc();
  if (c.mqtt_enabled && c.ha_discovery) {
    mqttPublishDiscovery(c);
    mqttPublishState(c, "reconnect");
  }
}

// Подключение к брокеру: логин/пароль (если заданы), LWT availability=offline, подписка на
// homeassistant/status, первая публикация. Возвращает успех.
static bool mqttConnect(const Config& c) {
  if (!c.mqtt_enabled || !c.mqtt_host[0] || WiFi.status() != WL_CONNECTED) return false;
  mqtt.setServer(c.mqtt_host, (uint16_t)c.mqtt_port);
  mqtt.setSocketTimeout(2);
  mqtt.setKeepAlive(30);
  static bool bufSet = false;
  if (!bufSet) {
    mqtt.setBufferSize(6144);
    bufSet = true;
  }
  mqtt.setCallback(mqttCallback);
  char willTopic[128];
  mqttTopic(willTopic, sizeof willTopic, c.mqtt_base_topic, "availability");
  bool ok;
  if (c.mqtt_user[0]) ok = mqtt.connect(c.mqtt_client_id, c.mqtt_user, c.mqtt_pass, willTopic, 0, true, "offline");
  else ok = mqtt.connect(c.mqtt_client_id, willTopic, 0, true, "offline");
  g_mqtt_state = mqtt.state();
  g_mqtt_ok = ok;
  if (ok) {
    g_mqtt_seen = true;
    g_mqtt_ok_ms = millis();
    g_mqtt_reconnects++;
    dbgLog("MQTT-TX", "reason=availability retain=1 topic=%s payload=online", willTopic);
    bool availOk = mqtt.publish(willTopic, "online", true);
    dbgLog("MQTT-TX", "result=%s reason=availability bytes=6", availOk ? "OK" : "FAIL");
    if (c.ha_discovery) {
      mqtt.subscribe("homeassistant/status");
      mqttPublishDiscovery(c);
    }
    mqttPublishState(c, "reconnect");
    dbgLog("MQTT", "CONNECTED %s:%d reconnects=%lu", c.mqtt_host, c.mqtt_port, (unsigned long)g_mqtt_reconnects);
  } else {
    dbgLog("MQTT", "CONNECT FAILED state=%d (%s)", g_mqtt_state, mqttStateText(g_mqtt_state));
  }
  return ok;
}
// Вызывается из loop() постоянно: держит соединение, обслуживает mqtt.loop(), публикует состояние
// при смене питания и по таймеру.
static void mqttService() {
  const Config& c = lc();
  if (!c.mqtt_enabled) {
    if (mqtt.connected()) mqtt.disconnect();
    g_mqtt_ok = false;
    g_mqtt_state = MQTT_DISCONNECTED;
    mqtt_last_power = 0xFF;
    return;
  }
  if (WiFi.status() != WL_CONNECTED || !c.mqtt_host[0]) {
    g_mqtt_ok = false;
    return;
  }
  if (!mqtt.connected()) {
    g_mqtt_ok = false;
    if ((uint32_t)(millis() - mqtt_last_try) >= 10000UL) {
      mqtt_last_try = millis();
      mqttConnect(c);
    }
    return;
  }
  mqtt.loop();
  g_mqtt_ok = mqtt.connected();
  g_mqtt_state = mqtt.state();
  if (!g_mqtt_ok) return;

// Приоритет — смена питания: она уходит сразу. Остальные данные (пинг, RSSI, память) ждут
// периодической публикации.
  // POWER is the priority event. Ping/RSSI/heap changes wait for periodic telemetry.
  if (g_state != ST_UNKNOWN && mqtt_last_power != g_state) {
    mqttPublishState(c, "power_changed");
  } else if ((uint32_t)(millis() - mqtt_last_pub) >= (uint32_t)c.mqtt_interval * 1000UL) {
    mqttPublishState(c, "periodic");
  }
}


// =============================================================================
// [11] ОПРЕДЕЛЕНИЕ ПИТАНИЯ — monitorTask (ГЛАВНАЯ ЛОГИКА)
// Цикл раз в check_interval секунд:
//  1) если Wi-Fi нет — измерение пропускается (потеря Wi-Fi не считается пропаданием питания,
//     счётчики подтверждения сбрасываются);
//  2) пингуются включённые устройства; отключённое в настройках считается «на связи»;
//  3) evalCond() даёт вердикт цикла: 1 = есть, 2 = нет, 0 = нечего проверять;
//  4) вердикт копится в счётчиках offc/onc; состояние меняется только когда накоплено
//     confirm_off_cycles (для «нет») или confirm_on_cycles (для «есть») одинаковых вердиктов
//     подряд;
//  5) при смене состояния: пишется статистика, включается/выключается экономный режим,
//     ставится флаг pending — heartbeat уйдёт сразу, а не по таймеру;
//  6) heartbeat отправляется сразу при смене состояния (повторяется каждый цикл, пока не получится)
//     и дальше каждые heartbeat_interval секунд;
//  7) пауза до следующей проверки.
// Экономный режим: пока питание подтверждённо отсутствует, проверки идут раз в
// ECO_CHECK_INTERVAL_SEC (60 с) и включается энергосбережение Wi-Fi. Как только устройство
// снова ответило — возвращается обычный интервал, чтобы быстро подтвердить возвращение питания.
// =============================================================================
// ---------- питание ----------
// Вердикт одного цикла по ответам o1/o2: все включённые ответили -> 1; ни одно -> 2; часть -> по
// настройке partial_is_on; нет включённых -> 0.
static int evalCond(const Config& c, bool o1, bool o2) {
  int tot = c.dev1_enabled + c.dev2_enabled;
  if (!tot) return 0;
  int act = (c.dev1_enabled && o1) + (c.dev2_enabled && o2);
  if (act == tot) return 1;
  if (act == 0) return 2;
  return c.partial_is_on ? 1 : 2;
}
// Текущий интервал проверок: 60 с в режиме «питания нет» (кроме момента возможного восстановления),
// иначе check_interval из настроек.
static uint32_t effectiveCheckIntervalSec(const Config& c, bool recoveryCandidate = false) {
  // RC1: while confirmed OFF, poll slowly. Once an ON candidate appears,
  // return to the normal configured cadence so ON debounce is not delayed by 60 s.
  if (g_state == ST_OFF && !recoveryCandidate) return ECO_CHECK_INTERVAL_SEC;
  return (uint32_t)c.check_interval;
}

// Включает/выключает экономный режим (энергосбережение Wi-Fi).
static void setEcoMode(bool enable) {
  if (g_eco_mode == enable) return;
  g_eco_mode = enable;
  WiFi.setSleep(enable);
  dbgLog("ECO", "%s wifi_sleep=%s interval=%us",
         enable ? "ENTER" : "EXIT",
         enable ? "ON" : "OFF",
         enable ? (unsigned)ECO_CHECK_INTERVAL_SEC : 0U);
}

// Задача проверки питания (запускается в setup(), стек 12 КБ). Подробное описание цикла — в
// заголовке раздела [11].
static void monitorTask(void*) {
  int offc = 0, onc = 0;
  uint32_t lastHb = 0;
  bool pending = false;
  for (;;) {
    // --- Начало цикла. g_check_ms нужен странице для отсчёта «Следующая проверка».
    uint32_t t0 = millis();
    g_check_ms = t0;
    Config c = configSnapshot();

    // POWER state may only be evaluated while STA has a valid IP.
    // Wi-Fi loss means "measurement unavailable", not "power OFF".
    // Drop any partial confirmation chain so evidence never spans a network outage.
    // --- Wi-Fi потерян: проверку пропускаем (это не пропажа питания), только ждём интервал.
    if (!gotIp || WiFi.status() != WL_CONNECTED) {
      if (offc || onc)
        dbgLog("POWER-CHECK", "PAUSED wifi=DOWN reset_confirm off=%d on=%d", offc, onc);
      offc = 0;
      onc = 0;
      uint32_t waitSec = effectiveCheckIntervalSec(c);
      while ((uint32_t)(millis() - t0) < waitSec * 1000UL)
        vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // --- Пинги. Отключённое в настройках устройство считается «на связи», чтобы не влиять на вердикт.
    bool o1 = true, o2 = true;
    int l1 = 0, l2 = 0;
    IPAddress ip;
    if (c.dev1_enabled) {
      l1 = ip.fromString(c.dev1_ip) ? pingHost(ip, c.ping_count, c.ping_timeout) : -1;
      o1 = l1 >= 0;
    }
    if (c.dev2_enabled) {
      l2 = ip.fromString(c.dev2_ip) ? pingHost(ip, c.ping_count, c.ping_timeout) : -1;
      o2 = l2 >= 0;
    }
    d1_on = o1;
    d1_lat = l1;
    d2_on = o2;
    d2_lat = l2;
    if (g_eco_mode)
      dbgLog("ECO", "CHECK d1=%s/%dms d2=%s/%dms", o1 ? "ON" : "OFF", l1, o2 ? "ON" : "OFF", l2);
    // --- Вердикт цикла и накопление подтверждений (offc — «нет» подряд, onc — «есть» подряд).
    int cond = evalCond(c, o1, o2);
    uint8_t ns = g_state;
    if (cond == 2) {
      onc = 0;
      if (offc < c.confirm_off_cycles) {
        ++offc;
        if (g_state != ST_OFF)
          dbgLog("POWER-CHECK", "OFF confirm=%d/%d d1=%s/%dms d2=%s/%dms", offc, c.confirm_off_cycles, o1 ? "ON" : "OFF", l1, o2 ? "ON" : "OFF", l2);
      }
      if (offc >= c.confirm_off_cycles) {
        offc = c.confirm_off_cycles;
        ns = ST_OFF;
      }
    } else if (cond == 1) {
      offc = 0;
      if (onc < c.confirm_on_cycles) {
        ++onc;
        if (g_state != ST_ON)
          dbgLog("POWER-CHECK", "ON confirm=%d/%d d1=%s/%dms d2=%s/%dms", onc, c.confirm_on_cycles, o1 ? "ON" : "OFF", l1, o2 ? "ON" : "OFF", l2);
      }
      if (onc >= c.confirm_on_cycles) {
        onc = c.confirm_on_cycles;
        ns = ST_ON;
      }
    }
    // --- Подтверждённая смена состояния: время, статистика, флаг pending (немедленный heartbeat),
    //    экономный режим.
    if (ns != g_state) {
      uint32_t now = millis();
      uint8_t oldState = g_state;
      if (oldState == ST_OFF && ns == ST_ON) g_last_outage = (now - g_state_since) / 1000;
      // Statistics count only real confirmed ON<->OFF transitions; UNKNOWN at boot is not an outage.
      time_t nowEpoch = time(nullptr);
      if (oldState == ST_ON && ns == ST_OFF) savePowerTransition(true, nowEpoch, c.stats_enabled);
      else if (oldState == ST_OFF && ns == ST_ON) savePowerTransition(false, nowEpoch, c.stats_enabled);
      else if (oldState == ST_UNKNOWN) g_stats_off_open = false; // new boot session baseline
      g_state_since = now;
      g_state = ns;
      pending = true;
      dbgLog("POWER", "%s -> %s d1=%s/%dms d2=%s/%dms", oldState == ST_UNKNOWN ? "UNKNOWN" : oldState == ST_ON ? "ON" : "OFF", ns == ST_ON ? "ON" : "OFF", d1_on ? "ON" : "OFF", (int)d1_lat, d2_on ? "ON" : "OFF", (int)d2_lat);
      setEcoMode(ns == ST_OFF);
    }
    // --- Отправка heartbeat: сразу при смене состояния (pending) или по таймеру heartbeat_interval.
    bool hbEnabled = c.server1_enabled || c.server2_enabled;
    if (!hbEnabled) pending = false;
    else if (g_state != ST_UNKNOWN && WiFi.status() == WL_CONNECTED && (pending || (uint32_t)(millis() - lastHb) >= (uint32_t)c.heartbeat_interval * 1000UL)) {
      lastHb = millis();
      if (sendHeartbeat(c, pending)) pending = false;
    }
    // --- Пауза до следующего цикла (в экономном режиме — длиннее, но при первом ответе устройства
    //    возвращается обычный интервал).
    bool recoveryCandidate = (g_state == ST_OFF && cond == 1);
    uint32_t waitSec = effectiveCheckIntervalSec(c, recoveryCandidate);
    while ((uint32_t)(millis() - t0) < waitSec * 1000UL) vTaskDelay(pdMS_TO_TICKS(200));
  }
}


// =============================================================================
// [12] ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ВЕБ-ЧАСТИ
// Все страницы и API защищены Basic-авторизацией: логин ADMIN_USER, пароль — из настроек.
// jn()/js() собирают JSON вручную (число / строка с экранированием кавычек).
// =============================================================================
// ---------- Web helpers ----------
// true, если браузер прислал верные логин и пароль. Пароль копируется под мьютексом.
static bool authOk() {
  char pass[64];
  if (xSemaphoreTake(mx, portMAX_DELAY) != pdTRUE) return false;
  strlcpy(pass, cfg.admin_pass, sizeof(pass));
  xSemaphoreGive(mx);
  return server.authenticate(ADMIN_USER, pass);
}
// Проверка в начале каждого обработчика: если вход не выполнен — запрашивает логин и пароль и
// возвращает false (обработчик должен сразу завершиться).
static bool checkAuth() {
  if (!authOk()) {
    server.requestAuthentication();
    return false;
  }
  return true;
}
// Местное время «ЧЧ:ММ:СС» или «--:--:--», пока время не синхронизировано.
static void fmtTime(char* b, size_t n) {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    strlcpy(b, "--:--:--", n);
    return;
  }
  strftime(b, n, "%H:%M:%S", &t);
}
// Добавляет в JSON пару "ключ":число, и запятую.
static void jn(String& o, const char* k, long v) {
  o += '"';
  o += k;
  o += "\":";
  o += v;
  o += ',';
}
// Добавляет в JSON пару "ключ":"строка" с экранированием, и запятую.
static void js(String& o, const char* k, const char* v) {
  o += '"';
  o += k;
  o += "\":\"";
  for (; *v; v++) {
    if (*v == '"' || *v == '\\') {
      o += '\\';
      o += *v;
    } else if ((uint8_t)*v >= 0x20) o += *v;
  }
  o += "\",";
}


// =============================================================================
// [13] ОФОРМЛЕНИЕ СТРАНИЦ: CSS, ПЕРЕВОДЫ, ШАПКА
// 13.1 CSS — один общий блок стилей для всех страниц (хранится во флеш, вставляется в каждую
//    страницу).
//   • Начало: переменные цветов. :root — светлая тема; блок prefers-color-scheme:dark и
//     :root[data-theme=dark] — тёмная (одинаковые значения продублированы для режима «Авто» и
//     ручного выбора).
//     Чтобы сменить цвет, правьте переменную (--green, --red, --amber, --bg, --card …).
//   • Размеры: единица --u (1px). Контейнерные запросы (@container) увеличивают её на широких
//     экранах (от 1700 px и от 2200 px) и уменьшают на телефонах (до 760 px: меню уходит вниз).
//   • Классы: .grid/.cg — сетки карточек; .card — карточка; .hero — большая плашка статуса;
//     .kv — пары «название – значение»; .tiles — плитки статистики; .btn — кнопки;
//     .field/.check/.hint — элементы форм; .drop — зона выбора файла прошивки; .actions —
//     панель «Сохранить».
//   • Состояния цвета: классы .on (зелёный), .off (красный), .warn (янтарный), .muted (серый) —
//     их ставят скрипты страниц.
// =============================================================================
static const char CSS[] PROGMEM = R"CSS(
:root{--bg:#f3f5f2;--rail:#fff;--card:#fff;--text:#14201a;--muted:#5d6b63;--line:#dde3de;--nb:#e3f3ea;--green:#12803f;--red:#c62f22;--amber:#8a5a00;--in:#f7f9f7;--inb:#c9d2cc;--bd:#bcc7c0;--okt:#fff;--hb:#8ccaa4;--h1:#e1f4e8;--rb:#e3a49d;--r1:#fde8e5;--ab:#e0c36a;--a1:#fff7dc;--su:1;--mo:.45;--sb:var(--nb);--mb:transparent;--shadow:0 1px 3px #1018281a;color-scheme:light}
@media(prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#101513;--rail:#0c100e;--card:#171e1b;--text:#e7ede9;--muted:#8a9890;--line:#26302b;--nb:#1b2a22;--green:#46d37e;--red:#ff6b5e;--amber:#f5b942;--in:#0f1412;--inb:#2c3832;--bd:#3a4740;--okt:#08130c;--hb:#2c6b47;--h1:#14281d;--rb:#7a2f29;--r1:#2b1614;--ab:#6b5a1f;--a1:#221e12;--su:.45;--mo:1;--sb:transparent;--mb:var(--nb);--shadow:none;color-scheme:dark}}
:root[data-theme=dark]{--bg:#101513;--rail:#0c100e;--card:#171e1b;--text:#e7ede9;--muted:#8a9890;--line:#26302b;--nb:#1b2a22;--green:#46d37e;--red:#ff6b5e;--amber:#f5b942;--in:#0f1412;--inb:#2c3832;--bd:#3a4740;--okt:#08130c;--hb:#2c6b47;--h1:#14281d;--rb:#7a2f29;--r1:#2b1614;--ab:#6b5a1f;--a1:#221e12;--su:.45;--mo:1;--sb:transparent;--mb:var(--nb);--shadow:none;color-scheme:dark}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.a{container-type:inline-size}
.w{--u:1px;display:grid;grid-template-columns:calc(96*var(--u)) minmax(0,1fr);min-height:100vh;font-size:calc(15*var(--u));line-height:1.45}
@container (min-width:1700px){.w{--u:1.15px}}
@container (min-width:2200px){.w{--u:1.4px}}
.r{display:flex;flex-direction:column;align-items:center;gap:6px;padding:calc(20*var(--u)) 0;border-right:1px solid var(--line);background:var(--rail);position:sticky;top:0;height:100vh;align-self:start}
.r a{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;width:calc(72*var(--u));min-height:44px;padding:calc(8*var(--u)) 0;border-radius:12px;color:var(--muted);text-decoration:none;font-size:calc(12*var(--u))}
.r a.active{background:var(--nb);color:var(--green)}
.m{padding:0 calc(36*var(--u)) calc(40*var(--u));display:grid;gap:calc(20*var(--u));align-content:start;min-width:0}
.h{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap;padding:calc(18*var(--u)) 0;border-bottom:1px solid var(--line)}
.br{font:800 calc(18*var(--u))/1.2 system-ui;letter-spacing:.06em}.bs{color:var(--muted);font:calc(12*var(--u)) ui-monospace,Menlo,Consolas,monospace}.c{display:flex;align-items:center;gap:12px}
.langs{display:flex;gap:4px}.langs button{background:none;border:1px solid var(--line);color:var(--muted);border-radius:8px;min-height:40px;padding:0 calc(11*var(--u));font:700 calc(12*var(--u)) ui-monospace,monospace;cursor:pointer}.langs button.active{color:var(--green);border-color:var(--green)}
.tb{display:flex;border:1px solid var(--line);border-radius:999px;padding:3px;cursor:pointer;background:var(--card);color:var(--text);font:inherit}.tb span{display:flex;align-items:center;gap:6px;min-height:34px;padding:0 calc(11*var(--u));border-radius:999px;font-size:calc(13*var(--u))}.sn{opacity:var(--su);background:var(--sb)}.mn{opacity:var(--mo);background:var(--mb)}
.grid{display:grid;gap:calc(20*var(--u));grid-template-columns:repeat(auto-fit,minmax(min(100%,calc(280*var(--u))),1fr))}form>.grid{grid-template-columns:repeat(auto-fit,minmax(min(100%,calc(420*var(--u))),1fr))}.card .grid{grid-template-columns:repeat(auto-fit,minmax(min(100%,calc(300*var(--u))),1fr))}.cg{grid-column:1/-1;display:grid;gap:calc(20*var(--u));grid-template-columns:repeat(auto-fit,minmax(min(100%,calc(280*var(--u))),1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:calc(16*var(--u));padding:calc(20*var(--u));box-shadow:var(--shadow);min-width:0}.wide{grid-column:1/-1}
.hero{display:grid;grid-template-columns:1fr auto;gap:calc(14*var(--u)) calc(24*var(--u));align-items:center}.hero h1{margin:0;font-size:calc(34*var(--u));line-height:1.1}.hero .age{color:var(--muted);margin-top:8px;font-family:ui-monospace,Menlo,Consolas,monospace}.hcd{text-align:right}
.hero:has(h1.on){border-color:var(--hb);background:linear-gradient(90deg,var(--h1),var(--card))}.hero:has(h1.off){border-color:var(--rb);background:linear-gradient(90deg,var(--r1),var(--card))}.hero:has(h1.warn){border-color:var(--ab);background:linear-gradient(90deg,var(--a1),var(--card))}
.hero .dot{display:inline-block;width:.5em;height:.5em;border-radius:50%;margin-right:.6em;background:currentColor;box-shadow:0 0 0 .22em color-mix(in srgb,currentColor 16%,transparent),0 0 0 .5em color-mix(in srgb,currentColor 8%,transparent)}
.bar{grid-column:1/-1;height:4px;background:var(--line);border-radius:4px;overflow:hidden}.bar i{display:block;height:4px;width:100%;transform-origin:left;background:var(--muted);transition:background .4s}.hero:has(h1.on) .bar i{background:var(--green)}.hero:has(h1.off) .bar i{background:var(--red)}.hero:has(h1.warn) .bar i{background:var(--amber)}@keyframes bar{to{transform:scaleX(0)}}
.on{color:var(--green)}.off{color:var(--red)}.warn{color:var(--amber)}.muted{color:var(--muted)}
.row{display:flex;align-items:center;justify-content:space-between;gap:12px}.title{font-size:calc(12*var(--u));font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.08em;margin-bottom:calc(13*var(--u))}.row>.title{margin:0}
.status{font-size:calc(18*var(--u));font-weight:700}.pill{display:inline-flex;align-items:center;border-radius:999px;padding:5px 10px;background:var(--in);border:1px solid var(--line);font:600 calc(12*var(--u)) ui-monospace,monospace}
.metric{font:600 calc(24*var(--u))/1.2 ui-monospace,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums}.sub{color:var(--muted);font-size:calc(13*var(--u));margin-top:4px;overflow-wrap:anywhere}
.kv{display:grid;grid-template-columns:1fr auto;gap:calc(9*var(--u)) calc(14*var(--u))}.kv div:nth-child(odd){color:var(--muted)}.kv b,.tiles b{font:600 calc(15*var(--u)) ui-monospace,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.tiles{display:grid;gap:calc(18*var(--u));margin-top:calc(16*var(--u));grid-template-columns:repeat(auto-fit,minmax(min(100%,calc(190*var(--u))),1fr))}.tiles b{display:block;font-size:calc(21*var(--u));line-height:1.25}.tiles .sub{margin:0 0 4px}
.btn{display:inline-flex;align-items:center;justify-content:center;min-height:44px;border:0;border-radius:10px;padding:0 calc(18*var(--u));background:var(--green);color:var(--okt);text-decoration:none;font:700 calc(15*var(--u)) system-ui;cursor:pointer}.btn.secondary{background:none;border:1px solid var(--bd);color:var(--text)}.btn.danger{background:none;border:1px solid var(--red);color:var(--red)}
.section{margin:0}.card h2{font-size:calc(12*var(--u));font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.08em;margin:0 0 calc(15*var(--u))}
.field{margin:0 0 14px}.field label{display:block;font-weight:600;margin-bottom:6px}.field input:not([type=checkbox]):not([type=radio]),.field select{width:100%;min-height:44px;padding:0 12px;border:1px solid var(--inb);border-radius:10px;background:var(--in);color:var(--text);font:inherit}
.check{display:flex;gap:10px;align-items:flex-start;margin:10px 0}.check input{width:22px;height:22px;margin:1px 0 0;accent-color:var(--green);flex:none}.hint{font-size:calc(13*var(--u));color:var(--muted);margin-top:5px}
details{border-top:1px solid var(--line);padding-top:12px;margin-top:10px}summary{cursor:pointer;font-weight:700;min-height:32px}
.actions{position:sticky;bottom:calc(12*var(--u));background:var(--card);border:1px solid var(--ab);border-radius:12px;padding:10px 12px;display:flex;align-items:center;justify-content:flex-end;gap:10px;box-shadow:0 6px 24px #0003}.toast{display:none;color:var(--amber);font-weight:700;margin-right:auto}
form>.actions{margin-top:calc(20*var(--u))}.drop{position:relative;display:flex;align-items:center;gap:calc(16*var(--u));padding:calc(18*var(--u));border:1.5px dashed var(--bd);border-radius:calc(14*var(--u));background:var(--in);cursor:pointer;transition:border-color .15s,background .15s}.drop input{position:absolute;inset:0;width:100%;height:100%;opacity:0;cursor:pointer}.drop:hover,.drop:focus-within,.drop.dz,.drop.has{border-color:var(--green);background:var(--nb)}.drop.has{border-style:solid}.drop .di{flex:none;width:calc(46*var(--u));height:calc(46*var(--u));border-radius:12px;display:grid;place-items:center;background:var(--nb);color:var(--green)}.drop.has .di{background:var(--green);color:var(--okt)}.drop .dt{flex:1;min-width:0}.drop .dt b{display:block;font-weight:700}.drop .sub{display:block;margin:2px 0 0}.drop .dbtn{pointer-events:none;flex:none}#ota{display:grid;gap:calc(14*var(--u))}#ota .btn{justify-self:start}
.dangerzone{border-color:var(--rb)}.dangerzone h2{color:var(--red)}p.muted{margin:0 0 14px}
@container (max-width:760px){.w{--u:.95px;grid-template-columns:minmax(0,1fr);grid-template-rows:1fr auto}.r{order:2;align-self:end;position:sticky;bottom:0;top:auto;height:auto;flex-direction:row;justify-content:space-around;padding:6px;border-right:0;border-top:1px solid var(--line);z-index:9}.r a{min-height:52px}.m{padding:0 14px 24px}.c{width:100%;justify-content:space-between}.hero{grid-template-columns:1fr}.hcd{text-align:left;display:flex;align-items:baseline;gap:12px}.hcd .title{margin:0}.actions{bottom:calc(72*var(--u))}#ota .btn{justify-self:stretch}.drop{padding:14px;gap:12px}.drop .dbtn{display:none}}
)CSS";



// =============================================================================
// [13] 13.2 ПЕРЕВОДЫ — I18N_JS (скрипт, добавляется в конец каждой страницы)
// Тексты на страницах написаны по-русски. Скрипт переводит их в браузере на украинский/английский.
//   • Таблица R: строка = [«исходный русский текст», замена для русского (null = без замены),
//     «украинский», «английский»]. Текст страницы должен совпадать с первым элементом точно.
//     Чтобы перевести новый текст, добавьте строку в R. Без строки в таблице текст останется
//     русским.
//   • U — правила для составных строк с числами («5 с назад», «2 ч 3 мин», «30 KB»).
//   • trText() переводит одну строку, apply() обходит страницу; MutationObserver переводит и
//      тексты,
//     которые страница обновляет сама (статусы, значения).
//   • Выбранный язык хранится в браузере (localStorage «pm_lang»: ru / ua / en); переключатель
//      RU·UA·EN
//     находится в шапке (head()).
// =============================================================================
static const char I18N_JS[] PROGMEM = R"JS(
<script>
(function(){
// Строка: [исходный текст, русский (null = без изменений), украинский, английский]
const R=[
["Обзор",null,"Огляд","Overview"],
["День",null,"День","Day"],["Ночь",null,"Ніч","Night"],
["Файл не выбран",null,"Файл не вибрано","No file selected"],["Выбрать",null,"Вибрати","Browse"],["Загрузка...",null,"Завантаження...","Uploading..."],
["Не отключайте питание и не закрывайте страницу, пока идёт обновление. После установки устройство перезагрузится.",null,"Не вимикайте живлення й не закривайте сторінку, поки триває оновлення. Після встановлення пристрій перезавантажиться.","Do not power off the device or close this page during the update. The device reboots after installation."],
["Настройки",null,"Налаштування","Settings"],
["Система",null,"Система","System"],
["ОПРЕДЕЛЕНИЕ...",null,"ВИЗНАЧЕННЯ...","DETECTING..."],
["ожидание данных",null,"очікування даних","waiting for data"],
["Устройство 1",null,"Пристрій 1","Device 1"],
["Устройство 2",null,"Пристрій 2","Device 2"],
["Сервер heartbeat",null,"Сервер heartbeat","Heartbeat server"],
["Статистика питания",null,"Статистика живлення","Power statistics"],
["Сеть и цикл",null,"Мережа та цикл перевірки","Network and check cycle"],
["Сбор статистики",null,"Збір статистики","Statistics collection"],
["Крайнее отключение",null,"Останнє відключення","Last outage"],
["Крайнее восстановление",null,"Останнє відновлення","Last restoration"],
["Текущее состояние","Длительность текущего состояния","Тривалість поточного стану","Current state duration"],
["Крайнее OFF","Длительность крайнего отключения","Тривалість останнього відключення","Last outage duration"],
["Отключений","Количество отключений","Кількість відключень","Outage count"],
["Общее время без питания",null,"Загальний час без живлення","Total time without power"],
["Максимальное отключение",null,"Найдовше відключення","Longest outage"],
["Следующая проверка",null,"Наступна перевірка","Next check"],
["Время","Время устройства","Час пристрою","Device time"],
["Контроль питания",null,"Контроль живлення","Power monitoring"],
["Использовать для определения питания",null,"Використовувати для визначення живлення","Use for power detection"],
["Одно устройство ONLINE - питание есть",null,"Один пристрій ONLINE - живлення є","One device ONLINE - power is present"],
["Если отвечает только один из двух контрольных узлов","Если отвечает только одно из двух контрольных устройств","Якщо відповідає лише один із двох контрольних пристроїв","If only one of the two monitored devices responds"],
["Мониторинг",null,"Моніторинг","Monitoring"],
["Проверять устройства каждые",null,"Перевіряти пристрої кожні","Check devices every"],
["Отправлять heartbeat каждые",null,"Надсилати heartbeat кожні","Send heartbeat every"],
["секунд",null,"секунд","seconds"],
["Расширенные параметры",null,"Розширені параметри","Advanced parameters"],
["Попыток ping",null,"Кількість спроб ping","Ping attempts"],
["Таймаут ping, мс",null,"Таймаут ping, мс","Ping timeout, ms"],
["Подтверждение OFF, циклов",null,"Підтвердження OFF, циклів","OFF confirmation, cycles"],
["Подтверждение ON, циклов",null,"Підтвердження ON, циклів","ON confirmation, cycles"],
["Сервер 1",null,"Сервер 1","Server 1"],
["Первый приоритет, если включены оба сервера",null,"Перший пріоритет, якщо ввімкнено обидва сервери","First priority if both servers are enabled"],
["Токен сервера 1",null,"Токен сервера 1","Server 1 token"],
["Сервер 2",null,"Сервер 2","Server 2"],
["Используется после ошибки сервера 1 либо самостоятельно",null,"Використовується після помилки сервера 1 або окремо","Used if server 1 fails, or on its own"],
["Токен сервера 2",null,"Токен сервера 2","Server 2 token"],
["Если оба сервера выключены, heartbeat не отправляется. Если включены оба: Сервер 1 → при ошибке Сервер 2.",null,"Якщо обидва сервери вимкнені, heartbeat не надсилається. Якщо ввімкнені обидва: сервер 1 → у разі помилки сервер 2.","If both servers are disabled, no heartbeat is sent. If both are enabled: Server 1 → Server 2 on failure."],
["Статистика электропитания",null,"Статистика електроживлення","Power statistics"],
["Собирать статистику",null,"Збирати статистику","Collect statistics"],
["Вести накопительный учёт отключений и сохранять его в памяти. Текущее состояние POWER и его длительность работают всегда.",null,"Вести накопичувальний облік відключень і зберігати його в пам’яті. Поточний стан POWER і його тривалість працюють завжди.","Keep cumulative outage statistics and store them in memory. The current POWER state and its duration are always tracked."],
["При выключении накопленные данные сохраняются, но новые события не учитываются. Удаление выполняется отдельно в разделе «Система».",null,"При вимкненні накопичені дані зберігаються, але нові події не враховуються. Видалення виконується окремо в розділі «Система».","When disabled, accumulated data is kept but new events are not counted. Data is cleared separately in System."],
["Включить MQTT",null,"Увімкнути MQTT","Enable MQTT"],
["MQTT работает независимо от HTTP heartbeat",null,"MQTT працює незалежно від HTTP heartbeat","MQTT operates independently of HTTP heartbeat"],
["Автоматически создаёт устройство ESP32-C3 Power Monitor и его сущности в Home Assistant. Discovery prefix: homeassistant",null,"Автоматично створює пристрій ESP32-C3 Power Monitor та його сутності в Home Assistant. Discovery prefix: homeassistant","Automatically creates the ESP32-C3 Power Monitor device and its entities in Home Assistant. Discovery prefix: homeassistant"],
["Новый пароль MQTT",null,"Новий пароль MQTT","New MQTT password"],
["Сохранённый пароль не выводится в Web UI",null,"Збережений пароль не відображається у Web UI","Saved password is never displayed in Web UI"],
["Интервал публикации",null,"Інтервал публікації","Publish interval"],
["Периодическая телеметрия. Изменение POWER ON/OFF отправляется немедленно.",null,"Періодична телеметрія. Зміна POWER ON/OFF надсилається негайно.","Periodic telemetry. POWER ON/OFF changes are sent immediately."],
["Проверка подключения",null,"Перевірка підключення","Connection test"],
["Проверить MQTT",null,"Перевірити MQTT","Test MQTT"],
["Используются значения из формы; сохранять их для теста не нужно.",null,"Використовуються значення з форми; зберігати їх для тесту не потрібно.","Values from the form are used; they do not need to be saved for the test."],
["Безопасность",null,"Безпека","Security"],
["Новый пароль администратора",null,"Новий пароль адміністратора","New administrator password"],
["Минимум 6 символов",null,"Мінімум 6 символів","Minimum 6 characters"],
["Есть несохранённые изменения",null,"Є незбережені зміни","Unsaved changes"],
["Отмена",null,"Скасувати","Cancel"],
["Сохранить",null,"Зберегти","Save"],
["Устройство",null,"Пристрій","Device"],
["Обновление firmware","Обновление прошивки","Оновлення прошивки","Firmware update"],
["Файл прошивки .bin",null,"Файл прошивки .bin","Firmware .bin file"],
["Загрузить и обновить",null,"Завантажити та оновити","Upload and update"],
["Удаляет только накопленную историю отключений. Настройки устройства и текущее состояние POWER не изменяются.",null,"Видаляє лише накопичену історію відключень. Налаштування пристрою та поточний стан POWER не змінюються.","Deletes only accumulated outage history. Device settings and current POWER state are unchanged."],
["Сбросить статистику",null,"Скинути статистику","Reset statistics"],
["Опасная зона",null,"Небезпечна зона","Danger zone"],
["Перезагрузка сохраняет настройки. Сброс настроек удалит конфигурацию мониторинга, серверов и пароль администратора, но статистику электропитания сохранит.",null,"Перезавантаження зберігає налаштування. Скидання налаштувань видалить конфігурацію моніторингу, серверів і пароль адміністратора, але статистику електроживлення збереже.","Reboot preserves settings. Factory reset removes monitoring/server configuration and the administrator password, but preserves power statistics."],
["Перезагрузить",null,"Перезавантажити","Reboot"],
["Сбросить настройки",null,"Скинути налаштування","Factory reset"],
["Оставьте пустым, чтобы не менять",null,"Залиште порожнім, щоб не змінювати","Leave blank to keep unchanged"],
["Сохранённый токен никогда не выводится в Web UI",null,"Збережений токен ніколи не відображається у Web UI","Saved token is never displayed in Web UI"],
["ЭЛЕКТРИЧЕСТВО ЕСТЬ",null,"ЕЛЕКТРОЕНЕРГІЯ В НАЯВНОСТІ","POWER ON"],
["ЭЛЕКТРИЧЕСТВА НЕТ",null,"ЕЛЕКТРОЕНЕРГІЇ НЕМАЄ","POWER OFF"],
["ожидание определения",null,"очікування визначення","waiting for detection"],
["IP","IP-адрес","IP-адреса","IP address"],
["IP-адрес",null,"IP-адреса","IP address"],
["Wi-Fi",null,"Wi-Fi","Wi-Fi"],
["Uptime","Время работы","Час роботи","Uptime"],
["с",null,"с","s"],
["Отключено",null,"Вимкнено","Disabled"],
["ВКЛЮЧЕН",null,"УВІМКНЕНО","ENABLED"],
["ОТКЛЮЧЕН",null,"ВИМКНЕНО","DISABLED"],
["ОТКЛЮЧЁН",null,"ВИМКНЕНО","DISABLED"],
["ПОДКЛЮЧЕН",null,"ПІДКЛЮЧЕНО","CONNECTED"],
["ОШИБКА",null,"ПОМИЛКА","ERROR"],
["серверы не выбраны",null,"сервери не вибрано","no servers selected"],
["Ещё не отправлялось",null,"Ще не надсилалося","Not sent yet"],
["ожидание первой отправки",null,"очікування першого надсилання","waiting for first send"],
["MQTT выключен",null,"MQTT вимкнено","MQTT disabled"],
["URL сервера 1",null,"URL сервера 1","Server 1 URL"],
["URL сервера 2",null,"URL сервера 2","Server 2 URL"],
["MQTT / Home Assistant",null,"MQTT / Home Assistant","MQTT / Home Assistant"],
["Home Assistant Discovery",null,"Home Assistant Discovery","Home Assistant Discovery"],
["Broker","Брокер","Брокер","Broker"],
["Port","Порт","Порт","Port"],
["Username","Имя пользователя","Ім’я користувача","Username"],
["Client ID","ID клиента","ID клієнта","Client ID"],
["Base topic","Базовый топик","Базовий топік","Base topic"],
["192.168.1.10 или mqtt.local",null,"192.168.1.10 або mqtt.local","192.168.1.10 or mqtt.local"],
["30 секунд",null,"30 секунд","30 seconds"],
["5 минут",null,"5 хвилин","5 minutes"],
["30 минут",null,"30 хвилин","30 minutes"],
["1 час",null,"1 година","1 hour"],
["3 часа",null,"3 години","3 hours"],
["6 часов",null,"6 годин","6 hours"],
["12 часов",null,"12 годин","12 hours"],
["24 часа",null,"24 години","24 hours"],
["Проверяю...",null,"Перевіряю...","Checking..."],
["Подключение успешно",null,"Підключення успішне","Connection successful"],
["Ошибка запроса",null,"Помилка запиту","Request failed"],
["Wi-Fi не подключён",null,"Wi-Fi не підключено","Wi-Fi is not connected"],
["Укажите MQTT broker",null,"Вкажіть MQTT broker","Specify the MQTT broker"],
["Chip","Чип","Чип","Chip"],
["Firmware","Прошивка","Прошивка","Firmware"],
["Build","Сборка","Збірка","Build"],
["Reset reason","Причина перезапуска","Причина перезапуску","Reset reason"],
["Runtime","Работа и ресурсы","Робота та ресурси","Runtime"],
["Free heap","Свободная память","Вільна пам’ять","Free heap"],
["Minimum heap","Минимум свободной памяти","Мінімум вільної пам’яті","Minimum heap"],
["Largest block","Наибольший свободный блок","Найбільший вільний блок","Largest block"],
["RSSI","Уровень сигнала (RSSI)","Рівень сигналу (RSSI)","RSSI"],
["Сбросить статистику электропитания? Настройки устройства останутся без изменений.",null,"Скинути статистику електроживлення? Налаштування пристрою залишаться без змін.","Reset power statistics? Device settings will remain unchanged."],
["Удалить настройки устройства и перезагрузить? Статистика электропитания будет сохранена.",null,"Видалити налаштування пристрою та перезавантажити? Статистику електроживлення буде збережено.","Delete device settings and reboot? Power statistics will be kept."],
["Power-on","Включение питания","Увімкнення живлення","Power-on"],
["Software","Программный перезапуск","Програмне перезавантаження","Software"],
["Panic","Аварийный сбой (panic)","Аварійний збій (panic)","Panic"],
["Interrupt WDT","Сторожевой таймер прерываний","Сторожовий таймер переривань","Interrupt WDT"],
["Task WDT","Сторожевой таймер задачи","Сторожовий таймер задачі","Task WDT"],
["Watchdog","Сторожевой таймер","Сторожовий таймер","Watchdog"],
["Deep sleep","Выход из глубокого сна","Вихід із глибокого сну","Deep sleep"],
["Brownout","Просадка питания","Просідання живлення","Brownout"],
["Other","Другое","Інше","Other"],
["Connection timeout","Тайм-аут подключения","Тайм-аут підключення","Connection timeout"],
["Connection lost","Соединение потеряно","З’єднання втрачено","Connection lost"],
["Connect failed","Не удалось подключиться","Не вдалося підключитися","Connect failed"],
["Disconnected","Отключено","Відключено","Disconnected"],
["Connected","Подключено","Підключено","Connected"],
["Bad protocol","Неверный протокол","Неправильний протокол","Bad protocol"],
["Bad client ID","Неверный Client ID","Неправильний Client ID","Bad client ID"],
["Broker unavailable","Брокер недоступен","Брокер недоступний","Broker unavailable"],
["Bad credentials","Неверный логин или пароль","Неправильний логін або пароль","Bad credentials"],
["Unauthorized","Доступ запрещён","Доступ заборонено","Unauthorized"],
["Unknown","Неизвестно","Невідомо","Unknown"]
];
const D={ru:{},ua:{},en:{}};
R.forEach(r=>{if(r[1])D.ru[r[0]]=r[1];D.ua[r[0]]=r[2];D.en[r[0]]=r[3]});
// Составные строки с числами. Единицы только после цифры и только целым словом (без «сек» внутри «секунд»).
const U={
ru:[[/(\d) ms\b/g,"$1 мс"],[/(\d) KB\b/g,"$1 КБ"]],
ua:[[/^текущее состояние ([\s\S]*)$/,"поточний стан $1"],[/последний успех (\d+) с назад/,"останній успіх $1 с тому"],[/публикация (\d+) с назад/,"публікація $1 с тому"],[/интервал /,"інтервал "],
[/(\d+) д(?![а-яё])/g,"$1 дн"],[/(\d+) ч(?![а-яё])/g,"$1 год"],[/(\d+) мин(?![а-яё])/g,"$1 хв"],[/(\d+) сек(?![а-яё])/g,"$1 с"],[/(\d) ms\b/g,"$1 мс"],[/(\d) KB\b/g,"$1 КБ"]],
en:[[/^текущее состояние ([\s\S]*)$/,"current state $1"],[/последний успех (\d+) с назад/,"last success $1 s ago"],[/публикация (\d+) с назад/,"published $1 s ago"],[/интервал /,"interval "],
[/(\d+) д(?![а-яё])/g,"$1 d"],[/(\d+) ч(?![а-яё])/g,"$1 h"],[/(\d+) мин(?![а-яё])/g,"$1 min"],[/(\d+) сек(?![а-яё])/g,"$1 s"]]
};
let lang=localStorage.getItem("pm_lang")||"ru";
if(!D[lang])lang="ru";
function trText(v){
 const m=v.match(/^([●○✓✕]\s*)([\s\S]*)$/),p=m?m[1]:"",b=m?m[2]:v;
 let z=D[lang][b];
 if(z===undefined){z=b;(U[lang]||[]).forEach(x=>z=z.replace(x[0],x[1]))}
 return p+z;
}
const oc=window.confirm.bind(window);window.confirm=function(m){return oc(trText(m))};
function apply(root){
 document.documentElement.lang=lang==="ua"?"uk":lang;
 document.title=document.title.split(" · ").map(trText).join(" · ");
 document.querySelectorAll(".langs button").forEach(b=>b.classList.toggle("active",b.dataset.lang===lang));
 let w=document.createTreeWalker(root||document.body,NodeFilter.SHOW_TEXT);
 let n,a=[];while(n=w.nextNode())if(n.parentElement&&!["SCRIPT","STYLE"].includes(n.parentElement.tagName)&&n.nodeValue.trim())a.push(n);
 a.forEach(n=>{let raw=n.nodeValue.trim(),t=trText(raw);if(t!==raw)n.nodeValue=n.nodeValue.replace(raw,()=>t)});
 document.querySelectorAll("[placeholder]").forEach(e=>{let t=trText(e.placeholder);if(t!==e.placeholder)e.placeholder=t});
}
document.addEventListener("click",e=>{let b=e.target.closest(".langs button");if(!b)return;localStorage.setItem("pm_lang",b.dataset.lang);location.reload()});
document.addEventListener("DOMContentLoaded",()=>{apply(document.body);new MutationObserver(m=>m.forEach(x=>x.addedNodes.forEach(n=>{if(n.nodeType===1)apply(n);else if(n.nodeType===3&&n.parentElement)apply(n.parentElement)}))).observe(document.body,{subtree:true,childList:true,characterData:true})});
window.__pmTr=trText;
})();
</script>
)JS";


// =============================================================================
// [13] 13.3 ШАПКА И МЕНЮ — head() и sendPage()
// head(заголовок, активный пункт) — начало любой страницы: <head> со стилями, боковое/нижнее меню
// (три пункта: Обзор, Настройки, Система), шапка «⚡ POWER MONITOR · ESP32-C3 · версия»,
//    переключатель
// языка и кнопка «День / Ночь» (тема хранится в localStorage «pm_theme»; нет значения = «Авто»).
// sendPage() — добавляет скрипт перевода и скрипт кнопки темы, закрывает страницу и отправляет её.
// Чтобы переименовать/добавить пункт меню: массивы hrefs[], names[], ids[], icons[] в head().
// =============================================================================
static String head(const char* title, const char* active) {
  String s;
  s.reserve(5000);
  s += F("<!doctype html><html lang=ru><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><meta name=color-scheme content='light dark'><script>try{var t=localStorage.getItem('pm_theme');if(t)document.documentElement.dataset.theme=t}catch(e){}</script><title>");
  s += title;
  s += F("</title><style>");
  s += FPSTR(CSS);
  s += F("</style></head><body><div class=a><div class=w><nav class=r>");
  const char* hrefs[] = { "/", "/settings", "/system" };
  const char* names[] = { "Обзор", "Настройки", "Система" };
  const char* ids[] = { "home", "settings", "system" };
  const char* icons[] = {
    "<svg width=24 height=24 fill=none stroke=currentColor stroke-width=1.8><rect x=3 y=3 width=7 height=9 rx=2 /><rect x=14 y=3 width=7 height=5 rx=2 /><rect x=14 y=12 width=7 height=9 rx=2 /><rect x=3 y=16 width=7 height=5 rx=2 /></svg>",
    "<svg width=24 height=24 fill=none stroke=currentColor stroke-width=1.8><path d='M4 7h10M18 7h2M4 17h2M10 17h10' /><circle cx=16 cy=7 r=2 /><circle cx=8 cy=17 r=2 /></svg>",
    "<svg width=24 height=24 fill=none stroke=currentColor stroke-width=1.8><rect x=6 y=6 width=12 height=12 rx=2 /><path d='M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4' /></svg>"
  };
  for (int i = 0; i < 3; i++) {
    s += "<a href='";
    s += hrefs[i];
    s += "'";
    if (!strcmp(active, ids[i])) s += " class=active";
    s += ">";
    s += icons[i];
    s += names[i];
    s += "</a>";
  }
  s += F("</nav><main class=m><header class=h><div><div class=br>⚡ POWER MONITOR</div><div class=bs>ESP32-C3 · v" FW_VERSION "</div></div><div class=c>"
         "<div class=langs><button type=button data-lang=ru>RU</button><button type=button data-lang=ua>UA</button><button type=button data-lang=en>EN</button></div>"
         "<button type=button id=thb class=tb aria-label='Day / Night'><span class=sn><svg width=16 height=16 fill=none stroke=currentColor stroke-width=2><circle cx=8 cy=8 r=3 /><path d='M8 1v2M8 13v2M1 8h2M13 8h2M3 3l1.5 1.5M11.5 11.5L13 13M13 3l-1.5 1.5M4.5 11.5L3 13' /></svg>День</span>"
         "<span class=mn><svg width=16 height=16 fill=none stroke=currentColor stroke-width=2><path d='M13 9.5A5.5 5.5 0 0 1 6.5 3a5.5 5.5 0 1 0 6.5 6.5z' /></svg>Ночь</span></button></div></header>");
  return s;
}
// Завершает страницу (скрипты, закрывающие теги) и отправляет браузеру без кэширования.
static void sendPage(String& s) {
  s += FPSTR(I18N_JS);
  s += F("<script>(function(){var r=document.documentElement,m=matchMedia('(prefers-color-scheme:dark)');document.getElementById('thb').onclick=function(){var n=(r.dataset.theme||(m.matches?'dark':'light'))=='dark'?'light':'dark';try{if(n==(m.matches?'dark':'light')){localStorage.removeItem('pm_theme');delete r.dataset.theme}else{localStorage.setItem('pm_theme',n);r.dataset.theme=n}}catch(e){r.dataset.theme=n}}})()</script></main></div></div></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", s);
}


// =============================================================================
// [14] СТРАНИЦЫ «ОБЗОР» И «НАСТРОЙКИ»
// Страница «Обзор» (адрес /). Разметка — одна длинная строка F("..."), ниже неё скрипт.
// Блоки: плашка статуса (#st, #age) и обратный отсчёт проверки (#cd, полоска #bar); устройства 1 и
// 2
// (#d1s/#d1ip/#d1lat, #d2s/…); сервер heartbeat (#sv, #sva, #code); MQTT (#mq, #mqa, #mqs); сеть
// (#wifi, #ip, #upt, #t); плитки статистики (#sen, #loff, #lon, #page, #ldur, #ocnt, #tdur, #mdur).
// Скрипт страницы: upd() каждые 2 с запрашивает /api/status и заполняет эти элементы; tick() каждую
// секунду «досчитывает» длительности между запросами; dur() форматирует секунды в «д ч мин»;
// полоска #bar анимируется CSS по полям cdi/cdms из /api/status.
// Если меняете id в разметке — меняйте и в скрипте (и наоборот).
// =============================================================================
void handleRoot() {
  if (!checkAuth()) return;
  String s = head("Power Monitor", "home");
  s += F("<div class=grid><section class='card wide hero'><div><h1 id=st><span class=dot></span>ОПРЕДЕЛЕНИЕ...</h1><div class=age id=age>ожидание данных</div></div><div class=hcd><div class=title>Следующая проверка</div><b class=metric><span id=cd>—</span> с</b></div><div class=bar><i id=bar></i></div></section>"
         "<div class=cg><section class=card><div class=title>Устройство 1</div><div class=row><div><div class=status id=d1s>—</div><div class=sub id=d1ip>—</div></div><div class=metric id=d1lat>—</div></div></section>"
         "<section class=card><div class=title>Устройство 2</div><div class=row><div><div class=status id=d2s>—</div><div class=sub id=d2ip>—</div></div><div class=metric id=d2lat>—</div></div></section></div>"
         "<div class=cg><section class=card><div class=title>Сервер heartbeat</div><div class=row><div><div class=status id=sv>—</div><div class=sub id=sva>—</div></div><span class=pill id=code>HTTP —</span></div></section>"
         "<section class=card><div class=title>MQTT</div><div class=row><div><div class=status id=mq>—</div><div class=sub id=mqa>—</div></div><span class=pill id=mqs>—</span></div></section>"
         "<section class=card><div class=title>Сеть и цикл</div><div class=kv><div>Wi-Fi</div><b id=wifi>—</b><div>IP</div><b id=ip>—</b><div>Uptime</div><b id=upt>—</b><div>Время</div><b id=t>—</b></div></section></div>"
         "<section class='card wide'><div class=row><div class=title>Статистика питания</div><div class=sub>Сбор статистики <b id=sen>—</b></div></div><div class=tiles>"
         "<div><div class=sub>Крайнее отключение</div><b id=loff>—</b></div><div><div class=sub>Крайнее восстановление</div><b id=lon>—</b></div><div><div class=sub>Текущее состояние</div><b id=page>—</b></div><div><div class=sub>Крайнее OFF</div><b id=ldur>—</b></div>"
         "<div><div class=sub>Отключений</div><b id=ocnt>0</b></div><div><div class=sub>Общее время без питания</div><b id=tdur>—</b></div><div><div class=sub>Максимальное отключение</div><b id=mdur>—</b></div></div></section></div>"
         "<script>const $=x=>document.getElementById(x);let localAge=-1,localUptime=0,lastSync=0,cdEnd=0;function dur(n){n=Math.max(0,n|0);let d=Math.floor(n/86400),h=Math.floor(n%86400/3600),m=Math.floor(n%3600/60),q=[];if(d)q.push(d+' д');if(h)q.push(h+' ч');if(m||!q.length)q.push(m+' мин');return q.join(' ')}function mi(n){return n<60?n+' сек':n<3600?(n/60)+' мин':(n/3600)+' ч'}function dt(v){return v?v.substring(0,19).replace('T',' '):'—'}function dev(p,en,on,ip,l){$(p+'ip').textContent=ip;if(!en){$(p+'s').textContent='Отключено';$(p+'s').className='status muted';$(p+'lat').textContent='—';return}$(p+'s').textContent=on?'● ONLINE':'● OFFLINE';$(p+'s').className='status '+(on?'on':'off');$(p+'lat').textContent=on?l+' ms':'—'}function upd(){if(document.hidden)return;fetch('/api/status').then(r=>r.json()).then(x=>{localAge=x.state_age;localUptime=x.uptime||0;lastSync=Date.now();let st=$('st');if(x.state==1){st.innerHTML='<span class=dot></span>ЭЛЕКТРИЧЕСТВО ЕСТЬ';st.className='on'}else if(x.state==2){st.innerHTML='<span class=dot></span>ЭЛЕКТРИЧЕСТВА НЕТ';st.className='off'}else{st.innerHTML='<span class=dot></span>ОПРЕДЕЛЕНИЕ...';st.className='warn'}$('age').textContent=localAge>=0?'текущее состояние '+dur(localAge):'ожидание определения';$('sen').textContent=x.stats_enabled?'● ВКЛЮЧЕН':'○ ОТКЛЮЧЕН';$('sen').className=x.stats_enabled?'on':'muted';$('loff').textContent=dt(x.last_power_off);$('lon').textContent=dt(x.last_power_on);$('page').textContent=localAge>=0?dur(localAge):'—';$('ldur').textContent=x.last_outage_duration?dur(x.last_outage_duration):'—';$('ocnt').textContent=x.outage_count;$('tdur').textContent=x.total_outage_duration?dur(x.total_outage_duration):'—';$('mdur').textContent=x.max_outage_duration?dur(x.max_outage_duration):'—';dev('d1',x.d1_en,x.d1_on,x.d1_ip,x.d1_lat);dev('d2',x.d2_en,x.d2_on,x.d2_ip,x.d2_lat);let he=x.srv1_en||x.srv2_en;if(!he){$('sv').textContent='○ ОТКЛЮЧЁН';$('sv').className='status muted';$('sva').textContent='серверы не выбраны';$('code').textContent='HTTP —'}else{$('sv').textContent=x.srv_age<0?'Ещё не отправлялось':x.srv_ok?'● HEARTBEAT OK':'● ОШИБКА';$('sv').className='status '+(x.srv_age<0?'muted':x.srv_ok?'on':'off');$('sva').textContent=x.srv_age<0?'ожидание первой отправки':'последний успех '+x.srv_age+' с назад · '+x.srv_lat+' ms';$('code').textContent=x.srv_code?'HTTP '+x.srv_code:'HTTP —'};if(!x.mqtt_en){$('mq').textContent='○ ОТКЛЮЧЁН';$('mq').className='status muted';$('mqa').textContent='MQTT выключен';$('mqs').textContent='—'}else{$('mq').textContent=x.mqtt_ok?'● ПОДКЛЮЧЕН':'● ОШИБКА';$('mq').className='status '+(x.mqtt_ok?'on':'off');$('mqa').textContent=x.mqtt_host+':'+x.mqtt_port+(x.mqtt_pub_age>=0?' · публикация '+x.mqtt_pub_age+' с назад':'')+' · интервал '+mi(x.mqtt_interval);$('mqs').textContent=x.mqtt_ok?'ONLINE':x.mqtt_msg};$('wifi').textContent=x.rssi+' dBm';$('ip').textContent=x.ip;$('upt').textContent=dur(localUptime);$('cd').textContent=x.cd;if(Math.abs(cdEnd-Date.now()-x.cdms)>400){cdEnd=Date.now()+x.cdms;let b=$('bar'),iv=x.cdi||1;b.style.animation='none';b.offsetWidth;b.style.animation='bar '+iv+'s linear '+Math.min(0,x.cdms/1000-iv)+'s infinite'};$('t').textContent=x.time}).catch(()=>{})}function tick(){if(document.hidden)return;if(localAge>=0)localAge++;localUptime++;$('age').textContent=localAge>=0?'текущее состояние '+dur(localAge):'ожидание определения';$('page').textContent=localAge>=0?dur(localAge):'—';$('upt').textContent=dur(localUptime)}upd();setInterval(tick,1000);setInterval(upd,2000)</script>");
  sendPage(s);
}

// Страница «Настройки» (адрес /settings): форма POST на /save. Поля name= совпадают с ключами в
// handleSave() и /api/config. Значения подставляет скрипт внизу страницы из /api/config; он же
// показывает «Есть несохранённые изменения» и выполняет «Проверить MQTT» (POST на /mqtt/test).
// Порядок блоков: Контроль питания, Мониторинг, Сервер heartbeat, MQTT / Home Assistant,
// Статистика электропитания, Безопасность. Пароли и токены в форму не подставляются (пустое поле =
// не менять).
void handleSettings() {
  if (!checkAuth()) return;
  String s = head("Настройки · Power Monitor", "settings");
  s += F("<form id=f action=/save method=POST><div class=grid><div class=cg><section class='card section'><h2>Контроль питания</h2>"
         "<label class=check><input type=checkbox name=d1_en><span><b>Устройство 1</b><div class=hint>Использовать для определения питания</div></span></label><div class=field><label>IP-адрес</label><input name=d1_ip inputmode=decimal></div>"
         "<label class=check><input type=checkbox name=d2_en><span><b>Устройство 2</b><div class=hint>Использовать для определения питания</div></span></label><div class=field><label>IP-адрес</label><input name=d2_ip inputmode=decimal></div>"
         "<label class=check><input type=checkbox name=part_on><span><b>Одно устройство ONLINE - питание есть</b><div class=hint>Если отвечает только один из двух контрольных узлов</div></span></label></section>"
         "<section class='card section'><h2>Мониторинг</h2><div class=field><label>Проверять устройства каждые</label><input type=number name=chk_int min=2 max=3600><div class=hint>секунд</div></div><div class=field><label>Отправлять heartbeat каждые</label><input type=number name=hb_int min=10 max=86400><div class=hint>секунд</div></div>"
         "<details><summary>Расширенные параметры</summary><br><div class=field><label>Попыток ping</label><input type=number name=p_cnt min=1 max=5></div><div class=field><label>Таймаут ping, мс</label><input type=number name=p_to min=100 max=3000></div><div class=field><label>Подтверждение OFF, циклов</label><input type=number name=c_off min=1 max=20></div><div class=field><label>Подтверждение ON, циклов</label><input type=number name=c_on min=1 max=20></div></details></section></div>"
         "<section class='card wide section'><h2>Сервер heartbeat</h2><div class=grid><div><label class=check><input type=checkbox name=s1_en><span><b>Сервер 1</b><div class=hint>Первый приоритет, если включены оба сервера</div></span></label><div class=field><label>URL сервера 1</label><input name=s_url></div><div class=field><label>Токен сервера 1</label><input type=password name=s_tok autocomplete=new-password placeholder='Оставьте пустым, чтобы не менять'><div class=hint>Сохранённый токен никогда не выводится в Web UI</div></div></div><div><label class=check><input type=checkbox name=s2_en><span><b>Сервер 2</b><div class=hint>Используется после ошибки сервера 1 либо самостоятельно</div></span></label><div class=field><label>URL сервера 2</label><input name=b_url></div><div class=field><label>Токен сервера 2</label><input type=password name=b_tok autocomplete=new-password placeholder='Оставьте пустым, чтобы не менять'></div></div></div><div class=hint>Если оба сервера выключены, heartbeat не отправляется. Если включены оба: Сервер 1 → при ошибке Сервер 2.</div></section>"
         "<section class='card wide section'><h2>MQTT / Home Assistant</h2><label class=check><input type=checkbox name=mq_en id=mq_en><span><b>Включить MQTT</b><div class=hint>MQTT работает независимо от HTTP heartbeat</div></span></label><label class=check><input type=checkbox name=ha_disc><span><b>Home Assistant Discovery</b><div class=hint>Автоматически создаёт устройство ESP32-C3 Power Monitor и его сущности в Home Assistant. Discovery prefix: homeassistant</div></span></label><div class=grid><div><div class=field><label>Broker</label><input name=mq_host placeholder='192.168.1.10 или mqtt.local'></div><div class=field><label>Port</label><input type=number name=mq_port min=1 max=65535></div><div class=field><label>Username</label><input name=mq_user autocomplete=username></div><div class=field><label>Новый пароль MQTT</label><input type=password name=mq_pass autocomplete=new-password placeholder='Оставьте пустым, чтобы не менять'><div class=hint>Сохранённый пароль не выводится в Web UI</div></div></div><div><div class=field><label>Client ID</label><input name=mq_cid></div><div class=field><label>Base topic</label><input name=mq_base></div><div class=field><label>Интервал публикации</label><select name=mq_int><option value=30>30 секунд</option><option value=300>5 минут</option><option value=1800>30 минут</option><option value=3600>1 час</option><option value=10800>3 часа</option><option value=21600>6 часов</option><option value=43200>12 часов</option><option value=86400>24 часа</option></select><div class=hint>Периодическая телеметрия. Изменение POWER ON/OFF отправляется немедленно.</div></div><div class=field><label>Проверка подключения</label><button class='btn secondary' type=button id=mqtest>Проверить MQTT</button><div class=hint id=mqtestres>Используются значения из формы; сохранять их для теста не нужно.</div></div></div></div></section><section class='card wide section'><h2>Статистика электропитания</h2><label class=check><input type=checkbox name=stats_en><span><b>Собирать статистику</b><div class=hint>Вести накопительный учёт отключений и сохранять его в памяти. Текущее состояние POWER и его длительность работают всегда.</div></span></label><div class=hint>При выключении накопленные данные сохраняются, но новые события не учитываются. Удаление выполняется отдельно в разделе «Система».</div></section>"
         "<section class='card wide section'><h2>Безопасность</h2><div class=field><label>Новый пароль администратора</label><input type=password name=adm_pass minlength=6 maxlength=63 autocomplete=new-password placeholder='Оставьте пустым, чтобы не менять'><div class=hint>Минимум 6 символов</div></div></section></div>"
         "<div class=actions><span class=toast id=dirty>Есть несохранённые изменения</span><a class='btn secondary' href=/settings>Отмена</a><button class=btn type=submit>Сохранить</button></div></form>"
         "<script>const f=document.getElementById('f'),dirty=document.getElementById('dirty'),tr=document.getElementById('mqtestres');fetch('/api/config').then(r=>r.json()).then(c=>{for(const k in c){let e=document.getElementsByName(k)[0];if(e)e.type==='checkbox'?e.checked=!!c[k]:e.value=c[k]}});f.addEventListener('input',()=>dirty.style.display='inline');f.addEventListener('change',()=>dirty.style.display='inline');document.getElementById('mqtest').onclick=async()=>{tr.textContent='Проверяю...';try{let r=await fetch('/mqtt/test',{method:'POST',body:new FormData(f)}),x=await r.json();tr.textContent=x.ok?'✓ Подключение успешно':'✕ '+x.message;tr.className='hint '+(x.ok?'on':'off')}catch(e){tr.textContent='✕ Ошибка запроса';tr.className='hint off'}}</script>");
  sendPage(s);
}


// =============================================================================
// [15] API И СОХРАНЕНИЕ НАСТРОЕК
// /api/status (GET) — текущее состояние для страницы «Обзор» (JSON). Основные поля: state (0
// неизвестно /
// 1 есть / 2 нет), state_age, cd/cdi/cdms (остаток и интервал проверки), d1_*/d2_*, srv_*, mqtt_*,
//    статистика,
// rssi, ip, uptime. Буфер b[1750] (худший случай ~1250 байт) — при добавлении полей проверяйте
// размер.
// /api/config (GET) — настройки для формы (без паролей и токенов).
// /save (POST) — приём формы, проверка, запись.   /mqtt/test (POST) — пробное подключение к
// брокеру.
// =============================================================================
void handleApiStatus() {
  if (!checkAuth()) return;
  Config c = configSnapshot();
  int activeCheckInterval = (int)effectiveCheckIntervalSec(c);
  int rem = activeCheckInterval - (int)((millis() - g_check_ms) / 1000);
  if (rem < 0) rem = 0;
  long remMs = (long)activeCheckInterval * 1000L - (long)(millis() - g_check_ms);
  if (remMs < 0) remMs = 0;
  long age = g_srv_seen ? (long)((millis() - g_srv_ok_ms) / 1000) : -1;
  long stateAge = g_state == ST_UNKNOWN ? -1 : (long)currentPowerAge();
  char tm[12];
  fmtTime(tm, sizeof tm);
  char b[1750], lastOff[40], lastOn[40];
  epochIso(g_last_power_off, lastOff, sizeof lastOff);
  epochIso(g_last_power_on, lastOn, sizeof lastOn);
  char ip[16];
  ipStr(ip);
  long mqttPubAge = g_mqtt_pub_ms ? (long)((millis() - g_mqtt_pub_ms) / 1000) : -1;
  snprintf(b, sizeof b,
           "{\"time\":\"%s\",\"state\":%u,\"state_age\":%ld,\"stats_enabled\":%d,\"last_power_off\":\"%s\",\"last_power_on\":\"%s\",\"last_outage_duration\":%lu,\"outage_count\":%lu,\"total_outage_duration\":%llu,\"max_outage_duration\":%lu,\"d1_en\":%d,\"d1_ip\":\"%s\",\"d1_on\":%d,\"d1_lat\":%d,\"d2_en\":%d,\"d2_ip\":\"%s\",\"d2_on\":%d,\"d2_lat\":%d,\"cd\":%d,\"cdi\":%d,\"cdms\":%ld,\"srv1_en\":%d,\"srv2_en\":%d,\"srv_ok\":%d,\"srv_code\":%d,\"srv_age\":%ld,\"srv_lat\":%lu,\"mqtt_en\":%d,\"mqtt_ok\":%d,\"mqtt_state\":%d,\"mqtt_msg\":\"%s\",\"mqtt_host\":\"%s\",\"mqtt_port\":%d,\"mqtt_interval\":%d,\"mqtt_pub_age\":%ld,\"mqtt_tx\":%lu,\"mqtt_reconnects\":%lu,\"mqtt_reason\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"uptime\":%lu,\"fw\":\"" FW_VERSION "\"}",
           tm, (unsigned)g_state, stateAge, c.stats_enabled, lastOff, lastOn,
           (unsigned long)g_last_outage_duration, (unsigned long)g_outage_count,
           (unsigned long long)g_total_outage_duration, (unsigned long)g_max_outage_duration,
           c.dev1_enabled, c.dev1_ip, (int)d1_on, (int)d1_lat, c.dev2_enabled, c.dev2_ip, (int)d2_on, (int)d2_lat, rem, activeCheckInterval, remMs,
           c.server1_enabled, c.server2_enabled, (int)g_srv_ok, (int)g_srv_code, age, (unsigned long)g_srv_lat,
           c.mqtt_enabled, (int)g_mqtt_ok, (int)g_mqtt_state, mqttStateText(g_mqtt_state), c.mqtt_host, c.mqtt_port, c.mqtt_interval, mqttPubAge,
           (unsigned long)g_mqtt_tx_count, (unsigned long)g_mqtt_reconnects, g_mqtt_last_reason, WiFi.RSSI(), ip, (unsigned long)(millis() / 1000));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", b);
}
// Отдаёт текущие настройки в JSON для заполнения формы на странице «Настройки». Пароли и токены не
// отдаются.
void handleApiConfig() {
  if (!checkAuth()) return;
  Config c = configSnapshot();
  String o;
  o.reserve(700);
  o += '{';
  jn(o, "d1_en", c.dev1_enabled);
  js(o, "d1_ip", c.dev1_ip);
  jn(o, "d2_en", c.dev2_enabled);
  js(o, "d2_ip", c.dev2_ip);
  jn(o, "chk_int", c.check_interval);
  jn(o, "p_cnt", c.ping_count);
  jn(o, "p_to", c.ping_timeout);
  jn(o, "c_off", c.confirm_off_cycles);
  jn(o, "c_on", c.confirm_on_cycles);
  jn(o, "part_on", c.partial_is_on);
  jn(o, "s1_en", c.server1_enabled);
  js(o, "s_url", c.oracle_url);
  jn(o, "s2_en", c.server2_enabled);
  js(o, "b_url", c.backup_url);
  jn(o, "hb_int", c.heartbeat_interval);
  jn(o, "mq_en", c.mqtt_enabled);
  jn(o, "ha_disc", c.ha_discovery);
  jn(o, "stats_en", c.stats_enabled);
  js(o, "mq_host", c.mqtt_host);
  jn(o, "mq_port", c.mqtt_port);
  jn(o, "mq_int", c.mqtt_interval);
  js(o, "mq_user", c.mqtt_user);
  js(o, "mq_cid", c.mqtt_client_id);
  js(o, "mq_base", c.mqtt_base_topic);
  o.setCharAt(o.length() - 1, '}');
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", o);
}
// Помощники чтения полей формы: argInt — число в диапазоне [lo, hi]; argIp — корректный IP; argUrl
// — адрес http(s)://…; argTok — токен/пароль (пустое = не менять); argStr — строка.
static void argInt(const char* n, int lo, int hi, int& v) {
  String a = server.arg(n);
  if (a.length()) v = constrain((int)a.toInt(), lo, hi);
}
static void argIp(const char* n, char* dst) {
  String a = server.arg(n);
  a.trim();
  IPAddress t;
  if (t.fromString(a)) strlcpy(dst, a.c_str(), 64);
}
static void argUrl(const char* n, char* dst, size_t sz) {
  String a = server.arg(n);
  a.trim();
  if (a.length() < sz && (!a.length() || a.startsWith("http://") || a.startsWith("https://"))) strlcpy(dst, a.c_str(), sz);
}
static void argTok(const char* n, char* dst, size_t sz) {
  String a = server.arg(n);
  if (a.length() && a.length() < sz) strlcpy(dst, a.c_str(), sz);
}
static void argStr(const char* n, char* dst, size_t sz, bool allowEmpty = true) {
  String a = server.arg(n);
  a.trim();
  if (a.length() < sz && (allowEmpty || a.length())) strlcpy(dst, a.c_str(), sz);
}
// Приём формы «Настройки»: читает поля, проверяет, обновляет cfg (под мьютексом, cfg_ver++), пишет
// во флеш,
// сбрасывает MQTT-соединение для переподключения с новыми параметрами и перенаправляет на
// /settings.
// Флажки (checkbox): если поля нет в запросе — значит снят (server.hasArg).
void handleSave() {
  if (!checkAuth()) return;
  Config c = configSnapshot();
  c.dev1_enabled = server.hasArg("d1_en");
  c.dev2_enabled = server.hasArg("d2_en");
  c.partial_is_on = server.hasArg("part_on");
  c.server1_enabled = server.hasArg("s1_en");
  c.server2_enabled = server.hasArg("s2_en");
  c.mqtt_enabled = server.hasArg("mq_en");
  bool oldStatsEnabled = c.stats_enabled;
  c.ha_discovery = server.hasArg("ha_disc");
  c.stats_enabled = server.hasArg("stats_en");
  argIp("d1_ip", c.dev1_ip);
  argIp("d2_ip", c.dev2_ip);
  argInt("chk_int", 2, 3600, c.check_interval);
  argInt("p_cnt", 1, 5, c.ping_count);
  argInt("p_to", 100, 3000, c.ping_timeout);
  argInt("c_off", 1, 20, c.confirm_off_cycles);
  argInt("c_on", 1, 20, c.confirm_on_cycles);
  argInt("hb_int", 10, 86400, c.heartbeat_interval);
  argUrl("s_url", c.oracle_url, 128);
  argUrl("b_url", c.backup_url, 128);
  argTok("s_tok", c.oracle_token, 128);
  argTok("b_tok", c.backup_token, 128);
  argStr("mq_host", c.mqtt_host, sizeof c.mqtt_host);
  argInt("mq_port", 1, 65535, c.mqtt_port);
  argInt("mq_int", 30, 86400, c.mqtt_interval);
  argStr("mq_user", c.mqtt_user, sizeof c.mqtt_user);
  argTok("mq_pass", c.mqtt_pass, sizeof c.mqtt_pass);
  argStr("mq_cid", c.mqtt_client_id, sizeof c.mqtt_client_id, false);
  argStr("mq_base", c.mqtt_base_topic, sizeof c.mqtt_base_topic, false);
  String p = server.arg("adm_pass");
  if (p.length() >= 6 && p.length() < 64) strlcpy(c.admin_pass, p.c_str(), 64);
  validateConfig(c);
  xSemaphoreTake(mx, portMAX_DELAY);
  cfg = c;
  cfg_ver++;
  xSemaphoreGive(mx);
  saveConfig(c);
  if (oldStatsEnabled != c.stats_enabled) {
    // Toggling collection never deletes accumulated values. An outage already in progress
    // when collection is enabled is deliberately not counted retroactively.
    //Preferences st;
    //st.begin("pm-stats", false);
    g_stats_off_open = false;
    //  st.end();
  }
  if (mqtt.connected()) mqtt.disconnect();
  mqtt_last_try = millis() - 10000UL;
  mqtt_last_pub = millis();
  mqtt_last_power = 0xFF;
  server.sendHeader("Location", "/settings", true);
  server.send(303);
}

// Кнопка «Проверить MQTT»: пробное подключение с введёнными в форму значениями (если пароль пуст —
// берётся сохранённый). Отвечает JSON {ok, state, message}.
void handleMqttTest() {
  if (!checkAuth()) return;
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"ok\":false,\"message\":\"Wi-Fi не подключён\"}");
    return;
  }
  char host[96] = "", user[64] = "", pass[96] = "", cid[64] = DEVICE_ID, base[96] = "power-monitor";
  int port = 1883;
  argStr("mq_host", host, sizeof host);
  argInt("mq_port", 1, 65535, port);
  argStr("mq_user", user, sizeof user);
  argStr("mq_pass", pass, sizeof pass);
  argStr("mq_cid", cid, sizeof cid, false);
  argStr("mq_base", base, sizeof base, false);
  // If password field is blank, use the already saved password for a realistic test.
  if (!pass[0]) {
    Config c = configSnapshot();
    strlcpy(pass, c.mqtt_pass, sizeof pass);
  }
  if (!host[0]) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"Укажите MQTT broker\"}");
    return;
  }
  WiFiClient net;
  PubSubClient test(net);
  test.setServer(host, (uint16_t)port);
  test.setSocketTimeout(2);
  test.setKeepAlive(15);
  String testId = String(cid) + "-test";
  bool ok = user[0] ? test.connect(testId.c_str(), user, pass) : test.connect(testId.c_str());
  int st = test.state();
  if (ok) test.disconnect();
  String out = String("{\"ok\":") + (ok ? "true" : "false") + ",\"state\":" + st + ",\"message\":\"" + (ok ? "Подключение успешно" : mqttStateText(st)) + "\"}";
  server.send(ok ? 200 : 503, "application/json", out);
}


// =============================================================================
// [16] СТРАНИЦА «СИСТЕМА» И СИСТЕМНЫЕ ДЕЙСТВИЯ
// Страница /system: сведения об устройстве и памяти, зона выбора файла прошивки (OTA, форма на
// /update),
// сброс статистики, перезагрузка и сброс настроек. Верхняя часть формируется через snprintf (буфер
// b[1000]
// — при добавлении строк проверяйте длину), нижняя — статический HTML.
// Действия (POST): /reboot — перезагрузка; /stats/reset — стереть статистику; /factory — стереть
// настройки
// (статистика остаётся). Перезагрузка выполняется не сразу, а через restart_at в loop() — чтобы
//    браузер
// успел получить ответ. Страницы «Перезагрузка/Обновление завершено» ждут запуска платы и сами
// открывают /.
// =============================================================================
static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt WDT";
    case ESP_RST_TASK_WDT: return "Task WDT";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    default: return "Other";
  }
}
// Страница «Система» (адрес /system). Подписи Chip/Firmware/Free heap и т. д. переводятся таблицей
// R (раздел [13]).
void handleSystem() {
  if (!checkAuth()) return;
  char tm[12];
  fmtTime(tm, sizeof tm);
  String s = head("Система · Power Monitor", "system");
  char b[1000];
  char ip[16];
  ipStr(ip);
  snprintf(b, sizeof b, "<div class=grid><div class=cg><section class=card><div class=title>Устройство</div><div class=kv><div>Chip</div><b>ESP32-C3</b><div>Firmware</div><b>%s</b><div>Build</div><b>%s</b><div>Reset reason</div><b>%s</b><div>Время</div><b>%s</b></div></section><section class=card><div class=title>Runtime</div><div class=kv><div>Uptime</div><b>%lu мин</b><div>Free heap</div><b>%u KB</b><div>Minimum heap</div><b>%u KB</b><div>Largest block</div><b>%u KB</b><div>RSSI</div><b>%d dBm</b><div>IP</div><b>%s</b></div></section></div>", FW_VERSION, FW_BUILD, resetReason(), tm, (unsigned long)(millis() / 60000), (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024), WiFi.RSSI(), ip);
  s += b;
  s += F("<section class='card wide'><h2>Обновление firmware</h2><form id=ota method=POST action=/update enctype=multipart/form-data><label class=drop><input id=fw type=file name=update accept=.bin required><span class=di><svg width=22 height=22 viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2 stroke-linecap=round stroke-linejoin=round><path d='M12 16V4M7 9l5-5 5 5M4 20h16' /></svg></span><span class=dt><b>Файл прошивки .bin</b><span class=sub id=fn>Файл не выбран</span></span><span class='btn secondary dbtn'>Выбрать</span></label><div class=hint>Не отключайте питание и не закрывайте страницу, пока идёт обновление. После установки устройство перезагрузится.</div><button class=btn id=otab>Загрузить и обновить</button></form></section><script>(function(){var i=document.getElementById('fw'),n=document.getElementById('fn'),d=i.parentNode,b=document.getElementById('otab');i.onchange=function(){var f=i.files[0];n.textContent=f?f.name+' · '+(f.size/1024|0)+' KB':'Файл не выбран';d.classList.toggle('has',!!f)};['dragenter','dragover'].forEach(function(e){i.addEventListener(e,function(){d.classList.add('dz')})});['dragleave','drop'].forEach(function(e){i.addEventListener(e,function(){d.classList.remove('dz')})});document.getElementById('ota').onsubmit=function(){b.disabled=true;b.textContent='Загрузка...'}})()</script><section class='card wide'><h2>Статистика электропитания</h2><p class=muted>Удаляет только накопленную историю отключений. Настройки устройства и текущее состояние POWER не изменяются.</p><form action=/stats/reset method=POST style='display:inline' onsubmit=\"return confirm('Сбросить статистику электропитания? Настройки устройства останутся без изменений.')\"><button class='btn secondary'>Сбросить статистику</button></form></section><section class='card wide dangerzone'><h2>Опасная зона</h2><p class=muted>Перезагрузка сохраняет настройки. Сброс настроек удалит конфигурацию мониторинга, серверов и пароль администратора, но статистику электропитания сохранит.</p><form action=/reboot method=POST style='display:inline'><button class='btn secondary'>Перезагрузить</button></form> <form action=/factory method=POST style='display:inline' onsubmit=\"return confirm('Удалить настройки устройства и перезагрузить? Статистика электропитания будет сохранена.')\"><button class='btn danger'>Сбросить настройки</button></form></section></div>");
  sendPage(s);
}

static const char RST_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="ru"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Power Monitor — )HTML";
static const char RST_CSS[] PROGMEM = R"HTML(</title><style>*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;background:#0f172a;color:#e2e8f0;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}.card{width:100%;max-width:420px;padding:32px 24px;text-align:center;background:#1e293b;border:1px solid #334155;border-radius:18px;box-shadow:0 16px 40px rgba(0,0,0,.25)}.icon{width:68px;height:68px;margin:0 auto 20px;display:flex;align-items:center;justify-content:center;border-radius:50%}h2{margin:0 0 12px;font-size:22px}p{margin:8px 0;color:#94a3b8;line-height:1.5}.status{margin-top:22px;color:#cbd5e1;font-size:14px}.dot{display:inline-block;width:8px;height:8px;margin-right:7px;border-radius:50%;background:#f59e0b;animation:pulse 1s infinite alternate}@keyframes pulse{from{opacity:.3}to{opacity:1}}</style></head><body><div class="card"><div class="icon" style=")HTML";
static const char RST_TAIL[] PROGMEM = R"HTML(<div class="status"><span class="dot"></span><span id="status">Ожидание устройства...</span></div></div><script>(function(){const L=localStorage.getItem("pm_lang")||"ru",M={ua:{"Перезагрузка":"Перезавантаження","Power Monitor перезагружается.":"Power Monitor перезавантажується.","Обновление завершено":"Оновлення завершено","Прошивка успешно установлена.":"Прошивку успішно встановлено.","Ожидание устройства...":"Очікування пристрою...","Power Monitor запущен. Открываю...":"Power Monitor запущено. Відкриваю..."},en:{"Перезагрузка":"Rebooting","Power Monitor перезагружается.":"Power Monitor is rebooting.","Обновление завершено":"Update complete","Прошивка успешно установлена.":"Firmware installed successfully.","Ожидание устройства...":"Waiting for device...","Power Monitor запущен. Открываю...":"Power Monitor is up. Opening..."}};function tt(x){return M[L]&&M[L][x]||x}document.documentElement.lang=L==="ua"?"uk":L;document.querySelectorAll("h2,p,#status").forEach(function(e){e.textContent=tt(e.textContent)});const status=document.getElementById("status");async function checkDevice(){let timer;try{const controller=new AbortController();timer=setTimeout(function(){controller.abort();},1200);const response=await fetch("/api/status?t="+Date.now(),{method:"GET",cache:"no-store",signal:controller.signal});clearTimeout(timer);if(response.ok){status.textContent=tt("Power Monitor запущен. Открываю...");setTimeout(function(){window.location.replace("/");},500);return;}}catch(e){if(timer)clearTimeout(timer);}setTimeout(checkDevice,1500);}setTimeout(checkDevice,2500);})();</script></body></html>)HTML";
// Собирает и отправляет страницу «перезагрузка / обновление»: шаблон RST_HEAD + RST_CSS + RST_TAIL
// (RST_TAIL содержит скрипт
// ожидания запуска платы и перевод). Параметры: title — заголовок вкладки, style — цвет значка,
// icon — символ, h — заголовок, body — пояснение.
static void sendRestartPage(const char* title, const char* style, const char* icon, const char* h, const char* body) {
  String s;
  s.reserve(2400);
  s += FPSTR(RST_HEAD);
  s += title;
  s += FPSTR(RST_CSS);
  s += style;
  s += F("\">");
  s += icon;
  s += F("</div><h2>");
  s += h;
  s += F("</h2>");
  s += body;
  s += FPSTR(RST_TAIL);
  server.sendHeader("Connection", "close");
  server.send(200, "text/html; charset=utf-8", s);
}

// POST /reboot: показывает страницу ожидания и планирует перезагрузку через ~0,8 с.
void handleReboot() {
  if (!checkAuth()) return;
  sendRestartPage("Reboot", "background:#1e3a5f;color:#60a5fa;font-size:34px", "&#8635;", "Перезагрузка", "<p>Power Monitor перезагружается.</p>");
  // Перезагрузка выполняется из loop(), когда HTTP-ответ уже отправлен браузеру.
  restart_at = (millis() + 800) | 1;
}

// POST /stats/reset: стирает всю статистику (флеш и переменные в памяти); настройки не меняются.
void handleResetStats() {
  if (!checkAuth()) return;
  Preferences st;
  st.begin("pm-stats", false);
  st.remove("outages");
  st.remove("last_off");
  st.remove("last_on");
  st.remove("last_dur");
  st.remove("total_dur");
  st.remove("max_dur");
  st.end();

  g_outage_count = 0;
  g_last_power_off = 0;
  g_last_power_on = 0;
  g_last_outage_duration = 0;
  g_total_outage_duration = 0;
  g_max_outage_duration = 0;
  g_stats_off_open = false;

  server.sendHeader("Location", "/system", true);
  server.send(303);
}

// POST /factory: стирает ВСЕ настройки (пространство pm-config) и перезагружает плату; статистика
// остаётся. После этого действуют значения по умолчанию из loadConfig() и пароль
// DEFAULT_ADMIN_PASS.
void handleFactory() {
  if (!checkAuth()) return;
  prefs.begin("pm-config", false);
  prefs.clear();
  prefs.end();
  server.send(200, "text/plain; charset=utf-8", "Сброс выполнен, перезагрузка...");
  restart_at = (millis() + 500) | 1;
}


// =============================================================================
// [17] OTA-ОБНОВЛЕНИЕ ПРОШИВКИ
// Форма на странице «Система» отправляет файл .bin на /update. otaUpload() принимает файл кусками
// и записывает его в память прошивки (только после проверки пароля), otaDone() выдаёт итоговую
// страницу и планирует перезагрузку. При ошибке плата продолжает работать на прежней прошивке.
// =============================================================================
// ---------- OTA ----------
// Приём файла по частям: START — проверка пароля и подготовка, WRITE — запись куска, END —
// завершение, ABORTED — отмена.
static void otaUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    ota_ok = authOk();
    if (ota_ok) {
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        ota_ok = false;
        Update.printError(Serial);
      }
    }
  } else if (!ota_ok) return;
  else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      ota_ok = false;
      Update.printError(Serial);
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      ota_ok = false;
      Update.printError(Serial);
    } else if (!Update.isFinished()) ota_ok = false;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    ota_ok = false;
    Update.abort();
  }
}

// Итог загрузки: успех -> страница «Обновление завершено» и перезагрузка; ошибка -> страница с
// ошибкой (код 500).
static void otaDone() {
  if (!authOk()) {
    server.requestAuthentication();
    return;
  }
  bool ok = ota_ok && !Update.hasError() && Update.isFinished();
  if (ok) {
    sendRestartPage("OTA", "background:#064e3b;color:#6ee7b7;font-size:36px;font-weight:700", "&#10003;", "Обновление завершено", "<p>Прошивка успешно установлена.</p><p>Power Monitor перезагружается.</p>");
    restart_at = (millis() + 800) | 1;
  } else {
    server.sendHeader("Connection", "close");
    server.send(500, "text/html; charset=utf-8", "<!DOCTYPE html><html lang=ru><head><meta charset=UTF-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Power Monitor — OTA Error</title></head><body><h2>Ошибка обновления</h2><p id=m>Прошивка не была установлена.</p><p><a href=/system>Вернуться в систему</a></p><script>const L=localStorage.getItem('pm_lang')||'ru',M={ua:{'Ошибка обновления':'Помилка оновлення','Прошивка не была установлена.':'Прошивку не встановлено.','Вернуться в систему':'Повернутися до системи'},en:{'Ошибка обновления':'Update error','Прошивка не была установлена.':'Firmware was not installed.','Вернуться в систему':'Back to System'}};document.documentElement.lang=L==='ua'?'uk':L;document.querySelectorAll('h2,#m,a').forEach(e=>{e.textContent=M[L]&&M[L][e.textContent]||e.textContent})</script></body></html>");
  }
  ota_ok = false;
}


// =============================================================================
// [18] СОБЫТИЯ WI-FI
// Обновляет флаг gotIp (есть ли IP-адрес — условие работы monitorTask и сервисов) и счётчик
// обрывов.
// =============================================================================
static void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    gotIp = true;
    dbgLog("WiFi", "GOT_IP %s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    dbgLog("WiFi", "CONNECTED");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    ++g_wifi_disconnects;
    gotIp = false;
    dbgLog("WiFi", "DISCONNECTED count=%lu", (unsigned long)g_wifi_disconnects);
  }
}


// =============================================================================
// [19] setup() — ЗАПУСК И ТАБЛИЦА АДРЕСОВ
// Порядок: Serial и лог -> мьютекс -> loadConfig() и loadPowerStats() -> настройка Wi-Fi (мощность
// передатчика 8,5 dBm — для стабильности на ESP32-C3 Super Mini) и подключение -> таблица адресов
// веб-сервера -> запуск monitorTask. Если мьютекс или задача не создались — перезагрузка.
// Адреса:  /  /settings  /system — страницы;  /api/status  /api/config — данные (GET);
//   /save  /mqtt/test  /reboot  /factory  /stats/reset  /update — действия (POST).
// Новый адрес добавляется строкой server.on("/путь", HTTP_GET|HTTP_POST, обработчик).
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  dbgLog("BOOT", "ESP32-C3 Power Monitor " FW_VERSION);
  dbgLog("BOOT", "Build: %s", FW_BUILD);
  dbgLog("BOOT", "Reset: %s", resetReason());
  dbgLog("BOOT", "heap=%u min_heap=%u flash=%u cpu=%uMHz", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getCpuFreqMHz());
  mx = xSemaphoreCreateMutex();
  if (!mx) {
    dbgLog("FATAL", "Mutex creation failed");
    delay(1000);
    ESP.restart();
  }
  loadConfig();
  loadPowerStats();
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("power-monitor");
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  // ESP32-C3 Super Mini: reduced TX power for Wi-Fi stability.
  // Same value as the known-good wifi_test firmware.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  WiFi.onEvent(onWiFiEvent);
  dbgLog("WiFi", "TX power set to 8.5 dBm");
  dbgLog("WiFi", "Connecting to configured SSID");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/system", HTTP_GET, handleSystem);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/mqtt/test", HTTP_POST, handleMqttTest);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/factory", HTTP_POST, handleFactory);
  server.on("/stats/reset", HTTP_POST, handleResetStats);
  server.on("/update", HTTP_POST, otaDone, otaUpload);
  server.begin();
  BaseType_t r = xTaskCreate(monitorTask, "monitor", 12288, nullptr, 1, nullptr);
  if (r != pdPASS) {
    dbgLog("FATAL", "Monitor task creation failed");
    delay(1000);
    ESP.restart();
  }
}

// =============================================================================
// [20] loop() — ОСНОВНОЙ ЦИКЛ
// Выполняется постоянно: обслуживает веб-запросы, MQTT, запускает mDNS и синхронизацию времени
// (NTP) после получения IP, раз в минуту пытается переподключить Wi-Fi при обрыве, пишет в лог
// строку HEALTH (раз в минуту) и перезагружает плату, когда наступило время restart_at.
// Не вставляйте сюда долгие операции (delay, ожидание сети): страница перестанет отвечать.
// Долгие задачи выполняются в monitorTask [11].
// =============================================================================
void loop() {
  server.handleClient();
  mqttService();
  if (gotIp && !svcUp) {
    svcUp = true;
    MDNS.begin("power-monitor");
    configTzTime(TZ_UA, "pool.ntp.org", "time.nist.gov");
  }
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t t = 0;
    if ((uint32_t)(millis() - t) > 60000) {
      t = millis();
      WiFi.reconnect();
    }
  }
#if DEBUG_DIAGNOSTICS
  static uint32_t healthAt = 0;
  uint32_t nowMs = millis();
  if (!g_time_log_announced && validEpoch(time(nullptr))) {
    g_time_log_announced = true;
    dbgLog("TIME", "NTP synchronized");
  }
  if ((uint32_t)(nowMs - healthAt) >= HEALTH_INTERVAL_MS) {
    healthAt = nowMs;
    int mqttStateNow = mqtt.connected() ? 0 : mqtt.state();
    dbgLog("HEALTH", "up=%luh%02lum heap=%u min=%u largest=%u rssi=%d wifi=%s mqtt=%s(%d) power=%s age=%lus wifi_disc=%lu mqtt_rec=%lu mqtt_tx=%lu hb_ok=%lu hb_fail=%lu",
           (unsigned long)(nowMs / 3600000UL), (unsigned long)((nowMs / 60000UL) % 60),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
           WiFi.status() == WL_CONNECTED ? "OK" : "DOWN",
           mqtt.connected() ? "OK" : "DOWN", mqttStateNow,
           g_state == ST_ON ? "ON" : g_state == ST_OFF ? "OFF" : "UNKNOWN",
           (unsigned long)currentPowerAge(), (unsigned long)g_wifi_disconnects,
           (unsigned long)g_mqtt_reconnects, (unsigned long)g_mqtt_tx_count,
           (unsigned long)g_hb_ok_count, (unsigned long)g_hb_fail_count);
  }
#endif
  if (restart_at && (int32_t)(millis() - restart_at) >= 0) ESP.restart();
  delay(2);
}
