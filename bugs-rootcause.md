# Kök nedenler — koddan ve canlı ölçümden doğrulandı

2026-08-28 03:20 civarı, `atlas.service` PID 3425365, `atlas scanner run`
PID 3886461 çalışırken ölçüldü. `bugs.md`'nin "scanner kapalı" dediği durum
artık geçerli değil: scanner 03:01'den beri ayakta.

## RC-A — Ana neden: `/opt/dna` tavanın üstünde, bu yüzden olay boşluğu
## **hiçbir zaman kapanamıyor**

**Ölçüldü.** `/opt/dna`: 1996 izlenen (tracked), **75 882 izlenmeyen ve
yok sayılmayan (untracked, non-ignored)** dosya. `nodus/build-*` dizinleri
`.gitignore`'da yok — `**/build-asan/` var, `build-d3`, `build-o15h-*` yok.

**Koddan doğrulandı.** Zincir:

1. `src/core/reconcile.c:1216` — `cc.limit = ATLAS_WATCH_MAX_DISCOVER_FILES`
   (= **20000**, `include/atlas/limits.h:274`).
2. `on_untracked` (`reconcile.c:369`) tavanı görünce
   `c->table->truncated = true`.
3. `reconcile.c:1232` → `note_truncated(...)` → `summary->truncated = true`.
4. `reconcile.c:1278` —
   `content_verified = opts->full && !summary->truncated && files_identity_hit == 0`
   → **her zaman false**.
5. `reconcile.c:1507` → `atlas_db_generation_complete(..., clear_gap=false, ...)`.
6. `src/db/db_state.c:343` — `event_gap` ve `pending_full_reconcile` yalnızca
   `clear_gap` doğruyken sıfırlanır → **hiç sıfırlanmıyor**.
7. `src/daemon/watch.c:1949` — `settle_owed_gaps` `!st.event_gap` istiyor →
   watcher'ın `owes_gap`'i **hiç kapanmıyor**.
8. `src/daemon/watch.c:3174` — `if (rw->owes_gap) full = true;` → **her geçiş
   tam**.
9. `src/core/reconcile.c:564` — tam geçişte `need_hash = true` koşulsuz, hiçbir
   kimlik okunmaz → `21996 hashed, 0 unchanged by identity`.

**Canlı kanıt.** `atlas events dna`:
`"event_gap":true, "pending_full_reconcile":true, "index_current":false,`
`"not_current_reason":"filesystem events were missed; a full reconciliation is outstanding"`.

**Sayıldı:** 02:00'dan beri **105 dna geçişinin 105'i tam** — hepsi
`21996 examined, 21996 hashed, 0 unchanged by identity, +0 ~0 -0`, her biri
~4650 ms. Bir tanesi bile artımlı değil.

**Düzeltme (bugs.md 4'e göre):** yoğunluk sabit değil, **tamlık** sabit.
Yayımdan ve izleme yeniden kurulumundan sonra kirli akış boşalana kadar
arka arkaya geçişler oluyor (03:17:25–03:19:12 arası on üç geçiş, boşluksuz),
sonra duruyor. 21996 = 1996 tracked + **tam olarak 20000 tavanı**.

**İkinci, bağımsız kesilme kaynağı.** `reconcile.c:1406` — `wts.truncated`,
tavanı `ATLAS_AI_MAX_CHANGED_PATHS` = **4096** (`limits.h:384`). dna'nın
aynasındaki `git status` 75 000'in üstünde iz sürülmeyen yol bildiriyor.
Yani **20000 tavanını yükseltmek tek başına yetmez**; 4096 yine keserdi.

Bu bir **kilitlenme**: tavanın üstündeki bir depo olay boşluğunu asla
kapatamaz, dolayısıyla sonsuza dek tam reconcile'a mahkûmdur. `bugs.md` 4
numaradaki hipotez (yazıcı meşgul → borç durur) değil; yazıcının meşgul olması
bunun **sonucu**.

## RC-B — CPU daemon'da ve **her `scanner.put` başına veritabanı işinde**
## (bugs.md 1 numara — atıf yanlıştı)

**Ölçüldü**, 30 saniyelik delta:

| | rchar | wchar | syscw | utime+stime |
|---|---|---|---|---|
| scanner 3886461 | +15.2 MB | **+0** | **+0** | +0.91 s (**%3**) |
| daemon 3425365 | +5.07 GB | +141.7 MB | +47 902 | +41.1 s (**%137**) |

Scanner `sock_alloc_send_pskb`'de **uyuyor** — sokete yazarken bloke.
`bugs.md`'nin "çelişki — çözülmedi" dediği şey (`rchar 41.9 GB`,
`wchar 60 MB`, `syscw 14956`) budur: scanner ömrünün çoğunu sokette bloke
geçiriyor, çünkü daemon onu okuyamıyor.

**Nerede yandığı ölçüldü.** Daemon iş parçacığı dökümü: **ana iş parçacığı
(serve döngüsü) %77, `R` durumunda**; diğer yedisi `futex_wait` veya
`do_sys_poll`. dna hiç reconcile edilmezken bile daemon %86 CPU.

`strace -c -f`, 6 saniye, ana iş parçacığı:

```
746   accept4      <- 6 saniyede 746 scanner.put (saniyede ~124)
12568 openat       <- put basina ~17
19368 pread64      <- put basina ~26  (SQLite sayfa okumasi)
18620 newfstatat   <- put basina ~25
14048 close        <- put basina ~19
6714  fcntl        <- put basina ~9
3670  mkdirat      <- put basina ~5 (3661'i EEXIST)
677   unlinkat     <- hepsi ENOENT
```

**Her `scanner.put`'un maliyeti, koddan:**

1. `src/ipc/server.c:1356` — **istek başına yeni bir salt-okunur SQLite
   bağlantısı** açılıp kapanıyor. Veritabanı **3.96 GB**, WAL **209 MB**.
   Yorumda gerekçesi yazılı ("her istek kendi başlangıcındaki anlık görüntüyü
   görür"); tasarım insan ölçekli istekleri varsayıyor, **geçiş başına
   100 000 istek gelen bir toplu veri kanalını değil**.
2. `server_scanner.c:285` — `require_scanner` → `atlas_db_repo_list`, yani
   **her put'ta `repositories` tam taraması**.
3. `server_scanner.c:320` — `atlas_db_repo_get_by_id`, ikinci sorgu.
4. `server_scanner.c:346` — `atlas_mirror_open_staging`, her put'ta
   `data_dir` → `mirror` → `<id>.next` yolunu baştan `mkdirat`+`openat`.
5. `mirror.c:walk_to_parent` — yol bileşeni başına `mkdirat`+`openat`+`close`.

Yani "scanner %10 çekirdek yakıyor" gözlemi, **daemon'un scanner'a hizmet
ederken yaktığı** CPU. Scanner'ın kendi payı %3–16.

**Ayrı ve gerçek olan kısım:** `bugs.md` 1'in kod iddiası doğru. Kuşak dizini
her geçişte boş başlıyor (`mirror.c:open_repo_dir`, publish `<id>.next`'i
`<id>`'ye taşıdığı için sonraki `make_dir` yeni ve boş bir dizin yaratır) ve
`put_file` (`service_scanner.c:364`) koşulsuz çağrılıyor. Aynada 98 853 dosya
var (20 972'si `.git`, 77 881'i ağaç). Hepsi her turda okunup hex'e çevrilip
gönderiliyor. Bu gerçek bir israf ama ölçülen CPU'nun kaynağı değil.

## RC-C — Her yayım kimlik önbelleğini bir kez düşürüyor (bugs.md 3 numara)

**Koddan doğrulandı.** `atlas_mirror_publish` `<id>`'yi `.old`'a, `<id>.next`'i
`<id>`'ye `renameat` ile taşıyor; her dosyanın inode'u yeni.

**Canlı kanıt.** `atlas` deposu, 03:16:56'da bir izleme yeniden kurulumunun
hemen ardından: `418 examined, 418 hashed, 0 unchanged by identity`. Öncesi ve
sonrası: `0 hashed, 417/418 unchanged by identity`.

Yani maliyeti **yayım başına bir** tam hash — gerçek, ama döngü değil. dna'da
döngüyü yapan RC-A.

## RC-F — `git ls-files --others` /opt/dna'da **60 saniyede zaman aşımına
## uğruyor** (yeni bulgu, `bugs.md`'de yok)

**Ölçüldü**, scanner günlüğü:

```
03:14:04   dna  mirrored 98854, skipped 0 symlink, 0 unreadable
03:19:12   dna  mirrored 10025, skipped 0 symlink, 0 unreadable
03:19:12 scanner: pass failed: /usr/bin/git timed out after 60000 ms
```

75 882 iz sürülmeyen dosya git'in `--others` taramasını 60 saniyelik sınırın
üstüne çıkarıyor. Geçiş 10 025 dosyada kesiliyor, `walked != ATLAS_OK` oluyor,
`complete=false` kalıyor, **ayna yayımlanmıyor**. Yani dna'nın aynası ancak
git'in tesadüfen zamanında bitirdiği turlarda tazeleniyor.

Aynı kök: RC-A ile aynı 75 882 dosya.

## RC-D — `atlas status dna` `0 compile databases` diyor (bugs.md 7 numara)

**Ölçüldü.** `atlas code sem-status dna` **4 compile databases, 1220 birim**
diyor. İki sayaç farklı tablolara bakıyor; `status`'unki A3/A8-CI tarafı.
Bir gerileme değil, iki ayrı sayaç. Doğrulaması yapılacak.

## RC-E — Daemon `kernel_max_user_watches: 1024` bildiriyor

**Ölçüldü.** `/proc/sys/fs/inotify/max_user_watches` = **122910**.
Daemon 1024 rapor ediyor ve 13 124 izleme tutuyor — bildirilen sayı yanlış.
Şu an zararsız, çünkü `watch_max_dirs_total = 61450` politikadan geliyor
(`watch_budget_source: "policy"`). Politika olmasaydı bütçe 1024'ün yarısından
türetilecekti. P0'ın "belgelenen sınır uygulanan sınır değilse" kuralının
tam olarak uyardığı şekil.

## Etkilenen ama ayrı olmayan

- Semantic indeks dna'da `STALE`, sebebi `the_file_index_is_not_current` —
  yani RC-A. Ayrı bir hata değil, RC-A'nın sonucu.
- `bugs.md` 5 (watch.c'yi scanner'a taşımak) RC-A çözülmeden anlamsız; bu
  değerlendirme doğru.

---

# Doğrulama — 2026-08-28 16:15

Yukarıdaki kök nedenler ölçülerek yazılmıştı; burası ne olduğunun ölçümü.
Orijinal metin değiştirilmedi.

## RC-A — build dizinleri yok sayıldı, keşif tavanı artık aşılmıyor,
## olay boşluğu kapanabildi

**Düzeltme:** `/opt/dna/.gitignore`'a `nodus/build-*/` (commit `0e40db3c`).
75 887 iz sürülmeyen dosyanın 75 612'si (%99,6) 32 adet CMake build dizinindeydi;
altlarında **sıfır** tracked dosya var, içerik 32 925 `.make`, 13 682 `.cmake`,
9 781 `.o`, 9 781 `.d` ve derlenmiş ikililer.

**Ölçüldü — zincir kırıldı.**

```
gen 19650: 2271 examined, 2271 hashed, +2 ~6 -19727, 1 commits, 28203 ms
gen 19651: 2271 examined, 2271 hashed, +0 ~0 -0,     0 commits,  1332 ms
gen 19655: 2271 examined, 2271 hashed, +0 ~0 -0,     0 commits,  1345 ms
```

19 727 build dosyası indeksten çıktı. **`event_gap` false oldu ve öyle kalıyor**
— RC-A'nın "asla kapanamaz" dediği bayrak budur, ve zincirin 6-8. adımları
kırıldı.

`index_current` 15:52'de `yes` okundu. Sonra tekrar `false` oldu, ama
**RC-A yüzünden değil**: `pending_full_reconcile`, `atlas_server_overlay_mirror`
(`src/ipc/server.c:445`) tarafından set ediliyor, çünkü iki `--once` geçişinden
sonra scanner çıktı ve heartbeat `ATLAS_SCANNER_MIRROR_MAX_AGE_MS` (5 dakika)
içinde duyulmadı. Bu A13'ün yazılı kuralı — scanner susarsa Atlas o depoyu
güncel saymayı bırakır — ve `bugs.md` 9 numaranın canlı kanıtı. İki bayrağı
ayırmak şart: biri kapandı, diğeri bilerek açık.

İzlenen dizin 13 060 → **294**. dna semantik indeksi STALE → **CURRENT**
(kuşak 563, commit `0e40db3c`).

**Kalıcılık — ölçülmüş bir hata.** Aynı düzeltme 14:17'de commit edilmeden
uygulandı ve **14:37'de silindi**: `.gitignore` tracked bir dosya, `/opt/dna`'da
eşzamanlı çalışma sürüyordu (o sırada 9 değiştirilmiş dosya ve yeni commit'ler
vardı) ve sıradan bir `git checkout` yetti. Arada koşan scanner geçişi yine
98 992 dosya aynaladı — yani "kaynağında kırıldı" iddiası ölçüm olarak doğru,
kalıcılık olarak yanlıştı. Düzeltme ancak commit edildikten sonra tuttu.

## RC-B — put başına açılan SQLite bağlantısı duruyor; değişen tek şey
## kaç kez çağrıldığı

Kod değişmedi: istek başına salt-okunur SQLite bağlantısı, `require_scanner`'ın
`repo_list` tam taraması ve put başına staging yolu açılışı yerinde duruyor.
Değişen, kaç kez çağrıldığı: ayna 98 992 → **23 393** dosya.

**Ölçüldü — şu anki CPU.** Scanner çalışmazken daemon boşta **%0** (30 saniyelik
delta). Oturum sırasında görülen %90, dna'nın semantik yeniden kurulumuydu
(1 220 çeviri birimi) ve o iş bitti. RC-B, scanner sürekli koştuğunda geri
gelir.

## RC-C — yayım hâlâ bütün inode'ları değiştiriyor, ama artık 2 271 dosyayı
## yeniden hash'letiyor, 21 996'yı değil

Kuşak 19651 hâlâ `0 unchanged by identity`: her yayım `renameat` ile bütün
inode'ları değiştiriyor, kimlik önbelleği hâlâ boşa çıkıyor. Maliyet 21 996
dosya / 19 864 ms yerine **2 271 dosya / ~1 300 ms**.

## RC-D — `status` ile `sem-status` ayrı tablolara bakıyor; ortada gerileme
## değil iki sayaç vardı

`atlas code sem-status dna` **4 compile database, 1220 birim** diyor ve şu an
`CURRENT`. `atlas status`'un `0` demesi gerileme değil, A3/A8-CI tarafındaki ayrı
bir sayaç. Kapatıldı.

## RC-E — daemon hâlâ çekirdeğin izleme bütçesini yanlış bildiriyor; politika
## doğru değeri verdiği için şimdilik zararsız

Daemon hâlâ `kernel_max_user_watches: 1024` bildiriyor; gerçek değer 122 910.
Politika `watch_max_dirs_total = 61450` verdiği için zararsız. Açık.

## RC-F — aynı 75 887 dosya gitti, `git ls-files --others` artık zaman aşımına
## uğramıyor

`git ls-files --others --exclude-standard` **60 saniyelik zaman aşımından
12 milisaniyeye** düştü. Aynı 75 887 dosyaydı; aynı düzeltme çözdü.

## Bu oturumda ortaya çıkan yeni maddeler

`bugs.md` 8 (daemon SIGTERM'e 30 sn'de cevap vermiyor, 10 sa 40 dk kesinti) ve
9 (scanner'ın systemd unit'i yok). İkisi de çözülmedi.
