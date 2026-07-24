# Перевод и полный технический конспект материалов «MQTT broker from scratch»

Документ объединяет шесть исходных частей в один русский конспект для практической части дипломной работы. Сохранены все ключевые технические идеи: назначение брокера, MQTT-пакеты, кодирование Remaining Length, упаковка/распаковка сообщений, сетевой сервер, callbacks/handlers, структуры данных, topics, wildcard-подписки и QoS-обработка. Исходные англоязычные тексты без изменений лежат в каталоге `docs/original/`.

## 1. Общая идея проекта

Проект называется `Sol`. Это минимальный MQTT-брокер на языке C под Linux. Он ориентирован на MQTT версии 3.1.1 и задуман как учебный аналог легковесного брокера Mosquitto. Автор исходного материала подчеркивает, что проект является MVP: брокер должен быть работоспособным, но в нем остается место для оптимизации, улучшения качества кода, расширения функциональности и исправления потенциальных ошибок.

Серия материалов делится на шесть частей:

1. `Protocol` — основы MQTT-протокола и модели пакетов.
2. `Networking` — сетевые функции, отправка и прием байтов.
3. `Server` — главный серверный цикл и callbacks.
4. `Data structures` — вспомогательные структуры данных: hash table, list, trie.
5. `Topic abstraction` — topic как ключевая абстракция MQTT и trie для поиска по префиксам.
6. `Handlers` — обработчики команд MQTT и завершение серверной логики.

Брокер по смыслу является middleware: он принимает сообщения от клиентов-производителей и пересылает их клиентам-потребителям. Группировка выполняется через topic — строковую метку, похожую на канал в чате или IRC. Клиент подписывается на topic filter, а брокер доставляет ему публикации, topic name которых соответствует этому фильтру.

Архитектурно сервер состоит из четырех крупных слоев:

```text
TCP socket layer
    ↓
MQTT binary protocol parser/packer
    ↓
Broker state: clients, sessions, subscriptions, topics
    ↓
Handlers: CONNECT, SUBSCRIBE, PUBLISH, PINGREQ, DISCONNECT, ACK packets
```

## 2. MQTT-протокол и структура пакета

Каждый MQTT Control Packet состоит из трех частей:

```text
Fixed Header      — обязательный
Variable Header   — необязательный, зависит от типа пакета
Payload           — необязательный, зависит от типа пакета
```

Fixed Header начинается с первого байта, в котором старшие четыре бита задают тип MQTT-пакета, а младшие четыре бита используются как flags. Далее идет поле `Remaining Length`, занимающее от 1 до 4 байтов.

Схематично:

```text
Byte 1: bits 7..4 = MQTT Control Packet Type
        bits 3..0 = flags
Byte 2..5: Remaining Length
```

Основные flags:

- `DUP` — повторная отправка сообщения;
- `QoS` — уровень качества доставки: 0, 1 или 2;
- `RETAIN` — брокер должен сохранить публикацию как retained message и отправлять ее новым подписчикам.

Типы MQTT-пакетов:

```c
enum packet_type {
    CONNECT     = 1,
    CONNACK     = 2,
    PUBLISH     = 3,
    PUBACK      = 4,
    PUBREC      = 5,
    PUBREL      = 6,
    PUBCOMP     = 7,
    SUBSCRIBE   = 8,
    SUBACK      = 9,
    UNSUBSCRIBE = 10,
    UNSUBACK    = 11,
    PINGREQ     = 12,
    PINGRESP    = 13,
    DISCONNECT  = 14
};
```

Уровни QoS:

```text
QoS 0 — at most once: сообщение отправляется без подтверждения.
QoS 1 — at least once: получатель подтверждает сообщение PUBACK.
QoS 2 — exactly once: используется цепочка PUBREC → PUBREL → PUBCOMP.
```

В исходной реализации первый байт удобно представлен через `union` и bit fields:

```c
union mqtt_header {
    unsigned char byte;
    struct {
        unsigned retain : 1;
        unsigned qos : 2;
        unsigned dup : 1;
        unsigned type : 4;
    } bits;
};
```

Такая модель позволяет обращаться и ко всему байту целиком, и к отдельным битовым полям.

## 3. Remaining Length

`Remaining Length` — это количество байтов, оставшихся в текущем MQTT-пакете после самого поля Remaining Length. В расчет входят Variable Header и Payload, но не входят первый байт Fixed Header и байты кодирования длины.

Поле кодируется переменной длиной. В каждом байте младшие 7 бит несут данные, а старший бит показывает, есть ли следующий байт. Максимум — четыре байта.

Алгоритм кодирования:

```c
int mqtt_encode_length(unsigned char *buf, size_t len) {
    int bytes = 0;
    do {
        short d = len % 128;
        len /= 128;
        if (len > 0)
            d |= 128;
        buf[bytes++] = d;
    } while (len > 0);
    return bytes;
}
```

Алгоритм декодирования:

```c
unsigned long long mqtt_decode_length(const unsigned char **buf) {
    char c;
    int multiplier = 1;
    unsigned long long value = 0LL;
    do {
        c = **buf;
        value += (c & 127) * multiplier;
        multiplier *= 128;
        (*buf)++;
    } while ((c & 128) != 0);
    return value;
}
```

В практическом коде из архива эти функции находятся в `src/mqtt.c`:

```c
int mqtt_encode_remaining_length(uint8_t *out, size_t len);
int mqtt_decode_remaining_length(const uint8_t *buf, size_t buflen,
                                 size_t *value, size_t *used);
```

## 4. CONNECT и CONNACK

`CONNECT` — первый MQTT-пакет, который клиент обязан отправить после установления TCP-соединения. Для одного TCP-соединения допускается ровно один CONNECT. Повторный CONNECT считается нарушением протокола; такой клиент должен быть отключен.

Variable Header для MQTT 3.1.1 содержит:

```text
Protocol Name   — строка "MQTT"
Protocol Level  — 4
Connect Flags   — username/password/will/clean session
Keep Alive      — 16-битное значение
```

Payload содержит Client ID и, в зависимости от flags, может содержать Will Topic, Will Message, Username и Password. Например, если flags `username` и `password` установлены, в payload после Client ID идут поля длины username/password и соответствующие байты строк.

В ответ брокер отправляет `CONNACK`:

```text
Byte 1: 0x20
Byte 2: 0x02
Byte 3: connect acknowledge flags, session present
Byte 4: return code, 0 = connection accepted
```

В коде архива это реализовано функцией:

```c
uint8_t *mqtt_build_connack(uint8_t session_present,
                            uint8_t return_code,
                            size_t *out_len);
```

и обработчиком в `src/broker.c`:

```c
static int handle_connect(broker_state *state,
                          client *c,
                          const mqtt_connect_packet *conn);
```

## 5. SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK

`SUBSCRIBE` содержит Packet Identifier и список пар:

```text
Topic Filter + Requested QoS
```

В ответ брокер отправляет `SUBACK` с тем же Packet Identifier и списком return codes в том же порядке, что и входные topic filters. Если подписка принята, return code равен согласованному QoS. Если rejected — `0x80`.

В исходной статье подчеркивается, что `SUBSCRIBE` — пакет со специальным ответом `SUBACK`, тогда как многие другие подтверждения можно представить общей ACK-структурой:

```c
struct mqtt_ack {
    union mqtt_header header;
    unsigned short pkt_id;
};
```

`UNSUBSCRIBE` удаляет подписки клиента и получает ответ `UNSUBACK`.

В практическом коде:

```c
static int handle_subscribe(broker_state *state,
                            client *c,
                            const mqtt_subscribe_packet *sub);

static int handle_unsubscribe(client *c,
                              const mqtt_unsubscribe_packet *unsub);
```

## 6. PUBLISH и QoS

`PUBLISH` содержит:

```text
Fixed Header: type, DUP, QoS, RETAIN
Variable Header: Topic Name, Packet Identifier при QoS > 0
Payload: полезная нагрузка сообщения
```

Логика обработки:

1. Проверить, что topic name не содержит wildcard-символов `+` и `#`.
2. Если установлен retain-флаг, сохранить сообщение в памяти брокера.
3. Найти всех клиентов, подписки которых соответствуют topic name.
4. Отправить сообщение подписчикам.
5. При входящем QoS 1 отправить `PUBACK` отправителю.
6. При входящем QoS 2 отправить `PUBREC`, затем на `PUBREL` ответить `PUBCOMP`.

В исходной статье также описан принцип выбора QoS при пересылке: доставочный QoS не должен превышать QoS подписки клиента. Поэтому применяется минимум между QoS публикации и QoS подписки.

В коде архива:

```c
static int handle_publish(broker_state *state,
                          client *c,
                          const mqtt_publish_packet *pub);
```

Пересылка подписчикам:

```c
static void publish_forward(broker_state *state,
                            const mqtt_publish_packet *pub);
```

## 7. Сетевой слой

Во второй части исходного материала вводится сетевой модуль. Его задача — скрыть низкоуровневые детали работы с сокетами.

Основные функции исходного варианта:

```c
int set_nonblocking(int);
int set_tcp_nodelay(int);
int create_and_bind(const char *, const char *, int);
int make_listen(const char *, const char *, int);
int accept_connection(int);
ssize_t send_bytes(int, const unsigned char *, size_t);
ssize_t recv_bytes(int, unsigned char *, size_t);
```

`send_bytes` отправляет все байты в цикле, пока не будет записан весь буфер. Это нужно, потому что `send()` не обязан отправлять все данные за один вызов.

`recv_bytes` аналогично читает нужное количество байтов и корректно обрабатывает частичные чтения.

В исходной архитектуре сервер планировался как single-threaded multiplexing I/O server на `epoll`. Такой подход позволяет обслуживать много клиентов в одном потоке без создания отдельного потока на каждое соединение. В практическом архиве используется `select()` как более компактный переносимый вариант мультиплексирования, но логика та же: слушающий сокет и клиентские сокеты проверяются на готовность к чтению, после чего вызывается соответствующая обработка.

## 8. Серверный цикл и callbacks

В третьей части описывается серверный модуль. В заголовке объявляется функция:

```c
int start_server(const char *, const char *);
```

Также задаются константы для epoll:

```c
#define EPOLL_MAX_EVENTS    256
#define EPOLL_TIMEOUT       -1
```

И коды результата handler-функций:

```c
#define REARM_R 0
#define REARM_W 1
```

Исходная модель callbacks:

```text
on_accept — принять нового TCP-клиента
on_read   — прочитать входящий MQTT-пакет и вызвать handler
on_write  — отправить подготовленный ответ клиенту
```

Функция `recv_packet` сначала читает первый байт Fixed Header, затем байты Remaining Length, декодирует длину, проверяет лимит размера пакета и дочитывает оставшееся тело. После этого пакет можно распаковать в структуру `union mqtt_packet` и передать нужному обработчику.

В текущем архиве та же идея реализована через:

```c
static uint8_t *recv_mqtt_packet(int fd, size_t *out_len);
static int handle_packet(broker_state *state, client *c, mqtt_packet *packet);
```

`handle_packet` выполняет dispatch по типу MQTT-пакета.

## 9. Вспомогательная bytestring-структура

В исходном материале для отправки данных вводится структура `bytestring`:

```c
struct bytestring {
    size_t size;
    size_t last;
    unsigned char *data;
};
```

Она нужна, чтобы хранить указатель на массив байтов и его размер. Это упрощает `on_write`, потому что функция знает, сколько байтов нужно отправить через socket.

В практическом коде архива вместо отдельной структуры используется пара `uint8_t *buf` + `size_t len`, которая передается в `send_packet()`:

```c
static int send_packet(client *c, uint8_t *buf, size_t len);
```

## 10. Структуры данных: hash table

В четвертой части создается hash table. Hash table хранит пары `key → value`, где key — строка, а value — `void *`. Используется массив buckets и hashing-функция, которая преобразует строковый ключ в индекс массива.

Структура entry:

```c
struct hashtable_entry {
    const char *key;
    void *val;
    bool taken;
};
```

API:

```c
HashTable *hashtable_create(int (*destructor)(struct hashtable_entry *));
void hashtable_release(HashTable *);
size_t hashtable_size(const HashTable *);
int hashtable_exists(HashTable *, const char *);
int hashtable_put(HashTable *, const char *, void *);
void *hashtable_get(HashTable *, const char *);
int hashtable_del(HashTable *, const char *);
int hashtable_map(HashTable *, int (*func)(struct hashtable_entry *));
int hashtable_map2(HashTable *, int (*func)(struct hashtable_entry *, void *), void *);
```

Внутренняя структура спрятана в `.c`-файле, что создает подобие инкапсуляции: пользователь модуля работает только через функции API.

Для hashing используется комбинация CRC32, Robert Jenkins mix function и Knuth multiplicative method. Для разрешения коллизий применяется linear probing. При заполнении таблицы выполняется rehash: создается массив большего размера, и все элементы вставляются заново.

## 11. Структуры данных: linked list

Также нужна связная структура для хранения подписчиков, subscriptions и дочерних узлов trie. Используется singly-linked list с указателями на head и tail, что дает O(1) вставку в начало или конец.

Базовая структура:

```c
struct list_node {
    void *data;
    struct list_node *next;
};

typedef struct list {
    struct list_node *head;
    struct list_node *tail;
    unsigned long len;
    int (*destructor)(struct list_node *);
} List;
```

API включает создание, освобождение, очистку, вставку в начало/конец, удаление узла, сортированную вставку и разбиение списка пополам для merge sort.

## 12. Topic abstraction

Topic — ключевая абстракция MQTT. Это UTF-8 строка длиной до 65535 байтов. Уровни разделяются символом `/`, как директории в файловой системе.

Wildcard-символы:

```text
# — multi-level wildcard, должен быть последним уровнем
+ — single-level wildcard, заменяет один уровень
```

Примеры:

```text
foo/bar/# подходит к:
foo/bar
foo/bar/baz
foo/bar/bat/yop

foo/+/baz подходит к:
foo/bar/baz
foo/zod/baz
foo/nop/baz
```

В практическом коде проверка topic filters и matching реализованы в `src/mqtt.c`:

```c
bool mqtt_topic_filter_is_valid(const char *filter);
bool mqtt_topic_name_is_valid(const char *topic);
bool mqtt_topic_matches(const char *filter, const char *topic);
```

## 13. Trie для хранения topics

Пятая часть исходного материала предлагает хранить topics в trie. Trie — дерево, где каждый узел соответствует символу/префиксу ключа. Значение хранится в конечном узле ключа.

Преимущество trie: поиск, вставка и удаление имеют сложность O(m), где m — длина ключа. Также удобно выполнять prefix scan, что важно для wildcard-подписок и иерархических topics.

Недостаток классического trie с массивом детей фиксированного размера — большой расход памяти. Если в каждом узле хранить массив на 94 или 96 возможных символов, большинство указателей будет NULL. Поэтому исходный материал рассматривает альтернативы:

1. общий dynamic vector и индексы детей;
2. adaptive/sized nodes в зависимости от числа детей;
3. linked list детей в каждом узле.

В исходной серии выбран третий вариант: у каждого trie node есть список детей.

Узел trie:

```c
struct trie_node {
    char chr;
    List *children;
    void *data;
};
```

Trie:

```c
struct trie {
    struct trie_node *root;
    size_t size;
};
```

API:

```c
struct trie_node *trie_create_node(char);
struct trie *trie_create(void);
void trie_init(Trie *);
size_t trie_size(const Trie *);
void *trie_insert(Trie *, const char *, const void *);
bool trie_delete(Trie *, const char *);
bool trie_find(const Trie *, const char *, void **);
void trie_release(Trie *);
void trie_prefix_delete(Trie *, const char *);
void trie_prefix_map_tuple(Trie *, const char *,
                           void (*mapfunc)(struct trie_node *, void *), void *);
```

В практическом коде архива вместо полного trie используется список подписок у каждого клиента и функция `mqtt_topic_matches`. Для учебной демонстрации это проще и достаточно, но в разделе диплома можно указать, что trie является направлением оптимизации для быстрого поиска подписчиков по topic prefix.

## 14. Core abstractions

В шестой части добавляется модуль `core.h`, где описаны главные сущности брокера:

```c
struct topic {
    const char *name;
    List *subscribers;
};

struct sol {
    HashTable *clients;
    HashTable *closures;
    Trie topics;
};

struct session {
    List *subscriptions;
};

struct sol_client {
    char *client_id;
    int fd;
    struct session session;
};

struct subscriber {
    unsigned qos;
    struct sol_client *client;
};
```

Смысл сущностей:

- `sol_client` — подключенный MQTT-клиент;
- `topic` — MQTT topic и список подписчиков;
- `subscriber` — связка клиента и QoS подписки;
- `session` — состояние клиента, в перспективе нужно для clean session false;
- `sol` — глобальное состояние брокера: clients, closures, topics.

В архивном коде эти роли упрощены и сосредоточены в `broker_state`, `client`, `subscription_node`, `retained_message`.

## 15. Handlers

Handlers — это функции, которые вызываются после распаковки MQTT-пакета. В исходной архитектуре они хранятся в массиве, индексированном типом MQTT-команды:

```c
static handler *handlers[15] = {
    NULL,
    connect_handler,
    NULL,
    publish_handler,
    puback_handler,
    pubrec_handler,
    pubrel_handler,
    pubcomp_handler,
    subscribe_handler,
    NULL,
    unsubscribe_handler,
    NULL,
    pingreq_handler,
    NULL,
    disconnect_handler
};
```

Результат handler-а определяет, что делать с socket:

```text
REARM_W   — есть данные для записи клиенту;
REARM_R   — ответа нет, снова ждать чтения;
-REARM_W  — клиент отключился или должен быть отключен.
```

В архиве аналогичная диспетчеризация выполнена через `switch`:

```c
static int handle_packet(broker_state *state, client *c, mqtt_packet *packet);
```

## 16. CONNECT handler

`connect_handler` проверяет, не подключен ли уже клиент с таким Client ID. Если клиент отправляет повторный CONNECT или Client ID уже используется, прежнее соединение закрывается. Затем создается объект клиента, он связывается с socket fd, сохраняется в глобальной таблице клиентов и получает CONNACK.

Упрощенная логика:

```text
if Client ID already exists:
    disconnect old/new duplicated client
else:
    create client state
    save client id and fd
    send CONNACK return code 0
```

## 17. DISCONNECT handler

`DISCONNECT` означает корректное завершение MQTT-сессии клиентом. Handler закрывает fd, удаляет клиента из карты clients и closures, обновляет счетчики статистики и не планирует ответ.

Упрощенная логика:

```text
close socket
remove client from global state
remove closure
update statistics
return disconnect code
```

## 18. SUBSCRIBE handler

Для каждой пары `(topic filter, qos)`:

1. Проверяется topic filter.
2. Если topic еще не существует, он создается.
3. Клиент добавляется в список подписчиков topic.
4. Если фильтр заканчивается на `/#`, нужно подписать клиента на дочерние topics. В trie это можно сделать рекурсивным prefix map.
5. В return codes добавляется согласованный QoS.
6. Отправляется `SUBACK`.

В архивном коде retained messages также отправляются сразу после `SUBACK`, если их topic совпадает с новым фильтром.

## 19. UNSUBSCRIBE handler

Для каждого topic filter клиент удаляется из списка подписок. Затем отправляется `UNSUBACK` с Packet Identifier из запроса.

## 20. PUBLISH handler

PUBLISH handler выполняет основную работу брокера:

```text
read topic and payload
validate topic name
if retain flag set: store retained message
find matching subscribers
forward PUBLISH to subscribers
if incoming QoS 1: send PUBACK
if incoming QoS 2: send PUBREC
```

QoS доставки подписчику выбирается как минимум между QoS публикации и QoS подписки. Например, если сообщение опубликовано с QoS 2, а клиент подписан с QoS 1, доставка должна идти не выше QoS 1.

## 21. PINGREQ handler

`PINGREQ` используется клиентом для keep alive. Брокер отвечает `PINGRESP`.

```text
Client → PINGREQ
Broker → PINGRESP
```

## 22. ACK handlers

Для QoS 1 и QoS 2 нужны подтверждения:

```text
QoS 1:
PUBLISH → PUBACK

QoS 2:
PUBLISH → PUBREC
PUBREL  → PUBCOMP
```

В учебной реализации из архива входящий QoS 2 закрывается корректной цепочкой ответов, но downstream-доставка подписчикам понижает QoS 2 до QoS 1, чтобы не усложнять хранение состояний каждого outgoing QoS2-сообщения.

## 23. Retained messages

Retain-флаг означает, что брокер сохраняет последнее retained-сообщение для topic. Когда новый клиент подписывается на matching topic filter, брокер сразу отправляет ему сохраненное сообщение.

Правило удаления: retained PUBLISH с пустым payload удаляет сохраненное сообщение для topic.

В архиве это реализовано структурами и функциями:

```c
typedef struct retained_message {
    char *topic;
    uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    struct retained_message *next;
} retained_message;

static void retained_store(...);
static void retained_send_matching(...);
```

## 24. Сравнение исходной архитектуры и практического архива

Исходные статьи строят проект постепенно и включают много низкоуровневых модулей: pack/unpack, network, evloop, hashtable, list, trie, core, server. Практический архив сводит эти идеи в более компактный компилируемый вариант.

Соответствие:

```text
Исходная идея                     Реализация в архиве
--------------------------------------------------------------
MQTT packet model                 src/mqtt.h, src/mqtt.c
Remaining Length                  mqtt_encode/decode_remaining_length
pack/unpack                       mqtt_parse_packet, mqtt_build_* functions
TCP server                        src/broker.c
callbacks/handlers                handle_packet + handle_* functions
clients                           client structure in broker.c
topics/subscriptions              subscription_node + mqtt_topic_matches
retained messages                 retained_message list
integration test                  tests/integration_test.py
```

Отличия:

```text
epoll заменен на select для компактности;
hashtable/trie заменены на простые списки;
сессии clean_session=false не сохраняются между переподключениями;
QoS 2 не хранит полноценное per-message состояние для downstream-доставки.
```

Эти отличия можно описать в дипломе как осознанные ограничения MVP и направления развития.

## 25. Возможные улучшения

Для развития проекта можно добавить:

1. `epoll` вместо `select` для масштабирования на большое число клиентов.
2. Hash table для быстрого поиска клиента по Client ID.
3. Trie или radix tree для ускоренного поиска подписок по topic.
4. Persistent session storage для clean session false.
5. Полноценный QoS 2 state machine для входящих и исходящих сообщений.
6. Очереди сообщений для offline-клиентов.
7. TLS и аутентификацию.
8. ACL на publish/subscribe.
9. Конфигурационный файл.
10. Unit tests для MQTT parser/packer и topic matcher.

## 26. Краткий вывод для диплома

В практической части реализован минимальный MQTT 3.1.1 брокер на C. Работа демонстрирует устройство бинарного протокола MQTT, сериализацию и десериализацию пакетов, TCP-сервер с мультиплексированием ввода-вывода, обработку базовых MQTT-команд, маршрутизацию публикаций по подпискам, wildcard topics и retained messages. Полученный проект является функциональным MVP и может быть использован как основа для дальнейшего расширения до более полного брокера.
