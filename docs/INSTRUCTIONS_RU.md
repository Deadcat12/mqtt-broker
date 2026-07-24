# Инструкция по сборке, запуску и использованию

## 1. Требования

Минимальная среда:

```bash
gcc
make
python3
```

Для ручной проверки через стандартные MQTT-клиенты удобно установить пакет `mosquitto-clients`:

```bash
sudo apt update
sudo apt install build-essential mosquitto-clients
```

На Fedora/RHEL-подобных системах:

```bash
sudo dnf install gcc make mosquitto
```

## 2. Сборка

Основной способ:

```bash
./build.sh
```

После сборки в корне проекта появится исполняемый файл:

```text
./sol-broker
```

Альтернативно можно использовать CMake:

```bash
cmake -S . -B cmake-build
cmake --build cmake-build
./cmake-build/sol-broker 0.0.0.0 1883
```

## 3. Запуск брокера

Формат запуска:

```bash
./sol-broker [host] [port]
```

Примеры:

```bash
./sol-broker
./sol-broker 0.0.0.0 1883
./sol-broker 127.0.0.1 18883
```

По умолчанию используются адрес `0.0.0.0` и порт `1883`.

Остановка: `Ctrl+C` или отправка `SIGTERM`.

## 4. Проверка встроенным интеграционным тестом

Тест запускает брокер на `127.0.0.1:18883`, создает двух MQTT-клиентов, выполняет `CONNECT`, `SUBSCRIBE`, `PUBLISH` и проверяет доставку сообщения подписчику.

```bash
./build.sh
python3 -S tests/integration_test.py
```

Ожидаемый результат:

```text
OK: CONNECT/SUBSCRIBE/PUBLISH tested
```

## 5. Проверка через mosquitto_pub/mosquitto_sub

Терминал 1 — запустить брокер:

```bash
./sol-broker 127.0.0.1 1883
```

Терминал 2 — подписаться на ветку topics:

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -t 'diploma/#' -v
```

Терминал 3 — опубликовать сообщение:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t 'diploma/demo' -m 'hello from diploma'
```

В терминале подписчика должно появиться:

```text
diploma/demo hello from diploma
```

## 6. Что поддерживается

Поддерживаемый минимум MQTT 3.1.1:

```text
CONNECT, CONNACK
SUBSCRIBE, SUBACK
UNSUBSCRIBE, UNSUBACK
PUBLISH
PUBACK, PUBREC, PUBREL, PUBCOMP
PINGREQ, PINGRESP
DISCONNECT
```

Topic filters:

```text
diploma/+/temperature
sensors/#
#
```

Retained messages:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t 'diploma/retained' -m 'saved' -r
mosquitto_sub -h 127.0.0.1 -p 1883 -t 'diploma/#' -v
```

Новый подписчик получит сохраненное retained-сообщение.

## 7. Ограничения учебной версии

- нет TLS;
- нет авторизации и ACL;
- нет постоянного хранения сессий на диске;
- QoS 2 для входящих сообщений закрывается handshake-ответами, но downstream-доставка QoS 2 понижается до QoS 1;
- нет полноценной очереди сообщений для offline-сессий;
- сервер однопроцессный и предназначен для демонстрации архитектуры, а не для высокой нагрузки.

## 8. Где смотреть код

Главные файлы:

```text
src/mqtt.c   — парсинг и сборка MQTT-пакетов, remaining length, topic matching
src/broker.c — TCP-сервер, список клиентов, подписки, retained messages, handlers
src/main.c   — точка входа и аргументы командной строки
```