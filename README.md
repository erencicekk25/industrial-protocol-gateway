# industrial-protocol-gateway

Sahadaki bir Modbus TCP cihazından veri okuyup bunu MQTT üzerinden JSON olarak yayınlayan bir endüstriyel gateway'in, **hiçbir donanıma ihtiyaç duymadan** yerel makinede çalışan C++17 simülasyonu.

Tek bir komutla hem sanal bir saha cihazı (bir kompresör motoru) ayağa kalkıyor, hem de bu cihazı periyodik olarak sorgulayıp verisini bulut dünyasının anlayacağı formata çeviren gateway çalışmaya başlıyor. PLC, seri port dönüştürücü ya da broker kurmanıza gerek yok.

---

## Neden böyle bir proje?

Fabrika sahasında hâlâ en yaygın dil Modbus. 1979'dan beri ortalıkta. Basit, sağlam ve neredeyse her PLC, enerji analizörü ve sürücü tarafından destekleniyor. Ama Modbus bir **sorgu-cevap** protokolü: biri sormadıkça cihaz hiçbir şey söylemiyor. Veri de anlamsız 16 bitlik register'lar hâlinde geliyor; `419` aslında 41.9 °C, ama bunu ancak cihazın kılavuzunu okumuş biri bilebilir.

Bulut tarafı ise tam tersi şekilde düşünür. MQTT, **yayınla-abone ol** (publish/subscribe) modeliyle çalışır. Veri anlamlı topic'ler altında, kendini anlatan JSON mesajlarıyla akar. Dashboard'lar, zaman serisi veritabanları ve alarm servisleri bu dili konuşur.

Bu iki dünya arasında köprü kuran cihaza **endüstriyel gateway** (ya da edge gateway) diyoruz. IIoT mimarilerinde neredeyse her zaman karşınıza çıkan bu katman şunları yapar:

1. Saha cihazlarını belirli aralıklarla sorgular (polling).
2. Ham register değerlerini mühendislik birimlerine çevirir (ölçekleme, işaretli sayılar, 32 bit birleştirme).
3. Veriyi yapılandırılmış mesajlara dönüştürür ve MQTT broker'ına yayınlar.
4. Bağlantı koptuğunda sessizce ölmez; yeniden bağlanır ve cihazın durumunu (online/offline) üst sistemlere bildirir.

Bu proje bu dört adımı uçtan uca, gerçek protokol formatlarıyla gösteriyor. Ekrana basılan baytlar, gerçek bir Modbus cihazına veya gerçek bir MQTT broker'ına gönderilecek baytların birebir aynısı.

```
   SAHA (OT)                    GATEWAY                         BULUT / IT
 ┌─────────────┐  Modbus TCP  ┌─────────────────────┐  MQTT   ┌──────────────────┐
 │ PLC, sensör │ ◄──────────► │ poll → decode → JSON│ ──────► │ Broker, SCADA,   │
 │ sürücü ...  │  (port 502)  │                     │ (1883)  │ dashboard, DB    │
 └─────────────┘              └─────────────────────┘         └──────────────────┘
```

---

## Hızlıca çalıştırma

### Gereksinimler

- C++17 destekleyen bir derleyici: GCC 8+, Clang 7+ veya MSVC 2019+
- CMake 3.16+
- Herhangi bir harici kütüphane gerekmiyor. Ağ katmanı doğrudan Winsock / POSIX socket API'si üzerine yazıldı.

### Derleme

**Windows (MinGW-w64 + Ninja):**

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Windows (Visual Studio):**

```powershell
cmake -S . -B build
cmake --build build --config Release
```

> Visual Studio çoklu yapılandırma kullandığı için çıktılar `build\Release\` altına düşer.

**Linux / macOS:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Çalıştırma

```bash
./build/industrial_gateway
```

Program açıldığında önce sanal cihaz `127.0.0.1:5020` adresinde dinlemeye başlar, ardından gateway ona bağlanır ve her saniye bir telemetri mesajı yayınlar. Durdurmak için `Ctrl+C` yeterli. Gateway önce "offline" mesajını yayınlar, sonra thread'leri düzgünce kapatır ve bir özet tablo basar.

Örnek çıktı (okunabilirlik için kısaltıldı):

```
[INFO ] [ModbusServer] virtual device listening on 127.0.0.1:5020 (8 holding registers)
[INFO ] [Gateway] connected to Modbus device
[INFO ] [MQTT] PUBLISH factory/line-1/compressor-01/availability (qos=1, retain=1, 53 bytes on the wire)
[INFO ] [MQTT]   payload online
[INFO ] [MQTT] PUBLISH factory/line-1/compressor-01/telemetry (qos=0, retain=0, 588 bytes on the wire)
[INFO ] [MQTT]   payload {"deviceId":"compressor-01","sequence":1,"timestamp":"2026-11-09T09:11:18.358Z", ...
                          "measurements":{"temperature":{"value":41.9,"unit":"celsius"}, ...
```

### Komut satırı seçenekleri

| Seçenek | Açıklama | Varsayılan |
|---|---|---|
| `--host <adres>` | Modbus cihazının adresi | `127.0.0.1` |
| `--port <numara>` | Modbus TCP portu | `5020` |
| `--unit-id <0-255>` | Modbus unit identifier | `1` |
| `--interval <ms>` | Sorgulama periyodu (50–60000 ms) | `1000` |
| `--cycles <n>` | `n` telemetri mesajından sonra dur (0 = sonsuz) | `0` |
| `--device-id <isim>` | MQTT topic'lerinde kullanılan cihaz adı | `compressor-01` |
| `--no-server` | Gömülü sanal cihazı başlatma, dışarıdaki bir Modbus sunucusunu sorgula | — |
| `-v`, `--verbose` | Debug log'ları: ham Modbus ve MQTT baytları dahil | — |

Birkaç faydalı kullanım:

```bash
# 5 mesaj üret ve çık (CI veya hızlı demo için ideal)
./build/industrial_gateway --cycles 5 --interval 250

# Kablo üzerinde ne olduğunu bayt bayt gör
./build/industrial_gateway --cycles 1 -v
```

`-v` ile çalıştırdığınızda şuna benzer satırlar görürsünüz:

```
[ModbusClient] TX 00 01 00 00 00 06 01 03 00 00 00 08
[ModbusClient] RX 00 01 00 00 00 13 01 | 03 10 01 A3 01 FD 00 01 19 8B 0F A4 00 01 00 01 01 02
[MQTT]   packet  30 C9 04 00 26 66 61 63 74 6F 72 79 2F ...
```

Protokolü öğrenmenin en keyifli yolu bu satırları elle çözmeye çalışmak. Aşağıdaki bölüm tam olarak bunu yapıyor.

### Testler

Projede harici framework'e ihtiyaç duymayan bir test paketi var. Protokol kodlamasını birim seviyesinde, sunucu-istemci-motor üçlüsünü de gerçek socket'ler üzerinden uçtan uca test ediyor:

```bash
ctest --test-dir build --output-on-failure
# veya doğrudan:
./build/ipg_tests
```

Testler sunucuyu `port 0` ile başlatıp işletim sisteminin boş bir port atamasına izin veriyor. Bu sayede arka planda çalışan bir gateway varken bile çakışma olmuyor.

Linux/macOS'ta bellek sızıntısı ve tanımsız davranış kontrolü için sanitizer'lı bir derleme alabilirsiniz:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DIPG_ENABLE_SANITIZERS=ON
cmake --build build-asan && ./build-asan/ipg_tests
```

---

## Mimari

### Bileşenler

```
                         ┌──────────────────────── main.cpp ────────────────────────┐
                         │  argümanları okur, bileşenleri kurar, Ctrl+C'yi dinler   │
                         └──────────┬───────────────────────────────┬───────────────┘
                                    │                               │
               ┌────────────────────▼─────────┐        ┌────────────▼──────────────────────────┐
  Thread #1    │      VirtualModbusServer     │        │            GatewayEngine              │  Thread #2
               │  ─ select() ile çoklu client │  TCP   │  ─ sabit periyotlu polling döngüsü    │
               │  ─ fiziksel model (motor)    │◄──────►│  ─ exponential backoff + reconnect    │
               │  ─ FC 0x03 + exception cevap │ :5020  │  ─ online/offline durumu              │
               └──────────────────────────────┘        │                                       │
                                                       │   ┌──────────────┐  ┌──────────────┐  │
                                                       │   │ ModbusClient │  │MqttPublisher │  │
                                                       │   │ MBAP/PDU     │  │ JSON + MQTT  │  │
                                                       │   │ kodla/çöz    │  │ PUBLISH      │  │
                                                       │   └──────────────┘  └──────┬───────┘  │
                                                       └────────────────────────────┼──────────┘
                                                                                    │ IMqttTransport
                                                                          ┌─────────▼──────────┐
                                                                          │ConsoleMqttTransport│
                                                                          │ (broker yerine)    │
                                                                          └────────────────────┘
```

| Dosya | Sorumluluk |
|---|---|
| [VirtualModbusServer](include/VirtualModbusServer.hpp) | Kendi thread'inde çalışan sahte saha cihazı. Gelen istekleri cevaplarken arka planda bir kompresör motorunun fiziksel modelini günceller. |
| [ModbusClient](include/ModbusClient.hpp) | Modbus TCP master. İsteği bayt bayt oluşturur, cevabı doğrular ve çözer. Hata türüne göre bağlantıyı koruyup korumayacağına karar verir. |
| [MqttPublisher](include/MqttPublisher.hpp) | Register'ları mühendislik birimlerine çevirir, JSON üretir ve MQTT 3.1.1 PUBLISH paketini kodlar. |
| [GatewayEngine](include/GatewayEngine.hpp) | Orkestratör. Zamanlama, yeniden bağlanma, istatistikler ve cihaz durum bildirimlerinden sorumlu. |
| [RegisterMap](include/RegisterMap.hpp) | Cihazın register haritası. Sunucu ile gateway arasındaki "sözleşme". |
| [ModbusProtocol](include/ModbusProtocol.hpp) | MBAP başlığı, fonksiyon kodları, exception kodları ve hata sınıfları. |
| [TcpSocket](include/TcpSocket.hpp) | Windows ve POSIX'i tek bir arayüzde birleştiren RAII socket sarmalayıcısı. |
| [Logger](include/Logger.hpp) | İki thread'in log satırlarının birbirine karışmasını engelleyen basit, thread-safe logger. |

### Simüle edilen cihaz ve register haritası

Sanal cihaz, bir üretim hattındaki hava kompresörünün motorunu taklit ediyor. Değerler rastgele zıplamıyor, aralarında fiziksel bir ilişki var:

- **Güç tüketimi** yaklaşık 45 saniyelik bir üretim döngüsüyle 5.3–9.7 kW arasında salınır. Ara sıra birkaç saniye süren yük sıçramaları (+1.8 kW) olur. İkinci kademe devreye giriyormuş gibi düşünebilirsiniz.
- **Gövde sıcaklığı** yükle birlikte artar ama anında değil: birinci dereceden bir gecikme filtresiyle hedef değere yavaşça yaklaşır. Gerçek bir motorun ısıl kütlesi de böyle davranır.
- **Şebeke gerilimi** 400 V civarında dolaşır ve yük arttıkça biraz düşer.
- Sıcaklık 50 °C'yi veya güç 9.5 kW'ı geçerse durum word'ündeki ilgili alarm bitleri set edilir.

| Adres | Ad | Tip | Ölçek | Birim |
|---|---|---|---|---|
| 0 | Sıcaklık | INT16 | 0.1 | °C |
| 1 | Nem | UINT16 | 0.1 | %RH |
| 2–3 | Güç | UINT32 (big-endian word sırası) | 0.1 | W |
| 4 | Gerilim | UINT16 | 0.1 | V |
| 5 | Durum word'ü | bit alanı | — | bit0 çalışıyor, bit1 aşırı sıcaklık, bit2 yüksek yük |
| 6 | Heartbeat | UINT16 | 1 | sürekli artan sayaç |
| 7 | Firmware | UINT16 | — | `major << 8 \| minor` |

Bu harita, sahada en sık karşılaşılan üç tuzağı bilerek içeriyor:

- **İşaretli değerler:** Sıcaklık INT16. `0xFF9C` aslında `-100`, yani -10.0 °C. Register'ı işaretsiz okursanız 6553.2 °C gibi bir saçmalık elde edersiniz.
- **32 bit değerler:** 7.5 kW × 10 = 75000, bu da 16 bite sığmıyor. Güç iki register'a bölünüyor. Hangi word'ün önce geldiği (word order) cihazdan cihaza değişir; bizde yüksek word önce.
- **Ölçekleme:** Modbus'ta ondalık sayı yok. 41.9 °C, kablo üzerinde `419` olarak gider.

### Bir döngüde veri akışı

```
GatewayEngine          ModbusClient            VirtualModbusServer        MqttPublisher
     │ deadline geldi        │                          │                        │
     │──readHoldingRegs────►│                          │                        │
     │                       │── 00 01 00 00 00 06 ───►│                        │
     │                       │   01 03 00 00 00 08      │ register'ları kilitle, │
     │                       │                          │ kopyala                │
     │                       │◄── 00 01 00 00 00 13 ───│                        │
     │                       │    01 03 10 01 A3 ...    │                        │
     │                       │ txn id, unit id, byte    │                        │
     │                       │ count doğrula            │                        │
     │◄─ [419, 509, 1, ...] ─│                          │                        │
     │──publishTelemetry────────────────────────────────────────────────────────►│
     │                       │                          │   decode → JSON →      │
     │                       │                          │   PUBLISH paketi →     │
     │                       │                          │   transport.transmit() │
     │ deadline += interval  │                          │                        │
```

İsteği satır satır okuyalım:

```
00 01   Transaction ID   her istekte artar, cevap aynı ID ile dönmek zorunda
00 00   Protocol ID      Modbus için her zaman 0
00 06   Length           arkadan gelen bayt sayısı (unit id + PDU)
01      Unit ID          hedef cihaz (gateway arkasındaki seri cihazları ayırt etmek için)
03      Function code    Read Holding Registers
00 00   Başlangıç adresi
00 08   Register sayısı
```

Cevapta `03 10` ifadesi "fonksiyon 3, arkadan 16 bayt geliyor" demek. `01 A3` = 419 → 41.9 °C. `00 01 19 8B` iki register'ı birleştirilince 72075 → 7207.5 W.

MQTT tarafında `30 C9 04 ...` ilk bakışta tuhaf görünebilir. `30` PUBLISH paket tipi (QoS 0, retain yok). `C9 04` ise MQTT'nin "remaining length" kodlaması: her bayt 7 bit taşıyor, en üstteki bit "devamı var" anlamına geliyor. `0x49 + 4 × 128 = 585` bayt.

### Hata yönetimi

Endüstriyel sistemlerde "hata olursa programı kapat" diye bir seçenek yok. Gateway hataları üç sınıfa ayırıyor ve her birine farklı tepki veriyor:

| Hata | Örnek | Bağlantı | Tepki |
|---|---|---|---|
| `ModbusException` | Cihaz "geçersiz adres" (0x02) diye cevap verdi | **Korunur.** Çerçeve eksiksiz okundu, akış senkron. | Uyarı log'u, backoff, tekrar dene |
| `ProtocolError` | Transaction ID uyuşmadı, byte count tutarsız | **Kapatılır.** Akışın neresinde olduğumuzu artık bilmiyoruz. | Yeniden bağlan |
| `NetworkError` | Timeout, bağlantı reddi, kopma | **Kapatılır.** | "offline" yayınla, exponential backoff ile yeniden bağlan |

Backoff 500 ms'den başlıyor, her başarısız denemede ikiye katlanıyor ve 8 saniyede sabitleniyor. İlk başarılı okumada tekrar başa dönüyor. Böylece cihaz kısa süreliğine gidip geldiğinde hızlı toparlanıyoruz, uzun süre yoksa da ağı gereksiz yere meşgul etmiyoruz.

Zamanlama **sabit hızlı** (fixed-rate): bir sonraki sorgu zamanı "şimdi + periyot" değil, "önceki deadline + periyot" olarak hesaplanıyor. Bu sayede saatler boyunca çalışsa bile polling kaymıyor. Bir döngü periyodu aşarsa, kaçırılan döngüleri art arda patlatmak yerine atlıyoruz.

---

## Kullanılan tasarım kalıpları

**RAII (Resource Acquisition Is Initialization).** Projenin belkemiği bu. Her kaynağın bir sahibi var ve kaynak, sahibinin ömrüyle birlikte otomatik olarak serbest bırakılıyor:

- `TcpSocket` sadece taşınabilir (move-only) bir tip. Socket handle'ı destructor'da tam olarak bir kez kapatılıyor. Kopyalama derleme zamanında engellendiği için "aynı socket'i iki kez kapatma" hatası mümkün değil.
- `NetworkContext`, Windows'taki `WSAStartup`/`WSACleanup` çiftini sarmalıyor. Socket sahibi her sınıf bunu socket'lerinden *önce* üye olarak tanımlıyor. C++ üyeleri ters sırada yok ettiği için ağ yığını her zaman socket'lerden sonra kapanıyor.
- `VirtualModbusServer` ve `GatewayEngine` destructor'larında thread'lerini durdurup `join` ediyor. Programın neresinden exception fırlarsa fırlasın, sahipsiz kalan bir thread veya açık bir socket olmuyor.
- `getaddrinfo` sonucu özel deleter'lı bir `std::unique_ptr` ile tutuluyor.

Kodda tek bir `new`/`delete` çifti yok; tüm sahiplik `std::unique_ptr`, `std::vector` ve değer semantiğiyle ifade ediliyor.

**Strategy.** `IMqttTransport` arayüzü, üretilen MQTT paketinin nereye gideceğini soyutluyor. Şu an `ConsoleMqttTransport` paketleri ekrana basıyor, testlerde ise `CapturingTransport` onları bir listeye topluyor. Gerçek bir broker'a bağlanmak için TCP üzerinden CONNECT/PUBLISH gönderen yeni bir transport yazmak yeterli. `MqttPublisher` ve `GatewayEngine` tek satır değişmez.

**Dependency Injection.** `GatewayEngine` publisher'ını kendisi yaratmıyor, dışarıdan alıyor. `MqttPublisher` da transport'unu dışarıdan alıyor. Testlerin gerçek konsol çıktısıyla uğraşmadan motoru uçtan uca doğrulayabilmesi bu sayede mümkün.

**Orchestrator / Facade.** `GatewayEngine`, `main.cpp`'ye çok sade bir arayüz sunuyor: `start()`, `stop()`, `statistics()`. Bağlantı yönetimi, zamanlama, backoff ve durum bildirimi gibi karmaşık işler bu arayüzün arkasında kalıyor.

**Monitor (thread-safe paylaşılan durum).** Sanal sunucunun register bankası bir mutex ile korunuyor. Fiziksel model register'lara yazarken istek işleyici aynı anda yarım güncellenmiş bir değeri okuyamıyor. Bu ayrıntı 32 bitlik güç değeri için özellikle önemli: yüksek ve düşük word farklı anlardan gelirse anlamsız bir sayı ortaya çıkar.

**Single source of truth.** Register haritası tek bir başlık dosyasında (`RegisterMap.hpp`) tanımlı. Hem sunucu (kodlayıcı) hem gateway (çözücü) bu dosyayı kullanıyor, dolayısıyla ikisinin birbirinden habersiz farklı şeyler düşünmesi mümkün değil.

### Kod kalitesi üzerine birkaç not

- **const-correctness:** Durumu değiştirmeyen tüm metotlar `const`, saf hesaplamalar (`buildReadHoldingRegistersRequest`, `encodePublishPacket` vb.) yan etkisiz `static` fonksiyonlar. Bu sayede socket açmadan birim testi yazılabiliyor.
- **`[[nodiscard]]`:** Dönüş değeri göz ardı edilmemesi gereken her fonksiyonda kullanıldı.
- **`noexcept`:** `stop()`, destructor'lar ve thread giriş fonksiyonları exception fırlatmıyor. Thread fonksiyonundan kaçan bir exception `std::terminate` demek; bu yüzden döngüler kendi içinde her şeyi yakalıyor.
- **Uyarılar:** Proje GCC'de `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` ile **sıfır uyarıyla** derleniyor. `-Wconversion` özellikle bayt seviyesinde çalışan kodda sessiz daralma (narrowing) hatalarını yakalamak için önemli.
- **Lokalden bağımsızlık:** JSON üretilirken stream'e `std::locale::classic()` uygulanıyor. Aksi takdirde Türkçe gibi ondalık ayırıcısı virgül olan bir sistemde `41,9` yazılıp geçersiz JSON üretilebilirdi.

---

## Bilinen sınırlamalar ve geliştirme fikirleri

Bu proje eğitim amaçlı. Sınırlarını bilerek çizdim:

- Sadece **Function 0x03** (Read Holding Registers) destekleniyor. Diğer fonksiyon kodları standarda uygun şekilde "illegal function" exception'ı ile cevaplanıyor.
- MQTT tarafı yalnızca **PUBLISH paketini kodluyor**. CONNECT, PUBACK ve PINGREQ gibi oturum yönetimi yok; paketler konsola gidiyor.
- Ağ katmanı yalnızca **IPv4** üzerinde çalışıyor.
- Windows'ta kapalı bir porta bağlanmaya çalışmak yaklaşık 2 saniye sürüyor. Bu, işletim sisteminin reddedilen SYN paketlerini yeniden denemesinden kaynaklanıyor; bizim kodumuzla ilgili değil.

Projeyi genişletmek isterseniz doğal sonraki adımlar şunlar:

1. Gerçek bir MQTT transport'u yazıp yerel bir Mosquitto broker'ına bağlanmak (`mosquitto_sub -t 'factory/#' -v` ile mesajları izlemek oldukça tatmin edici).
2. Function 0x04 (Input Registers) ve 0x06/0x10 (yazma) desteği eklemek; ardından MQTT'den gelen komutları cihaza iletmek (çift yönlü gateway).
3. Register haritasını koda gömmek yerine bir JSON/YAML yapılandırma dosyasından okumak; böylece aynı gateway farklı cihazlarla çalışabilir.
4. Birden fazla cihazı farklı periyotlarla sorgulamak.
5. `--no-server` ile [diagslave](https://www.modbusdriver.com/diagslave.html) veya `pymodbus` simülatörü gibi harici bir Modbus sunucusuna karşı test etmek.

---

## Proje yapısı

```
industrial-protocol-gateway/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── ByteUtils.hpp            big-endian yardımcıları, hex dump
│   ├── GatewayEngine.hpp
│   ├── Logger.hpp
│   ├── ModbusClient.hpp
│   ├── ModbusProtocol.hpp       MBAP, fonksiyon/exception kodları
│   ├── MqttPublisher.hpp
│   ├── RegisterMap.hpp          cihaz register haritası
│   ├── TcpSocket.hpp            RAII socket (Winsock + POSIX)
│   ├── TimeUtils.hpp
│   └── VirtualModbusServer.hpp
├── src/
│   ├── GatewayEngine.cpp
│   ├── Logger.cpp
│   ├── ModbusClient.cpp
│   ├── MqttPublisher.cpp
│   ├── TcpSocket.cpp
│   ├── TimeUtils.cpp
│   ├── VirtualModbusServer.cpp
│   └── main.cpp
└── tests/
    └── GatewayTests.cpp
```
