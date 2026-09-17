# Memory OFF/ON karşılaştırması — en küçük plan (2026-09-08)

**Kod değişikliği gerekmiyor.** Bu plan `2026-09-07-memory-offon-benchmark-and-remote-update.md`'nin
T1–T7 görevlerini açmaz; oradaki ölçüm kuralları (§E.4) aynen geçerlidir. Aşağıdaki her
"doğrulandı" bu oturumda koddan veya canlı sistemden okundu; önceki raporlar kanıt sayılmadı.

## 1. Karşılaştırılan mekanizma ve ON/OFF seçimi

- **A10.1 sınırlı çapraz-koşu paketi.** Yerel `atlas job submit --memory bounded` (ON) ve
  `--memory off` (OFF). Paket gönderim işleminde dondurulur (`src/db/db_orch.c:965-973`),
  worker istemine bir kez eklenir; OFF hiçbir şey eklemez.
- A12.1 Context Pack **kapsam dışı**: canlı `system.conf`'ta `memory_source` yok, iki kol da
  pack almaz; ön koşul yapılmadı.
- ON kolunun gerçekten bir şey aldığı **ölçüldü**: bugün 08:59'daki `fake` deneme koşusu
  `rb91ae818…` → `BOUNDED / PRESENT / 2952 bayt / 3 kaynak`. Aday korpusu, M23 öncesi altı
  koşudur (`orch_runs` id 2–7, atlas soyu `d1dd0d0c`, manifest satırı yok); M23 sonrası her
  koşu manifest taşıdığı için korpus sabittir ve yeni koşular onu değiştiremez.
- **Uzak yol ON'u çalıştıramaz**: `job.remote_submit` `memory` alanını reddeder ve modu OFF
  yazar (`src/ipc/server_orch_remote.c:199,347`). Bu yüzden dört koşu da **yerel CLI**'den,
  `nocdem` olarak gönderilir. Önceki worker'ların run kimliğine ulaşamamasının nedeni de bu:
  son `claude` işlerinin hepsi gateway'den (submitter uid 992) gönderilmişti; yerel
  `run-status` başkasının koşusuna "no such run" der.

## 2. Benchmark görevi

`docs/backlog.md` "Claim text is bound with `strlen`, so an embedded NUL truncates it
silently (2026-09-03)" — gerçek, sınırlı, tek komutla doğrulanabilir; A14R-F'nin 15
commit'siz dosyasına dokunmaz. Görev metni **tek tasarımı sabitler** (iki kol tasarımda değil
memory'de ayrılsın). Görev dosyası: `<scratchpad>/memexp-task.txt`, aşağıdaki metin, bayt
bayt aynı dört kolda; `sha256sum` rapora yazılır.

```
Fix the backlog entry "Claim text is bound with strlen, so an embedded NUL truncates it
silently" (docs/backlog.md, 2026-09-03). Choose the intake refusal, not the by-length bind:
in src/verify/intake.c, a claim whose text carries an embedded NUL is refused at intake with
a stated reason, following the precedent src/memory/extract.c already sets for a NUL-bearing
anchor token ("refused rather than shortened"). No row may be written for a refused claim.
Add a regression test tests/test_verify_nul.c with its own ATLAS_TEST_MAIN, wired into the
ATLAS_TESTS list and a set_tests_properties LABELS line (unit) in tests/CMakeLists.txt, so the
build target and the CTest test are both named exactly test_verify_nul. The test proves the
refusal and proves no claim row exists afterwards. Update the backlog entry to say what was
chosen. Do not touch src/db/db.c. Run make and ctest -R test_verify_nul before finishing.
```

Doğrulama, Atlas'ın kendi kapısı (`--gate`, workspace'te dispatcher koşturur, çıktısı worker
akışına girmez): `make` sonra
`ctest --test-dir build -R ^test_verify_nul$ --no-tests=error --output-on-failure`
(`--no-tests=error` yüksüz değil: test yoksa kapı geçmesin). İş `SUCCEEDED` ⇔ çıkış 0 ∧
kapılar geçti; bu mevcut kabul koşuludur, ayrı yama incelemesi yapılmaz.

## 3. Yürütme protokolü (executor, makinede, `nocdem`)

Ön koşullar (hepsi salt okunur kontrol):
1. `git rev-parse HEAD` = `6ecf332…` ve dört gönderim boyunca **commit yok** (snapshot
   pinlenen commit'ten alınır, `src/orch/snapshot.c:298-308`; çalışma ağacındaki 15 dosya
   worker'a görünmez → "HEAD ≠ ağaç" kararı gereksiz). `claude-repo`/`job run` **kullanılmaz**:
   canlı ağacı düzenler.
2. `atlas job list --json` içinde terminal olmayan iş yok; uzak steward pencere boyunca
   gönderim yapmaz (tek model dispatcher FIFO'dur, `src/orch/dispatch.c` lease döngüsü).
3. §4'teki operatör kararı uygulanmış ve `atlas.service` yeniden başlatılmış (aksi hâlde
   4 MiB tavanı kabul edilmiş demektir).

Faz 0 — ücretsiz ön kayıt (nihai görev baytlarıyla; iki kalıcı ACTIVE satır bedeli):
```
atlas job submit --repo atlas --driver fake --mode patch --memory bounded --task "$(cat memexp-task.txt)" --idempotency-key memexp.p0.on  --json   # run'ı kaydet
atlas job submit --repo atlas --driver fake --mode patch --memory off     --task "$(cat memexp-task.txt)" --idempotency-key memexp.p0.off --json
atlas job run-status RUN_P0_ON --json   # memory_package_status PRESENT ve memory_package_digest → D0
```
`EMPTY` ise görev metni değiştirilir, ücretli koşu başlamaz.

Faz 1 — iki çift, dört koşu (kollar arka arkaya gönderilir; hiçbiri diğerini göremez:
ACTIVE koşu aday değildir ve manifest taşır). Sıra dengeli: çift 1 OFF→ON, çift 2 ON→OFF.
```
G='--repo atlas --driver claude --mode patch --gate make --gate "ctest --test-dir build -R ^test_verify_nul$ --no-tests=error --output-on-failure"'
atlas job submit $G --memory off     --task "$(cat memexp-task.txt)" --idempotency-key memexp.p1.off --json
atlas job submit $G --memory bounded --task "$(cat memexp-task.txt)" --idempotency-key memexp.p1.on  --json
# ikisi terminal olunca:
atlas job submit $G --memory bounded --task "$(cat memexp-task.txt)" --idempotency-key memexp.p2.on  --json
atlas job submit $G --memory off     --task "$(cat memexp-task.txt)" --idempotency-key memexp.p2.off --json
```
Her yanıtın `run` alanı kaydedilir (`job submit --json` verir: `src/ipc/server_orch.c:162`;
yedek: `orch_jobs.run_uid` salt okunur sorgu). Model politikadan sabit: `executor_model =
claude-sonnet-5`; `max_attempts = 1` → çöken kol tekrar **sorulmadan** koşulmaz.
**İlk ücretli kol** usage yolunun canlı kanıtıdır (`fake` yalnız UNKNOWN gösterir):
`usage_status` `AVAILABLE` değilse ikinci kola geçmeden dur ve nedeni raporla.

Faz 2 — okuma, kol başına `atlas job run-status RUN --json` + `atlas job get JOB --json`:
`state`, `created_at`/`terminal_at`, `memory_mode`, `memory_package_status/digest/bytes`,
`memory_sources`, `usage_status`, `usage_input_tokens`, `usage_output_tokens`,
`usage_cache_creation_tokens`, `usage_cache_read_tokens`, `usage_worker_duration_ms`,
`usage_turns`, `usage_cost_known_micro_usd`, `usage_cost_complete`.

Rapor kuralları: iki ON kolunun `memory_package_digest`'i D0'a eşit olmalı (sabit memory
kanıtı; değilse çift `MEMORY_DRIFTED` işaretlenir, atılmaz). `usage_status != AVAILABLE` →
ilgili hücreler `unknown`, hiçbir yerde 0 yazılmaz; çıktı tavanında ölen kolun `result`
satırı yoktur, UNKNOWN kalır. Bağlam girdisi tek sayıya indirgenmez
(`input + cache_creation + cache_read` ayrı ayrı, çıktı ayrı); maliyet yalnız sağlayıcının
`cost_micro_usd`'ü. İki kol aynı HOME ve aynı eklenti kümesiyle koşar; araç kullanımı
sayılmaz ve zorlanmaz. Betik yok: dört satırlık tablo elle doldurulur.

## 4. Yama gerekip gerekmediği

**Gerekmiyor.** Seçim (`--memory`), run kimliği (`job submit --json` → `run`), ölçüm
(`run-status --json` → `usage_*`, `src/cli/render_json.c:657-697`) ve kabul (`--gate`)
kurulu binary'de var. Karşılaştırmayı gerçekten engelleyen tek şey kod değil, politikadır:

**Operatör kararı — verildi ve uygulandı (2026-09-08 13:57 yerel):** operatör önce 32 MB
istedi; derlenmiş mutlak 16 MiB olduğu ve üstü politikayı MALFORMED yapıp orkestrasyonu
kapattığı için 16 MiB'de karar kılındı. `/etc/atlas/orchestration.conf` düzenlendi, yedek
`orchestration.conf.pre-memexp-20260908`, `atlas.service` yeniden başlatıldı, `daemon ping`
ve `job list` cevap veriyor. Kanıt ilk gönderimde: `orch_jobs.max_output_bytes = 16777216`.
Aşağısı kararın gerekçesidir, olduğu gibi bırakıldı.

`/etc/atlas/orchestration.conf` → `max_output_bytes = 16777216`
(derlenmiş mutlak tavan, `include/atlas/orch.h:320`), ardından `sudo systemctl restart
atlas.service` — ilk gönderimden **önce**. Neden: tavan gönderim anında daemon'un başlangıçta
yüklediği politikadan işe yazılır (`server_orch.c:416` sıfır→tavan; `daemon.c:259` bir kez
yükler) ve lease ile dispatcher'a taşınır; restart'tan önce gönderilen iş 4 MiB'de kalır.
Dispatcher restart'ı gerekmez. Ölçülen bedel: 2026-09-07'de bir worker 34 dakikada 4 MiB
akışa ulaşıp SIGKILL ile öldü, usage UNKNOWN. Alternatif: tavanı olduğu gibi bırakıp ~30
dakikayı aşan kolun ölçümsüz kalmasını kabul etmek. Karar bu ikisinden biri.

## 5. Sonuç — 2026-09-08, dört koşu tamamlandı

Görev sha256 `660ec933…` (916 bayt), commit `6ecf3323`, model `claude-sonnet-5`, sürücü
`claude`/patch, kapılar `make` + `ctest -R ^test_verify_nul$ --no-tests=error`. Faz 0 ON
paketi PRESENT (D0 `1920eb13…`, 2952 B, kaynaklar `r29b70dcf`, `rd45293f2`, `r241979305`);
her iki ücretli ON kolu aynı D0'ı taşıdı. Yeni işlere `max_output_bytes = 16777216` yazıldı.
Dört kol da `SUCCEEDED` (kapılar geçti). Her sayı `orch_usage` satırından, `usage_status`
dördünde de `AVAILABLE`.

| Kol | job | run | state | worker s | turn | input | output | cache_create | cache_read | USD |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| p1.off | `j654fc66f` | `r416e9f75` | SUCCEEDED | 626.1 | 66 | 132 | 30 291 | 190 146 | 8 915 271 | 4.885 |
| p1.on  | `j7dda304f` | `r6adc909e` | SUCCEEDED | 601.6 | 66 | 134 | 33 130 | 165 347 | 9 042 459 | 4.863 |
| p2.on  | `j0d9b5118` | `r6a536901` | SUCCEEDED | 337.4 | 54 | 106 | 27 416 | 168 162 | 7 508 645 | 2.449 |
| p2.off | `j1bcee1ed` | `re2211696` | SUCCEEDED | 547.4 | 61 | 122 | 29 463 | 175 269 | 8 925 969 | 5.079 |

Duvar (created→terminal): 11:01, 10:34, 6:14, 9:39. Sıra çift 1 OFF→ON, çift 2 ON→OFF.
Toplam 17.28 USD.

Gözlem, hüküm değil: iki çiftte de ON kolu daha kısa ve daha ucuz — çift 1'de 24 s ve
0.02 USD (gürültü içinde), çift 2'de 210 s ve 2.63 USD (%38). Farkın neredeyse tamamı tek
koşudan (p2.on) geliyor; n=2 ile bu bir verdict değildir, A10.1'in kendi "USEFUL on time,
not on cost" bulgusuyla yönü tutarlıdır. Bağlam girdisi tek sayıya indirgenmedi.

Dört worker'ın oturum dökümü sayıldı: **sıfır MCP çağrısı**, Atlas araçları dâhil; yalnız
Bash/Read/Edit. İki kol aynı araç kümesiyle koştu. Ölçülen şey A10.1'in leksikal paketidir;
Atlas'ın semantik/yapısal indeksinin worker'a etkisi bu deneyle ölçülmedi, çünkü çalışma
alanı kayıtlı bir depo değildir ve görev metni araç kullanmayı istemedi. Bunun için ayrı bir
deney gerekir: kayıtlı bir klon (`atlas-bench`), `claude-repo`, ON kolunda açık araç talimatı.

## 6. İkinci deney — Atlas'ın kod indeksi (MCP araçları) ON/OFF, 2026-09-08

Operatörün isteğiyle asıl soru ölçüldü: worker Atlas'ın yapısal ve semantik indeksini MCP
araçlarıyla **kullanınca** ne kazanır? Düzenek: `/opt/atlas` HEAD `6ecf3323`'ün klonu
`/opt/atlas-bench` olarak kaydedildi (scanner uid 1000, `orchestration.conf`'a
`repo = atlas-bench`), semantik nesil yayında (21 271 sembol, keşif COMPLETE, tek eksik
üretilmiş `build/atlas_ui_page.c` birimi). Sürücü `claude-repo`, kök iş
`job submit --attempts 1` + `job run --resume` (politika `max_attempts = 1` iken `job run`
kökü 3 denemeyle açtığı için), A10.1 belleği iki kolda da OFF, aynı iki kapı. Aynı görev
metni (§2) + son paragraf: ON kolunda "önce Atlas MCP araçlarıyla yönünü bul" talimatı
(`semexp-task-on.txt`, sha256 `cfb849d7…`), OFF kolunda "hiçbir MCP aracı çağırma"
(`semexp-task-off.txt`, sha256 `262701fd…`). Her kol arası klon `git checkout -- . && git
clean -fd` ile HEAD'e döndürüldü ve scanner/indeksin yetişmesi beklendi. Uyum, worker
oturum dökümlerinden sayıldı: ON kollarında 17–27, OFF kollarında 0 MCP çağrısı.
Dört çift; sıra ON→OFF, OFF→ON, ON→OFF, OFF→ON. Sekiz koşu da `ACCEPTED` (kapılar geçti).

| Çift | Kol | run | worker s | turn | output | cache_create | cache_read | USD | MCP |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | ON  | `rf8574ff1` | 267.7 | 50 | 19 991 | 140 935 | 4 901 849 | 1.744 | 17 |
| 1 | OFF | `r8b5a3f30` | 300.3 | 60 | 21 439 | 128 958 | 7 196 392 | 2.170 | 0 |
| 2 | OFF | `r260aae7f` | 584.3 | 63 | 25 464 | 163 081 | 8 646 011 | 4.774 | 0 |
| 2 | ON  | `re8b4900d` | 319.8 * | 58 | 21 900 | 175 977 | 4 667 645 | 1.857 | 27 |
| 3 | ON  | `rbdd5d417` | 384.6 | 51 | 28 954 | 170 453 | 7 129 870 | 2.398 | 17 |
| 3 | OFF | `r5b3c59ac` | 288.8 | 41 | 17 442 | 134 542 | 4 959 207 | 1.705 | 0 |
| 4 | OFF | `r1667822f` | 297.8 | 44 | 20 317 | 163 077 | 5 389 953 | 1.934 | 0 |
| 4 | ON  | `r25677d1a` | 440.3 | 56 | 28 743 | 167 641 | 7 991 055 | 2.556 | 17 |

\* Çift 2 ON: kök görev 45 s / 16 turn sonra kapıda kaldı, Atlas'ın açtığı bir takip görevi
274 s'de geçti; satır iki denemenin toplamıdır (A11.1 mekanizması, ayrı sayılmadı).

Çift bazında ON'un OFF'a göre farkı (süre / maliyet): çift 1 −%10.9 / −%19.6;
çift 2 −%45.3 / −%61.1; çift 3 +%33.2 / +%40.6; çift 4 +%47.8 / +%32.2.
Ortalama: ON 353.1 s, 2.139 USD, 53.8 turn; OFF 367.8 s, 2.646 USD, 52.0 turn →
ON −%4.0 süre, −%19.2 maliyet. **Medyan:** ON 352.2 s / 2.128 USD, OFF 299.1 s / 2.052 USD →
ON +%17.8 süre, +%3.7 maliyet. Ortalamadaki kazanç tek bir uzun OFF koşusundan (çift 2,
584 s) geliyor; o çift dışarıda bırakılınca ON +%23 süre, +%15 maliyet.

**Hüküm (gözlem, n=4):** bu görevde indeks kullanımı tutarlı bir kazanç vermedi; iki çift
ON, iki çift OFF lehine ve koşudan koşuya değişkenlik (OFF 289–584 s) etkiden büyük.
Nedensel zincir: görev tek dosya + bir test kadar küçük; ON kolu 17 araç çağrısını turn ve
bağlam olarak öder (ON output ortalaması OFF'tan %18 fazla), OFF kolu aynı yeri grep ile
saniyeler içinde buluyor. İndeksin değer üretmesi için kodun yayıldığı, çağrı grafiğine
ihtiyaç duyulan bir görev gerekir; bu deney o soruyu cevaplamaz. Bilinen zayıflıklar: ON
kolları `atlas_record_reason` ile defterde gerekçe bıraktı ve sonraki ON kolları bunu
`atlas_file_context` ile görebilirdi (ON lehine sızıntı; buna rağmen ON kazanmadı);
sekiz koşu aynı saatte, aynı API'de koştu. Toplam 19.14 USD.

## 7. Üçüncü deney — indeks ON/OFF, büyük görev (MCP `additionalProperties` zorlaması), 2026-09-08

§6'nın düzeneği aynen; görev büyütüldü: backlog'daki "Every MCP tool publishes
`additionalProperties: false` but only one enforces it" girişinin içerilmiş düzeltmesi —
dispatch yolunda tek noktadan, şemanın bildirdiği anahtarlardan türetilen izin listesiyle
`atlas_jsonv_check_only_keys`; `tests/test_mcp.c`'ye iki test (her araç fazladan anahtarı
reddeder; bildirilen anahtarlarla çağrı geçer); backlog ve `CLAUDE.md` notu. Görev metinleri
`semexp2-task-on.txt` (sha256 `6e2efab0…`) ve `semexp2-task-off.txt` (`c41ead78…`), fark
yine yalnız son paragraf. Kapılar `make` + `ctest -R ^(test_mcp|test_decision_mcp)$`.
İki çift, sıra ON→OFF, OFF→ON. Dört koşu da `ACCEPTED`; her kol 7–8 dosyaya dokundu.

| Çift | Kol | run | worker s | turn | output | cache_create | cache_read | USD | MCP |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | ON  | `r44109cf8` | 1067.4 | 100 | 77 419 | 223 600 | 16 586 261 | 4.986 | 5 |
| 1 | OFF | `r318667af` | 949.1 | 91 | 63 684 | 201 159 | 14 072 375 | 6.880 | 0 |
| 2 | OFF | `r8e456761` | 1123.4 | 86 | 61 747 | 213 940 | 12 808 729 | 6.134 | 0 |
| 2 | ON  | `rc3dd697a` | 866.8 | 81 | 63 501 | 217 938 | 13 590 183 | 4.225 | 13 |

ON'un OFF'a göre farkı (süre / maliyet / turn): çift 1 +%12.5 / −%27.5 / +%9.9;
çift 2 −%22.8 / −%31.1 / −%5.8. Ortalama: süre −%6.7 (967 s / 1036 s), maliyet −%29.2
(4.61 / 6.51 USD), turn +%2.3, cache_read +%12.3, output +%12.4. Toplam 22.23 USD.

**Gözlem, hüküm değil (n=2):** süre bir çiftte ON, bir çiftte OFF lehine; sağlayıcının
bildirdiği maliyet iki çiftte de ON lehine. **Ancak maliyet sütunu token sütunlarıyla aynı
yönde değil:** ON kolları daha fazla output ve cache_read token harcamışken daha ucuz
görünüyor (ON ≈ 0.30 USD / 1M cache_read, OFF ≈ 0.48; §6'daki sekiz koşunun hepsi ≈ 0.33–0.36).
Bu farkın kaynağı bu oturumda açıklanamadı; sağlayıcı `total_cost_usd` alanı olduğu gibi
yazıldı, ama karar için token ve süre sütunları daha güvenilirdir ve onlar net bir kazanç
göstermiyor. Worker büyük görevde indeksi daha az kullandı (5 ve 13 çağrı; küçük görevde
17–27): yönünü araçlarla bulup gerisini dosya okuyarak yaptı. İki deney birlikte, on altı
koşu: **indeks kullanımı bu iki görevde süre ve token açısından tutarlı bir kazanç
sağlamadı; kayıp da göstermedi.** "Bütün memory'yi indeksin üstüne kurma" kararı bu veriyle
desteklenmiyor; kararı veriyle vermek için, doğru sonucun indeks olmadan zor bulunduğu bir
görev sınıfı (çağrı grafiği boyunca etki analizi gerektiren bir değişiklik) ve daha çok
çift gerekir.

**§7 eki — maliyet/token uyuşmazlığının nedeni (aynı gün, dökümlerden hesaplandı).**
Dört kolun oturum dökümleri mesaj kimliğine göre tekilleştirilip toplandı; Atlas'ın
`orch_usage` satırlarıyla birebir aynı. Sonnet 5 tarifesiyle (input 2, output 10, önbellek
okuma 0.20, 1 saatlik önbellek yazımı 4 USD/M) hesaplanan maliyet ON kollarında sağlayıcı
rakamına kuruşuna kadar eşit (4.986 ve 4.225 USD). OFF kollarında formül 4.256 ve 4.035 USD
verir; sağlayıcı 6.880 ve 6.134 USD. Fark (2.62 ve 2.10 USD) her OFF kolunda bağlamı 300K'yı
aşan **tek bir mesajdan** geliyor (423 695 ve 336 512 token; ON kollarında en yüksek bağlam
248 770 ve 243 108). O mesajın usage satırı dökümde `claude-fable-5-1` etiketli, diğer her
mesaj `claude-sonnet-5`; ek ücret, o mesajın tüm girdisinin önbellek indirimsiz ≈6 USD/M'den
fiyatlanmasına denk (2.52 ve 1.94 USD). Sunucu tarafı model yedeklemesi mi, uzun bağlam
primi mi, dökümden ayırt edilemez. Nedensel zincir: OFF worker'ları 4 800 satırlık
`src/mcp/mcp_tools.c`'yi bütün okuyup bağlamı şişirdi; ON worker'ları indeksle nokta atışı
sorgulayıp 250K altında kaldı. **Düzeltilmiş okuma:** büyük görevdeki %29 maliyet avantajı
gerçek para, ama "indeks daha az token harcadı" değil "OFF kolu bağlamı bir kez 300K'yı
aşırdı" demektir; tek isteğe bağlı, n=2, ve dosyaları parça parça okutan bir talimatla da
önlenebilir. Süre ve turn tarafında yine tutarlı fark yok.
