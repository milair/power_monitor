# ESP32-C3 Power Monitor

**Wi-Fi Power Outage Monitoring System | Система моніторингу електроживлення**

Platform: **ESP32-C3 / Arduino Framework**

---
## 🇺🇦 Про проєкт

**ESP32-C3 Power Monitor** — автономна система моніторингу наявності електроживлення, побудована на мікроконтролері ESP32-C3.

Пристрій визначає наявність або відсутність електроживлення шляхом перевірки доступності мережевих пристроїв через Wi-Fi, фіксує відключення та відновлення живлення, накопичує статистику й передає інформацію зовнішнім системам.

Проєкт розроблено для домашніх мереж, систем резервного живлення, серверного обладнання та інтеграції з розумним будинком.

### Основні можливості

- **Моніторинг живлення:** перевірка доступності одного або двох мережевих пристроїв.
- **Виявлення відключень:** автоматичне визначення втрати та відновлення електроживлення.
- **Статистика:** кількість відключень, тривалість, час останніх подій, максимальна та сумарна тривалість відсутності живлення.
- **Енергоощадний режим:** зменшення частоти перевірок під час підтвердженої відсутності живлення.
- **Вебінтерфейс:** перегляд поточного стану, статистики та налаштувань через браузер.
- **MQTT:** передавання стану живлення, діагностичних показників і статистики.
- **Home Assistant Discovery:** автоматичне створення пристрою та його сутностей у Home Assistant.
- **HTTP/HTTPS Heartbeat:** передавання JSON-повідомлень на основний і резервний сервери.
- **OTA:** оновлення прошивки через Wi-Fi.
- **NVS:** збереження налаштувань і накопиченої статистики в енергонезалежній пам'яті.
- **Діагностика:** інформація про Wi-Fi, MQTT, IP-адресу, час роботи та доступну пам'ять.

## Принцип роботи

ESP32-C3 періодично перевіряє доступність заданих IP-адрес у локальній мережі.

Якщо контрольовані пристрої перестають відповідати, після налаштованого підтвердження система реєструє відсутність електроживлення. Після відновлення доступності фіксується повернення живлення.

Події можуть передаватися через MQTT та HTTP/HTTPS Heartbeat.

**Важливо:** система визначає стан живлення опосередковано, через доступність мережевих пристроїв. Втрата Wi-Fi, мережеві несправності або вимкнення контрольованого обладнання також можуть впливати на результат. Пристрій не вимірює напругу електромережі безпосередньо.

## Режим енергозбереження

Прошивка підтримує спеціальний ECO-режим для роботи від резервного акумуляторного живлення.

Після підтвердження відсутності електроживлення частота мережевих перевірок зменшується, що допомагає знизити активність мікроконтролера та витрати енергії.

Це особливо корисно для компактних пристроїв на базі ESP32-C3 Super Mini з живленням від акумулятора 18650.

## Інтеграція з Home Assistant

Завдяки MQTT Device Discovery пристрій може автоматично з'являтися в Home Assistant.

Передаються поточний стан електроживлення, час роботи, статистика відключень і технічна інформація.

Дані можна використовувати для автоматизацій, сповіщень, журналів подій і панелей моніторингу.

## HTTP/HTTPS Heartbeat

Система підтримує передавання JSON-повідомлень на зовнішні сервери.

Передбачено два сервери з пріоритетом основного та використанням резервного в разі помилки. Зміна стану живлення має пріоритет над періодичним передаванням.

## Налаштування та оновлення

Основні параметри налаштовуються через вебінтерфейс без повторного програмування пристрою.

Підтримується оновлення прошивки OTA через локальну мережу.

---

## 🇬🇧 About the Project

**ESP32-C3 Power Monitor** is a standalone Wi-Fi-based power outage monitoring system built around the ESP32-C3 microcontroller.

The device detects power availability by monitoring the network reachability of selected devices. It records power outages and restoration events, maintains historical statistics, and reports information to external monitoring systems.

The project is designed for home networks, backup power systems, server infrastructure, and smart home integration.

### Key Features

- **Power monitoring:** network reachability checks for one or two devices.
- **Outage detection:** automatic detection of power loss and restoration.
- **Outage statistics:** outage count, duration, timestamps, maximum outage duration, and total downtime.
- **Energy-saving mode:** reduced monitoring frequency during confirmed power outages.
- **Web dashboard:** browser-based status monitoring and configuration.
- **MQTT integration:** power status, statistics, and diagnostic data publishing.
- **Home Assistant Discovery:** automatic device and entity registration.
- **HTTP/HTTPS Heartbeat:** JSON status reporting to primary and backup servers.
- **OTA firmware updates:** wireless firmware upgrades.
- **Persistent storage:** configuration and outage statistics stored in NVS.
- **Diagnostics:** Wi-Fi signal, MQTT status, IP address, uptime, and available memory.

## How It Works

The ESP32-C3 periodically checks the availability of configured network devices using Wi-Fi.

When monitored devices become unreachable, the system confirms the condition according to the configured detection parameters and registers a power outage.

Once connectivity to the monitored devices is restored, the system records a power restoration event.

Status changes can be reported through MQTT and HTTP/HTTPS Heartbeat.

**Important:** Power availability is inferred from network device reachability rather than direct electrical measurements. Wi-Fi failures, network interruptions, or shutdowns of monitored equipment may affect detection accuracy.

## Energy-Saving Mode

The firmware includes an ECO mode intended for battery-backed operation.

During a confirmed power outage, the monitoring interval is increased to reduce network activity and help conserve energy.

This makes the project particularly suitable for compact ESP32-C3 Super Mini boards powered by an external 18650 battery system.

## Home Assistant Integration

MQTT Device Discovery allows the ESP32-C3 Power Monitor to register automatically in Home Assistant.

The integration exposes power status, uptime, outage statistics, and diagnostic information.

These entities can be used in automations, notifications, event logging, and monitoring dashboards.

## HTTP/HTTPS Heartbeat

The firmware supports JSON status reporting to external monitoring servers.

A primary and secondary server can be configured, with priority given to the primary endpoint and fallback to the secondary when needed.

Power state changes are reported with priority over periodic updates.

## Configuration and Firmware Updates

Device settings can be managed through the built-in web interface without reflashing the microcontroller.

Wireless firmware updates are supported through OTA.

---

## Technology Stack

- **Microcontroller:** Espressif ESP32-C3
- **Framework:** Arduino / Arduino-ESP32
- **Connectivity:** Wi-Fi 2.4 GHz
- **Protocols:** MQTT, HTTP, HTTPS
- **Smart Home:** Home Assistant MQTT Device Discovery
- **Storage:** ESP32 NVS / Preferences
- **Firmware Updates:** OTA
- **Web Interface:** Embedded HTTP server
- **Execution:** Arduino + FreeRTOS

## Hardware

Recommended development board: **ESP32-C3 Super Mini**.

For battery-backed operation, a suitable regulated power supply and an external battery system are required. Battery protection and charging circuitry must be selected according to the actual hardware configuration.

## License

License information should be provided in the repository's `LICENSE` file.

---

**ESP32-C3 Power Monitor — Smart Power Monitoring, Outage Detection & Home Assistant Integration.**
