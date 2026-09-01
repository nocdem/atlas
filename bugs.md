# Açık sorunlar

2026-08-28 03:20 itibarıyla yazıldı; **2026-08-28 16:15'te güncellendi**. Her
madde, **ölçülmüş** olanla **varsayılan** olanı ayırarak yazıldı. Kanıtı olmayan
hiçbir şey neden diye sunulmadı. Orijinal bulgular silinmedi: her maddenin
altına o günkü metin korunarak tarihli bir **DURUM** notu eklendi.

## Durum özeti — 2026-08-28 16:15

Kök neden kapatıldı ve etkisi ölçüldü: `/opt/dna`'nın iz sürülmeyen dosya
sayısı Atlas'ın 20 000'lik keşif tavanının üstündeydi, tavan aşılınca bir geçiş
kendini "içerik doğrulandı" ilan edemiyor, dolayısıyla olay boşluğu asla
kapanamıyor ve her geçiş tam oluyordu. Build dizinleri yok sayılınca sayı
tavanın altına indi. (Notlarda bu neden `RC-A` diye indekslenir.)

| | Önce | Sonra |
| --- | --- | --- |
| dna aynası | 98 992 dosya | **23 393** |
| dna reconcile | 21 996 incelendi / 21 996 hash | **2 271 / 2 271** |
| reconcile süresi | ~4 500–5 000 ms (en kötü 19 864) | **~1 300 ms** |
| izlenen dizin (dna) | 13 060 kaynak | **294** |
| `event_gap` | **true** — kapanamıyordu | **false** (iki depo, kalıcı) |
| `index current` | **no** | **yes** — ancak bir scanner poll etmeye devam ettiği sürece |
| dna semantik indeks | STALE | **CURRENT** (kuşak 563) |
| `git ls-files --others` | 60 sn zaman aşımı | **12 ms** |
| daemon CPU (boşta) | %90–137 | **%0** (30 sn ölçüm) |

Kanıt zinciri: kuşak 19650 geçiş turu `2271 examined, +2 ~6 -19727` → kuşak
19651 `2271, 1332 ms` → `index current: yes`. 19 727 build dosyası indeksten
çıktı.

**Bir ayrımı karıştırmayın.** Keşif tavanının kapattığı bayrak `event_gap` ve o
kalıcı olarak **false**. `index_current` ayrıca canlı bir scanner heartbeat'i
ister: iki `--once` geçişinden sonra scanner çıktığı için 5 dakika içinde
`pending_full_reconcile` yeniden set oldu ve `index_current` false'a döndü.
Bu A13'ün tasarımı: scanner susarsa Atlas o depoyu güncel saymayı bırakır.
Keşif tavanı sorununun nüksü değil; ayrıntısı 9 numarada.

**Kurulan düzeltmeler.** Atlas `bb5df17` (2 numara; kapılar release/ASan/UBSan/
TSan 105/105 her biri, adversarial 18/18) ve `fe7e615`. `/opt/dna` `0e40db3c`
(`nodus/build-*/` yok sayılıyor).

**Kalıcılık dersi — ölçüldü.** Aynı `.gitignore` düzeltmesi 14:17'de commit
edilmeden uygulandı ve 14:37'de silindi: `.gitignore` tracked bir dosya, /opt/dna'da
eşzamanlı çalışma sürüyordu ve sıradan bir `git checkout` yetti. Scanner o sırada
yine 98 992 dosya aynaladı. Düzeltme ancak commit edildikten sonra tuttu.

---

## Durum özeti — 03:20'de yazıldığı hâliyle (tarihsel kayıt)

- Kurulu ve canlıda çalışan tek düzeltme: `fff67a5` (semantic sonsuz yeniden
  kurulum döngüsü). `main`'de ve `origin/main`'de.
- Çalışma ağacında **commit edilmemiş** bir düzeltme duruyor: `scanner.state`'in
  servis döngüsünü kilitlemesi. Aşağıda 2 numara.
- `atlas-scanner.service` şu anda **kapalı**. Kapalıyken `/opt/atlas` ve
  `/opt/dna` tazelenmez; daemon aynadaki son hâli okumaya devam eder.
- Daemon (`atlas.service`) ayakta.

---

## 1. Scanner sürekli ~%10 çekirdek yakıyor

**Ölçüldü.** 7072 saniye ömür, 736 saniye CPU. Kapatılmadan önce durmadan
çalışıyordu: `dna` geçişi ~10 dakika sürüyor, döngü ~13 dakika, yani boşta
kaldığı süre çok az.

**Ölçüldü.** Kuşak dizini (`<id>.next`) her geçişte **boş başlıyor**
(`src/daemon/mirror.c`, `open_repo_dir` → `make_dir`). `put_file`
`src/core/service_scanner.c:364`'te koşulsuz çağrılıyor. Yani değişen dosya olsun
olmasın 96 000 dosyanın tamamı her turda okunup hex'e çevrilip gönderiliyor.

**Çelişki — çözülmedi.** Eski scanner sürecinin sayaçları tutarsız:
`rchar 41.9 GB`, `wchar 60 MB`, `syscw 14956`. 96 000 dosya gönderen bir süreçte
yazma çağrısı sayısı bundan çok daha büyük olmalıydı. CPU'nun tam olarak nerede
gittiğini **bilmiyorum**. Ölçmeden çözüm yazılmamalı.

**Aday çözüm (yazılmadı):** scanner ne gönderdiğini hatırlasın (yol → 8 alanlı
kimlik), değişmeyeni hiç okumasın, sileni kendi hesaplayıp bildirsin, ayna
yerinde güncellensin. Bu, A13'ün iki yazılı kararını tersine çevirir (kuşak
atomikliği; "küçük silme süpürmesi diye bir şey yoktur"), o yüzden kendi
tasarımını hak ediyor.

**DURUM (2026-08-28 16:15).** Atıf düzeltildi: RC-B ölçtü ki CPU scanner'da
değil daemon'daydı (scanner %3, daemon %137). Şimdi scanner çalışmıyor ve daemon
boşta **%0** (30 sn ölçüm). Kod iddiası geçerliliğini koruyor — kuşak her geçişte
boş başlıyor, `put_file` koşulsuz çağrılıyor, her tur tüm dosyalar gönderiliyor —
ama hacim 98 992'den **23 393**'e düştü. **Çözülmedi, küçüldü.**

---

## 2. `scanner.state` servis döngüsünü kilitliyor — ÇÖZÜLDÜ ve kuruldu

**Ölçüldü.** 17 dakika boyunca saniyede bir `atlas daemon ping` örneklendi —
yalnızca JSON yazan, ne yazıcıya ne veritabanına dokunan bir metot:

```
54 duraklama (>=500 ms), toplam 169.4 s bloke, en kotu 9284 ms
```

**Ölçüldü.** Duraklamalar, arkasında bekledikleri reconcile'larla birebir
örtüşüyor: 4346 ms duraklama ↔ 4936 ms'lik geçiş; 4590 ms ↔ 4742 ms.

**Nedeni kanıtlandı.** `method_scanner_state` yazıcıyı 10 saniyelik süreyle
senkron bekliyordu. Servis döngüsü aynı anda tek istek işlediği için yazıcıyı
bekleyen bir istemci, bekleyen tüm istemciler demek (A9.2.6).

**Düzeltme yazıldı:** `atlas_writer_call_mirror_state` kaldırıldı, yerine
beklemesiz `atlas_writer_submit_mirror_state` kondu; istemci iş **kabul
edildiğinde** cevaplanıyor (A8-CI kuralı). Onay kanalı `scanner.poll`: aynası
tamamlanmamış depo için direktifi `full`.

Ayrıca scanner'ın yanlış uyarısı düzeltildi: "the daemon did not record this run"
diyordu, oysa kayıt düşmüştü — zaman aşımı kaydedilmediğinin kanıtı değil.

**Kapılar:** release / ASan / UBSan 105/105. **TSan yarıda kesildi.**
Commit edilmedi, push edilmedi, kurulmadı.

**DURUM (2026-08-28 16:15). Çözüldü, kuruldu.** `a153afb` → merge `bb5df17`.
TSan tamamlandı; kapılar release/ASan/UBSan/TSan **105/105 her biri**,
adversarial **18/18**. Kuruldu; daemon bu binary ile çalışıyor.

---

## 3. Her ayna yayımı, deponun tamamını yeniden hash'letiyor

**Ölçüldü.**

```
reconciled dna generation 19499: 21996 examined, 21996 hashed,
0 unchanged by identity, 19864 ms
```

21 996 dosyanın hiçbiri kimlikten eşleşmiyor, çünkü yayım `<id>` dizinini
komple değiştiriyor: her dosyanın inode'u, ctime'ı, mtime'ı yeni. A1'in sekiz
alanlı kimlik önbelleği tasarım gereği işe yaramıyor.

**Not:** sabit bağ (hard link) bunu çözmez — `link()` inode'un ctime'ını
değiştirir, kimlik yine eşleşmez, hash yine yapılır. Bu yol denenip elenmeli
diye yazıldı ki tekrar önerilmesin.

Aynı yayım 12 540 izlemeyi de yıkıp yeniden kurduruyor ve bir olay boşluğu
doğuruyor.

**DURUM (2026-08-28 16:15). Çözülmedi, ~15 kat ucuzladı.** Kuşak 19651 hâlâ
`0 unchanged by identity` — her yayım inode'ları değiştiriyor, mekanizma aynı.
Ama 21 996 yerine **2 271** dosya, 19 864 ms yerine **~1 300 ms**, ve yıkılıp
kurulan izleme 12 540 yerine **294**.

---

## 4. `dna`'nın her reconcile'ı tam — nedeni kanıtlanmadı

**Ölçüldü.** 15 dakikada `dna` 10 kez, `atlas` 45 kez reconcile edildi.
`dna`'nınkilerin hepsi tam (21 996 hash), `atlas`'ınkilerin hepsi artımlı
(0 hash, 417 unchanged by identity).

**Aday neden — kanıtlanmadı.** `src/daemon/watch.c:3174`: `owes_gap` doğruyken
geçiş `full` oluyor, ve yorumuna göre borç, boşluk yayımı veritabanına düşene
kadar duruyor. Yazıcı meşgulse yayım gecikir, borç durur, geçiş tam olur,
22 000 hash yazıcıyı daha da meşgul eder. Kendini besleyen bir döngü olabilir.

Bu **hipotez**. 2 numaralı düzeltme kurulduktan sonra `dna`'nın geçişleri
artımlıya dönerse doğrulanmış olur.

**DURUM (2026-08-28 16:15). Hipotez yanlıştı; gerçek neden keşif tavanının
aşılmasıydı.** Buradaki
doğrulama ölçütü de geçersizdi: 2 numara kurulsa bile dna artımlıya dönemezdi,
çünkü kesilme keşif tavanından geliyordu, yazıcının meşguliyetinden değil.
Tavan sorunu giderildikten sonra `index current: yes`.

---

## 5. `watch.c` hâlâ daemon'da

**Ölçüldü ve önemli:** watcher'ı scanner'a taşımak **tek başına hiçbir şey
kazandırmıyor.** Kuşak her geçişte boş başladığı için, inotify scanner'a "tek
dosya değişti" dese bile 96 000 dosyanın hepsi yeniden yazılmak zorunda.

Taşımanın anlamlı olması için önce 1 numaranın çözülmesi gerekiyor.

**DURUM (2026-08-28 16:15).** Değerlendirme geçerli: 1 numara çözülmeden
watcher'ı taşımak hâlâ anlamsız.

---

## 6. Yazılı ama çözülmemiş borçlar (A13'ten devir)

- Kayıt, bir deponun olgularını **hangi principal'ın** ürettiğini söylemiyor.
  Günlüğe yazılıyor, saklanmıyor; saklamak bir kolon ve migration ister.
- Daemon tarafındaki gate, okunamayan ağaçta kapalı düşüyor: `atlas_gate_run`
  veritabanı alıyor, bağlam almıyor. A6 sözleşme değişikliği gerektirir.
- `core.excludesFile` P0'da yazılı bir maliyet, çözülmüş değil.

**DURUM (2026-08-28 16:15).** Üçü de değişmedi.

---

## 7. Küçük / dış kaynaklı

- `/opt/dna`'nın kendi derleme veritabanı silinmiş kaynakları adlandırıyor.
  Atlas bunu doğru rapor ediyor; düzeltme `/opt/dna` tarafında.
- `atlas status dna` şu an `0 compile databases` diyor. Daha önce 1200 birim
  görünüyordu. **Değişimin sebebi incelenmedi.**

**DURUM (2026-08-28 16:15).** İkincisi çözüldü ve bir hata değildi: RC-D'ye
bakın — `atlas status` ile `code sem-status` iki ayrı sayaca bakıyor. Gerileme
yok; `sem-status dna` **4 compile database, 1220 birim** diyor ve kuşak 563
itibarıyla **CURRENT**. Birincisi (`/opt/dna`'nın derleme veritabanının silinmiş
kaynakları adlandırması) değişmedi ve hâlâ `/opt/dna` tarafında.

---

## 8. Daemon SIGTERM'e 30 saniyede cevap vermiyor — YENİ, çözülmedi

**Ölçüldü, iki kez.** 03:38:29 (reboot öncesi) ve 03:47:21. İkisinde de
`TimeoutStopSec=30` doldu ve systemd SIGKILL gönderdi:

```
03:47:21  received signal 15; shutting down
03:47:51  State 'stop-sigterm' timed out. Killing.
          Failed with result 'timeout'.   NRestarts=0
```

**Ölçüldü — sonucu.** `Restart=on-failure`, stop kaynaklı bir zaman aşımından
sonra tetiklenmiyor. Daemon 03:47:51'den 15:28'e kadar, **10 saat 40 dakika**
kapalı kaldı ve kimse fark etmedi.

**Ölçüldü — yan etkisi.** SIGKILL sonrası açılışta daemon şunu diyor: `the
previous Atlas daemon did not shut down cleanly; every repository is marked
incomplete until a full content verification completes`. Bu, borçlu bir tam
geçiş demek — yani tavanın üstündeki bir depoda asla tamamlanamayan bir borç.

**Nedeni kanıtlanmadı.** 03:42:46'daki `BUSY: ... semantic maintenance` satırı
yazıcıyı işaret ediyor ama bu bir **varsayım**, ölçüm değil. Kapanışın nerede
takıldığı ölçülmeden düzeltme yazılmamalı.

**Şiddeti azaldı, kaybolmadı.** Tavan sorunu gidince "incomplete" işaretinin
maliyeti kalıcı bir döngü değil, tek bir ~1,3 saniyelik geçiş. Ama bir sonraki
`systemctl stop` yine SIGKILL'lenir.

---

## 9. Scanner'ın systemd unit'i yok — YENİ

**Ölçüldü.** `atlas-scanner.service` diye bir unit `/etc/systemd/system/`
altında **yok**. `bugs.md`'nin "kapalı" demesi bir unit'i ima ediyordu; scanner
elle çalıştırılıyor.

**Sonucu:** bu oturumdaki iki elle geçişten sonra aynalar yine donacak. Ayna
donduğunda daemon eski hâli okumaya devam eder ve `scanner.poll` heartbeat'i
kesildiği için Atlas bu depoları er ya da geç güncel saymayı bırakır.

**Çözülmedi.** Bir unit yazmak A13'ün cadence kuralına (poll aralığı Atlas'ın,
scanner'ın değil) uymalı; bu kendi tasarımını hak ediyor.

**Canlı kanıt — 2026-08-28 16:20 ölçüldü.** Bu maddenin maliyeti aynı gün
gözlendi. İki elle `--once` geçişinden sonra scanner çıktı, yani heartbeat
sustu. Sonuç, `atlas events --json`:

```
dna:    event_gap: false,  pending_full_reconcile: true,  index_current: false
atlas:  event_gap: false,  pending_full_reconcile: true,  index_current: false
```

**Bu bir hata değil, A13'ün tasarımı.** `atlas_server_overlay_mirror`
(`src/ipc/server.c:445`) scanner destekli bir depoda heartbeat
`ATLAS_SCANNER_MIRROR_MAX_AGE_MS` içinde duyulmadıysa `pending_full_reconcile`
set ediyor; sabit **300 000 ms = 5 dakika**, poll aralığı onun yarısı
(`limits.h:294`, `limits.h:303`). Zaman çizgisi birebir oturuyor: scanner 15:51'de
bitti, 15:52'de `index_current: yes`, ~15:56'da pencere doldu.

**Ayrımı not etmek gerekir.** `event_gap` **false** ve öyle kalıyor — keşif
tavanının
kapattığı bayrak o. `index_current`'ı şimdi false tutan şey ayrı bir bayrak ve
ayrı bir sebep: kimse ağacı gözlemlemiyor. İkisini karıştırmak, çözülmüş bir
sorunu çözülmemiş sanmaya yol açar.

**Yani:** sürekli bir scanner olmadan bu iki depo kalıcı olarak "güncel değil"
kalır. `atlas scanner run` (`--once` olmadan) bunu düzeltir; kalıcı çözüm bir
systemd unit'idir ve **yazılmadı** — CLAUDE.md'nin "koddan veya testten asla
gerçek bir systemd servisi kurma/başlatma" kuralı gereği bu bir operatör
kararıdır.

**Unit yazıldı — 2026-08-28 16:30, kurulmayı bekliyor.**
`/opt/atlas/atlas-scanner.local.service` (kurulumu:
`bash /opt/atlas/install-scanner.local.sh`). `systemd-analyze verify` sıfır
döndü; tek uyarı `man atlas(1)` sayfasının olmaması, ki diğer Atlas unit'leri
de aynı `Documentation=` satırını taşıyor.

Tasarımı A13'ün kurallarına bağlı: `User=nocdem`, çünkü scanner kimliği depo
kökünün sahibidir ve başka bir hesap A13'ün bitirmek için var olduğu hatayı
yeniden üretir; `Restart=always`, çünkü döngünün temiz çıkışı yoktur ve sessizce
duran bir scanner tam da tazelik kuralının yakalamak için var olduğu durumdur;
`ExecStart` `--once` **taşımaz**, çünkü o bir anlık görüntüdür. Sandbox
`ReadOnlyPaths=/opt/atlas /opt/dna` ile sınırlı ve `InaccessiblePaths=/var/lib/atlas`
scanner'ın indeksi hiç açmamasını koddan değil çekirdekten gelen bir özellik
yapıyor.

Unit depoya **konmadı**, bilerek: hangi uid'in hangi ağaca sahip olduğu bir
dağıtım özelliğidir, Atlas bunu `repo add --scanner-uid` ile öğrenir. Commit
`6c4b9bb` `*.local.service`'i yok sayıyor.

---

## Durum — 2026-08-28 20:45

Üç madde kapatıldı, her biri beş kapıdan geçti (release/ASan/UBSan/TSan 105/105
her biri, adversarial 18/18).

| Commit | Madde | Ne yapıldı |
| --- | --- | --- |
| `0db5387` | 8 (kapanma zaman aşımı) | Kapanış artık her birim üzerinden geçen bir derleyici turunu beklemiyor |
| `9b5d8ca` | 7 (`--json` iki belge) | Orkestrasyon ailesi daemon yokken geçersiz JSON basıyordu |
| `60756e5` | 1 + 3 (ayna) | Geçiş yalnızca değişeni gönderiyor, gerisini daemon taşıyor |

**Madde 8 — zincir koddan kuruldu.** `stopping` her gönderim noktasında ve boşta
bekleme döngüsünde okunuyordu, **çalışan bir işin içinde hiçbir yerde**; A9.2.7'nin
yield'i de onu okumuyordu. Yani `pthread_join` o anki işin tamamını bekliyordu.
Semantik katmanda `cancel` zaten vardı ve birimler arasında yoklanıyordu;
`atlas_sem_index_on` onu dışarı açmıyordu. Açıldı.

*Zincir yazarken yakalanan ikinci hata:* iptal `ATLAS_ERR_USAGE` döndürüyor,
writer ise USAGE'ı "derleme tanımı bozuk" diye sınıflandırıp governor'ı `HOLD`'a
alıyor. İlk hâliyle düzeltme, kapanış yüzünden iptal olan bir geçişten sonra
semantik indeksin biri bir dosyayı düzenleyene kadar bir daha kurulmamasına yol
açardı. Atlas'ın kendi durdurduğu geçiş artık hiç kaydedilmiyor.

**Madde 7 — ölçüm backlog'u çürüttü.** "İki belge" değil, **hiç JSON olmayan**
çıktı: kapanmamış tek belge, içinde `"ok":true`, hata belgesi açık dizinin içine
gömülü. Suite'in kendi doğrulayıcısı beşini de reddediyordu. Fark önemli, çünkü
backlog'un önerdiği çare ("iki belgeye tahammül et") işe yaramazdı.

**Madde 1+3 — ölçülen ödül.** Beş dakikada bir 28.450 dosya (atlas 5.015 +
dna 23.435), neredeyse hiçbiri değişmemiş. Ömür ortalamaları: **scanner %7,6,
daemon %69,5** — maliyet ağırlıkla daemon'un scanner'a hizmet etmesinde, çünkü
her `scanner.put` 3,96 GB'lık veritabanına yeni bir bağlantı açıyor.

`scanner.keep` yolu adlandırıyor, baytları değil. **Scanner'ın hafızası yalnızca
sormaya karar veriyor**; daemon kendi yayımlanmış kuşağında ne tutuyorsa onu
link'liyor, tutmuyorsa `kept: false` diyor. Hafıza süreçte, diskte değil.
Karşılaştırdığı şey A1'in sekiz alanlı kimliği.

**Kalan yazılı maliyet:** daemon aldığını yine hash'liyor (hard link ctime'ı
değiştirir). Ölçek gönderimden ağaca indi: dna 2.271 dosya / ~1,3 sn.

**Ölçülmedi:** uçtan uca tasarruf, iki ardışık geçiş gerektirdiği için ancak
kurulumdan sonra görülür. `bash /opt/atlas/deploy.local.sh`, sonra scanner
günlüğünde `mirrored 23435 (23400 carried)` biçiminde bir satır beklenir.

---

## Durum — 2026-09-01 12:30

Dokuz maddenin hepsi canlı duruma karşı yeniden ölçüldü. Gövdeler silinmedi;
aşağısı o gövdelerin bugünkü karşılığıdır. Bir de yeni bir madde açıldı (10).

### Uçtan uca tasarruf ölçüldü — 28 Ağustos'un "ölçülmedi" notu kapandı

`60756e5` + `d258f69` kurulu (`/usr/local/bin/atlas`, Build ID `77e74763…`,
`build/atlas` ile aynı; tek fark install sırasında çıkarılan RPATH).

| | Önce (28 Ağu) | Şimdi (ömür ort., 82,6 sa) |
| --- | --- | --- |
| daemon | %69,5 | **%7,9** |
| scanner | %7,6 | **%0,65** |

Beklenen günlük satırı geldi: `atlas mirrored 5104 (5104 carried)`,
`dna mirrored 23425 (23424 carried)`.

Ömür ortalaması bu sabahki ~30 dakikalık yeniden kurulum bloğunu **içerir**.
Kararlı hâl ondan düşük: 5 dakikalık örneklemde %3,2 (94 örneğin 90'ı %15 altı),
tek nokta ölçümlerde %1,5.

### Madde madde

| # | Bugünkü hâl | Kanıt |
| --- | --- | --- |
| 1 | **Kapandı** | Scanner ömür ort. %0,65. Gövdedeki kod iddiası ("kuşak her geçişte boş başlıyor, `put_file` koşulsuz") `60756e5`+`d258f69` ile geçersizleşti |
| 2 | Kapandı (28 Ağu) | `bb5df17` kurulu |
| 3 | **Açık, bugün yeniden ölçüldü** | Aşağıya bakın |
| 4 | Kapandı (28 Ağu) | Kök neden keşif tavanıydı |
| 5 | **Açık ve ilk kez anlamlı** | Aşağıya bakın |
| 6 | Açık, üçü de | Şemada principal kolonu hâlâ yok |
| 7a | **Yeri bulundu, Atlas tarafı kapatıldı** | Aşağıya bakın |
| 7b | Kapandı (28 Ağu) | İki ayrı sayaç, gerileme değil |
| 8 | **Kapandı** | `0db5387` çalışan binary'de. Gövde hâlâ "çözülmedi" diyor; onu düzelten şey bu nottur. Canlı `systemctl stop` denenmedi |
| 9 | **Kapandı** | `atlas-scanner.service` kurulu, `enabled`, 3 gündür `active (running)` |

### 3 — hâlâ açık, mekanizma değişmedi

```
reconciled dna generation 24827: 2274 examined, 2274 hashed,
0 unchanged by identity, +0 ~0 -0, 0 commits, 1343 ms
```

Ayna yayımlanmayan turlarda aynı depo `420 examined, 0 hashed, 420 unchanged by
identity` veriyor — yani neden tam da gövdede yazdığı gibi: yayım her dosyanın
inode'unu değiştiriyor. Gövdenin "sabit bağ bunu çözmez, `link()` ctime'ı
değiştirir" öngörüsü **doğrulandı**: `d258f69` sabit bağ kullanıyor ve
`0 unchanged by identity` duruyor.

Bedel 19 864 ms → ~1 350 ms indi, sıfırlanmadı: her ~2,5 dakikada 2 694 dosya
(atlas 420 + dna 2274).

### 5 — bugün ilk kez gerçekten açıldı

Gövde "1 numara çözülmeden watcher'ı taşımak anlamsız" diyordu. 1 numara
çözüldü. Değerlendirme artık geçerli değil; madde yeniden değerlendirilmeyi
hak ediyor.

### 7a — yeri tam olarak bulundu, Atlas tarafı kapatıldı

`nodus/build-o15e-act/compile_commands.json` (20 Ağustos'tan kalma bayat build
ağacı) artık var olmayan **beş** dosya adlandırıyor:

```
nodus/src/witness/nodus_witness_v2_activation.c
nodus/src/witness/nodus_witness_v2_seam.c
nodus/tests/test_v2_activation.c
nodus/tests/test_v2_seam.c
shared/dnac/activation_wire.c
```

Bunlar dna'nın semantik indeksindeki tam olarak o beş `FAILED` birim
(`compiler_produced_no_translation_unit`). Diğer üç veritabanı temiz:
dnac 61/0, messenger 373/0, nodus/build 404/0 (girdi/eksik).

**Dışlamadan önce ölçüldü — kapsam kaybı sıfır.** `build-o15e-act` 366 ayrı
dosya adlandırıyor, diğer üçü 616. Yalnızca `build-o15e-act`'te olan dosya
sayısı **beş**, ve beşinin de var olanı **sıfır**.

Uygulandı: `atlas code sem-config dna --exclude nodus/build-o15e-act`.
Yazma iki kez `BUSY` aldı (A9.2.6 sözleşmesi: hiçbir şey kuyruğa girmedi),
üçüncüde kabul edildi. Geri okundu: pinlenen iki veritabanı ve `messenger/tests`
test kökü **korundu** (`--exclude` yalnızca kendi listesini yazıyor;
`cli.c`'deki `*_given` bayrakları).

**Sonuç ölçüldü, kuşak 615.** Yazmadan önceki tahminim "altıncı birim
(`PARTIAL nodus/tests/test_v2_restart_gate.c`) etkilenmez" idi; **yanlıştı**.

| | Kuşak 614 (önce) | Kuşak 615 (sonra) |
| --- | --- | --- |
| çeviri birimi | 1223 | **838** |
| complete | 1217 | **838** |
| failed | 5 | **0** |
| partial | 1 | **0** |

Birim sayısındaki 385'lik düşüş `build-o15e-act`'in girdi sayısıdır: birim
`(dosya, veritabanı)` çiftidir, dosya değil, yani 380 dosya iki kez değil bir kez
derleniyor. Her geçiş yaklaşık **%31 ucuzladı** — daemon'un en büyük tekrarlayan
masrafı bu.

**Ama bir boşluk diğeriyle takas edildi ve bu iddia edilmemelidir.**
`build-input discovery` **COMPLETE** iken **PARTIAL** oldu:
`incomplete because an operator excluded a subtree from the search`. A9.2.4'ün
disiplini bu — sınırlı arama evreni verdiktin yanında raporlanır — ama
`ATLAS_COVDIM_BUILD_INPUT_DISCOVERY` her olumsuz doğrulayıcının dayanağıdır, yani
`supports an absence` **yine `no`**, bu sefer başka sebeple ve sebep **operatörün
kendi tercihi**.

**Temiz çözüm, uygulanmadı çünkü operatör kararı:** dışlamayı geri al
(`--no-excludes`) ve bayat `/opt/dna/nodus/build-o15e-act/` dizinini sil. O zaman
hem beş hayalet birim gider hem keşif COMPLETE kalır. Silme `/opt/dna` tarafında
bir karardır ve Atlas hiçbir hedef depoyu değiştirmez.

**Yeniden okunmayı bekleyen bir değer.** Kuşak 615 `scope discovery UNKNOWN` ve
`gap: the_generation_recorded_no_coverage_manifest` diyor; kuşak 614 `DECLARED`
ve `616 of 813` diyordu. Yayımı yeni bitmiş bir kuşağın geçici hâli gibi duruyor
(`holding: a_generation_is_already_being_built`), ama **doğrulanmadı**: /opt/dna
üzerinde eşzamanlı çalışma sürdüğü için oturmuş bir kuşak beklenemedi. dna
sakinleştiğinde `code sem-status dna` bir kez daha okunmalı.

---

## 10. Semantik katman aynayı hiç bilmiyor — YENİ, 2026-09-01

**Ölçüldü.** `dna` için `code sem-config` çıktısı, keşif yürüyüşünün derinlik
tavanına çarptığı 28 dizin adlandırıyor, hepsi şu biçimde:

```
.claude/worktrees/agent-<id>/messenger/dna_messenger_flutter/android/app/src/
  main/kotlin/io/cpunk/dna_connect  [the_walk_reached_its_depth_ceiling_below_this_directory]
```

artı girilemeyen üç dizin (`Testing/stress_wallets`, `nodus/.cache`,
`nodus/build/.cache`). `last walked` o okumadan iki dakika önce.

**Zincir.** `.claude/` `/opt/dna/.gitignore:86`'da yok sayılıyor, dolayısıyla
aynada **yok** — aynanın `.git` dışı dosya kümesini git'in tracked+untracked
kümesiyle karşılaştırdım: 2278'e 2274, fark tam olarak dört `compile_commands.json`,
`.claude` altında tek dosya yok. O hâlde bu engelleri üreten yürüyüş
`/opt/dna`'yı okudu, aynayı değil. `/opt/dna` `drwxrwxr-x nocdem:cpunk-team`,
yani `o+rx`; daemon `uid=994(atlasd)` ve `cpunk-team`'de değil ama dünya izniyle
okuyabiliyor. Kaynak tarafı doğruluyor: **`src/sem/` içinde "mirror" dizesi hiç
geçmiyor**, ve `src/sem/index.c` derleme veritabanlarını `repo->root_path`
üzerinden `open` ediyor.

**Neden önemli.** CLAUDE.md'nin A13 kuralı "daemon ağacı okumaz; scanner
adlandıran bir depo aynasından ve **başka hiçbir şeyden** okunur" diyor ve
yalnızca iki istisna adlandırıyor: `src/orch/rundriver.c` ile
`src/orch/snapshot.c`. `src/sem/` bu listede yok. Burada çalışıyor olmasının
sebebi `/opt/dna`'nın dünyaya okunur olması — yani A13'ün var olma sebebi olan
senaryoda (mod 0400 nesneler, umask 077) reconcile başarılı olurken semantik
katman başarısız olurdu.

**Bugün gözlenen ikinci dereceden bedeli.** `/opt/dna/.claude/worktrees` altında
42 ajan worktree'si var: **87 140 dosya, 7,0 GB**. Ayna bunları bilerek dışarıda
bırakıyor; keşif yürüyüşü ise her turda içlerinde geziniyor ve derinlik tavanını
onlara harcıyor.

**Ölçülmedi:** bu, sabahki `a_compilation_database_changed` tekrarlarına katkıda
bulunuyor mu. Ayrım şu testle gelir: iki ayna yayımı arasında `compdb digest`
değişiyor mu.

**Düzeltme yazılmadı.** Semantik katmanı aynaya bağlamak A13'ün kapsamını
genişletir ve kendi tasarımını hak ediyor; burada yalnızca ölçüm var.
