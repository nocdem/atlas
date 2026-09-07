# Memory OFF/ON Karşılaştırması ve Uzaktan Güncelleme — Uygulama Planı

> **Yürütücü için:** Bu planı görev görev uygulamak için `superpowers:subagent-driven-development`
> (önerilen) ya da `superpowers:executing-plans` kullanın. Adımlar `- [ ]` ile işaretlidir.
> **Bu planda commit adımı yoktur.** Operatör commit/push yapmayı yasakladı; entegrasyon
> operatörün kararıdır. Her görev "derleme + hedefli test + diff okuması" ile kapanır.

**Amaç:** Bir iş gönderiminde memory seçiminin (OFF/ON) her seferinde ayrı, MCP üzerinden
yapılabilmesi; açık OFF'ta hem A10.1 çapraz-koşu paketinin hem A12.1 Context Pack'in
kapanması; talep edilen ve uygulanan durumun rapordan okunması; gerçek model koşularında
token/süre/maliyet/doğruluk farkını ölçen eşleştirilmiş OFF/ON düzeni; ve operatör onaylı
güncelleme kurulumu/restart akışının mevcut yetenekler üzerinden yazılması.

**Mimari:** Seçim `atlas_orch_op` üzerinde yeni bir üç değerli alan (`memory_select`:
UNSET/OFF/ON) olarak taşınır, `ATLAS_ORCH_SPEC_DOMAIN` yerinden oynamaz. Yazma noktasında
UNSET bugünkü davranışı üretir; OFF her iki paketi bastırır; ON A10.1'i BOUNDED yapar ve
Context Pack'i bugünkü kuralla kurar. Talep edilen değer `orch_run_memory.requested`
kolonuna (migration 33) dondurulur; uygulanan durum zaten var olan `memory_package_*`
alanları ile bu planda eklenen `context_pack_*` alanlarından okunur. Uzak yol
(`job.remote_submit`, gateway rotası, `atlas_job_submit` MCP aracı) aynı alanı `memory`
adıyla, yalnızca `off|on` sözlüğüyle kabul eder.

**Teknoloji:** C17, SQLite, mevcut `atlas_test.h` harness'ı, bash (Python yok).

**Spec:** Operatörün bu oturumdaki altı kesin gereksinimi (aşağıda §A'da madde madde) ve
`docs/remote-submission.md` (A14/A14R kararları), `docs/orchestration.md` (A10.1),
`docs/context-reconciliation.md` (A12.1).

---

## A. Gereksinimlerin mevcut durumu ve somut eksikler

Her madde: doğrulanan kaynak → mevcut durum → eksik. "Doğrulandı" = bu oturumda dosyadan
veya canlı sistemden okundu; "önceki rapor" = operatörün aktardığı, burada doğrulanmayan.

### A.1 Gönderim başına memory seçimi, MCP üzerinden

Doğrulandı:

- A10.1 modu `atlas_orch_memory_mode {UNKNOWN=0, OFF, BOUNDED}`
  (`include/atlas/orch_memory.h:74-78`). Yerel `job.submit` `"memory"` parametresini
  `off|bounded` olarak alır; yok ise OFF (`src/ipc/server_orch.c:295-317`). CLI
  `--memory` verilmediğinde parametreyi **göndermez** (`src/core/service_orch.c:145`,
  boş değer atlanır) — bu, UNSET'i ayırt etmeyi mümkün kılar.
- `job.remote_submit` `"memory"` adını **reddeder** (`FORBIDDEN[]`,
  `src/ipc/server_orch_remote.c:190-191`) ve modu sabit OFF yazar (`:339`).
- MCP `atlas_job_submit` şeması yalnızca `repo`, `task`, `key` (`src/mcp/mcp_tools.c:3627-3658`)
  ve `job.remote_submit`'e iletir. Gateway rotası da yalnızca `{"repo","task","key"}`
  iletir (`src/gw/gateway.c:1073`).
- Canlı binary'de `remote_submit_memory` dizesi **yok** (strings sayımı 0); worker'ın kısmi
  işi yalnızca politika satırı ekliyordu, istek alanı değil.

Eksik: istek alanı yok; UNSET/OFF/ON ayrımı yok; MCP/gateway iletmiyor.

### A.2 Açık OFF'ta iki paketin de kapanması; talep edilen ve uygulanan durumun raporu

Doğrulandı:

- Context Pack, `run_orch_build_pack` (`src/daemon/writer.c:1233-1268`) tarafından yalnızca
  kök SUBMIT'te ve sistem politikasında `memory_source` varsa kurulur; **`memory_mode`'a
  bakmaz**. Yani bugün yerel `--memory off` bile pack alır.
- Canlı `/etc/atlas/system.conf`'ta `memory_source` satırı **yok** → canlı sistemde hiçbir
  koşu Context Pack almıyor. ON kolu bu haliyle yalnızca A10.1 paketini deneyebilir.
- A10.1 aday kümesi: `orch_runs.status IN ('ACCEPTED','BLOCKED')` ve `orch_run_memory`
  satırı **olmayan** koşular (`src/db/db_orch_memory.c:259-263`). `atlas_db_orch_memory_freeze`
  OFF modunda da satır yazar (`db_orch_memory.c:412-480`, INSERT koşulsuz). Nedensel zincir:
  migration 23'ten sonra oluşturulan her koşu satır alır → hiçbiri aday olamaz → korpus
  M23 öncesi koşularla sabitlenmiştir. `atlas` soyunda böyle bir koşu var mı, bilinmiyor;
  §E Faz 0 bunu sıfır maliyetle ölçer.
- Rapor: `job.run_status` A10.1 alanlarını (`memory_mode`, `memory_package_status/digest/bytes`,
  `memory_source_count`, `memory_sources`) verir; Context Pack hakkında **hiçbir şey vermez**
  (`src/ipc/server_orch.c:979-1013`). `atlas memory pack --run` bu kurulumda çalışmaz
  (soket üzerinden sunulmuyor; sistem indeksi `atlasd`'ye ait).
- Yerel `job get` **`run` anahtarı vermez** (JSON'da anahtar listesi doğrulandı); uzak bir işin
  run kimliğine yerel operatör ulaşamaz, dolayısıyla `run-status`'a da.

Eksik: pack bastırma kuralı; talep edilen değerin saklanması; `context_pack_*` raporu;
`job get` → `run`; uzak okuma yüzeyinde memory özeti.

### A.3 Karşılaştırılabilir koşullar

Doğrulandı:

- Worker snapshot'ı **HEAD commit'inden** alınır, çalışma ağacından değil: başarısız işin
  `source/` dizininde `src/orch/jobresult.c` yok ve `server_orch_remote.c` A14R metnini
  içermiyor; oysa canlı binary A14R dizelerini içeriyor (`remote_submit_max_active_total`,
  `job.remote_result` sayımı 1). Nedensel zincir: worker HEAD'i düzenler, ama Atlas
  araçları çalışma ağacının indeksini cevaplar → iki farklı kaynak. Deney öncesi
  HEAD == çalışma ağacı ön koşuldur (§G.2).
- Worker, `model_credential = operator_session` altında operatörün gerçek HOME'uyla başlar
  (`src/orch/driver.c:603-611`); Claude Code bu HOME'daki eklentileri yükler. Başarısız işin
  `init` kaydı: 8 MCP sunucusu bağlı (`plugin:atlas:memory`, `filesystem`, `puppeteer`,
  `fetch`, Google Drive/Gmail/Calendar, `sequential-thinking`), 70 `mcp__` aracı,
  `permissionMode: acceptEdits`, model `claude-sonnet-5`, Claude Code 2.1.263.
- Claude Code'un otomatik proje belleği çalışma dizinine göre anahtarlanır; her iş ayrı
  `work/` dizininde çalıştığı için `~/.claude/projects/<workspace>/memory/` her işte boş
  başlar (31 böyle dizin var, doğrulandı). Kollar arası taşınma yok.
- SessionStart kancaları (superpowers vb.) her worker'a aynı metni enjekte eder: iki kolda
  eşit sabit, kirlenme değil; fakat çıktı hacmine katkısı büyük (A.4'e bakın).
- `orch_runs` manifesti koşu oluşturulurken dondurulur (`src/db/db_orch.c:954-971`);
  pack de aynı işlemde (`:986-995`). ON kolları arasında `memory_package_digest` ve
  `context_pack_digest` eşitliği "sabit memory"nin kanıtıdır.

Eksik: kod değil, protokol: ön koşul listesi ve eşitlik kontrolleri (§E Faz 1–2).

### A.4 Gerçek model ölçümleri

Doğrulandı:

- `orch_usage` (`src/db/migrate.c:3808-3847`): `input_tokens`, `output_tokens`,
  `cache_creation_tokens`, `cache_read_tokens`, `cost_micro_usd`, `duration_ms`,
  `api_duration_ms`, `turns`; hepsi NULL'lanabilir, NULL = gözlenmedi.
- Kaynak: `atlas_usage_from_stream` (`src/orch/usage.c:179-241`) stream-json'daki **son**
  `"type":"result"` satırından okur; `canonicalModel`, `total_cost_usd`, `num_turns`,
  `duration_ms`, `duration_api_ms`, dört token alanı. `AVAILABLE` ancak altı alan tam ise.
- Yüzeyler: `job.run_status` → `usage_status`, `usage_attempts_started/measured/missing`,
  `usage_input_tokens`, `usage_output_tokens`, `usage_cache_creation_tokens`,
  `usage_cache_read_tokens`, `usage_worker_duration_ms`, `usage_turns`,
  `usage_tokens_complete`, `usage_cost_known_micro_usd`, `usage_cost_complete`
  (`src/cli/render_json.c:657-699`). `job.remote_get` → `usage{present, model, has_cost,
  cost_micro_usd, has_turns, turns}`; `job.remote_result` → `usage{..., complete,
  incomplete_reason}`.
- **Başarısız işin nedeni üç tutarlı gözlemden çıkarıldı, defterden okunmadı:**
  `logs/stdout.log` 4 194 034 bayt (sınırın 270 bayt altı; `proc.c` sınırı sink'e yazmadan
  önce kontrol eder); politika `max_output_bytes = 4194304`; `src/core/proc.c:449-456` sınır
  aşımında SIGKILL ve "output of claude exceeded the 4194304 byte limit". Logda `result`
  olayı **yok** (sayım 0), son mesaj 17:00:25Z, FAILED 17:00:26Z. Nedensel zincir:
  stream-json + operatör HOME'unun kancaları/araç sonuçları → 34 dakikada 4 MiB → çocuk
  öldürüldü → usage UNKNOWN, artefakt yok, iş FAILED. Doğrulama: defterin kendi `reason`
  alanı steward'ın bir `job.remote_get` çağrısı uzağındadır; yerel `job get` nedeni göstermez. Ölçülen hız ≈ 2 KB/s; derlenmiş mutlak tavan 16 MiB
  (`include/atlas/orch.h:320`) ≈ 2 s 15 dk; duvar tavanı 3 s. Deneyde bu düzeltilmezse
  30 dakikayı geçen her kol aynı yerde ölür.

Eksik: kod değil: `max_output_bytes` politikası (operatör), toplama/rapor betiği,
"bilinmiyor" kuralı ve önbellek alanlarını çift saymayan toplam tanımı (§E Faz 3).

### A.5 Atlas'tan başlatılma ≠ Atlas aracı kullanma ≠ zorunluluk

Doğrulandı:

- Zorlama **yok**: `src/orch/*.c` ve IPC dosyalarında `mcp__`/`atlas_` araç adı geçmez;
  tek araç-adı tanıma `PROGRESS_TOOLS` (`driver.c:130-131`, yalnızca yerleşik araçlar,
  yalnızca boşta-zamanaşımı saati için). Argv'de `--mcp-config`/`--allowedTools` yok
  (`driver.c:495-543`); worker'a Atlas aracını veren şey Atlas kodu değil, operatör HOME'undaki
  eklenti kaydıdır.
- Gerçek kullanım kanıtı: Atlas'ın kendi yakaladığı `logs/stdout.log` (stream-json,
  `tool_use` kayıtları adlarıyla) ve Claude Code'un operatör HOME'undaki oturum dökümü
  (`~/.claude/projects/<workspace-anahtarı>/<oturum>.jsonl`). İkisi de sayıldı: başarısız
  işte 88 Bash, 45 Read, 33 Edit, **0 `mcp__` çağrısı**; kurtarma işinde de **0**. Yani iki
  worker da Atlas tarafından başlatıldı ve Atlas'ın hiçbir aracını kullanmadı.
- Çalışma alanı başarıda silinir; stream logu yalnızca başarısızlıkta diskte kalır. Oturum
  dökümü her durumda kalır ama operatörün kişisel verisidir; A8.1 gereği Atlas onu okumaz.
  Sayım betiği bu yüzden operatör tarafından çalıştırılır (§D T6).

Eksik: kod değil; sayım betiği ve raporda "zorunlu değil, gözlendi/gözlenmedi" ifadesi.

### A.6 Operatör onaylı uzaktan güncelleme ve restart

Doğrulandı:

- Var: `job.remote_submit/get/result/list/cancel` (gateway + uzak MCP), `changes.patch`
  dahil üç adlı sonuç dosyası satır içi ≤ 320 KiB (A14R). `deploy.local.sh` kurulum +
  yalnızca `atlas.service` restart'ı yapıyor. Beş birim var: `atlas.service`,
  `atlas-gateway.service`, `atlas-dispatcher.service`, `atlas-scanner.service` (sistem) ve
  `atlas-model-dispatcher.service` (kullanıcı). Hiçbirinde `PartOf`/`BindsTo` yok → daemon
  restart'ı diğer dördünü eski binary'de bırakır (15:58'de hepsi elle yeniden başlatılmış).
- `nocdem` `(ALL) NOPASSWD: ALL`; model dispatcher birimi sandbox'sız. Bugün teknik olarak
  hiçbir şey bir worker'ın `sudo systemctl restart atlas` çalıştırmasını engellemiyor; gate
  izin listesi (`src/orch/validate.c:27-38`) `sudo`/`systemctl`'i yalnızca **gate programı**
  olarak reddeder. A14R kuralı `job.remote_apply`'ı yasaklar; uzaktan uygulama/kurulum/restart
  **tasarım gereği yok** ve bu plan da eklemez.
- Yerel operatörün saklanmış bir uzak iş sonucunu okuyacağı CLI yok (`atlas job` alt
  komutları: `submit|run|run-status|get|list|cancel`); tarayıcı Jobs görünümü veya uzak
  MCP okur.

Eksik: `deploy.local.sh`'ın beş birimi de yeniden başlatıp doğrulaması; çalışan bir model
işi varken restart'ı reddetmesi; isteğe bağlı yerel `atlas job result` (§D T7).

---

## B. Güncel kaynak temelini nasıl kullanacağız

**Canlı = çalışma ağacı.** `/usr/local/bin/atlas` 15:50:52'de kuruldu; çalışma ağacındaki
en yeni değişiklik 15:50:45 (`src/ipc/server_orch_remote.c`); daemon 15:58:45'te başladı;
son tam suite 15:58, **125/125 geçti** (`build/Testing/Temporary/LastTest.log`). Sağlama
toplamları eşit değildir (cmake --install RPATH'i soyar; bilinen tuzak); içerik doğrulaması
A14R dizeleriyle yapıldı. Sonuç: canlı sistem = HEAD `2922214` + commit edilmemiş A14R
değişiklikleri (28 dosya, +1779/−52, iki yeni dosya).

**Worker snapshot'ı = HEAD `2922214`**, A14R'siz. Eski snapshot güncel kaynak değildir;
executor **/opt/atlas çalışma ağacında**, A14R'nin üstünde çalışır. Toplu commit, reset
veya üzerine yazma yok. İlk adım güvenlik kopyası:
`git diff HEAD > <scratchpad>/a14r-baseline-2026-09-07.patch` ve iki izlenmeyen dosyanın
kopyası.

**Kısmi işin (18 dosya, 991 satır diff) ayıklanması** — tek parça `git apply --check`
çalışma ağacına karşı (her başarısız dosyayı ayrı bildirir): 14 dosya temiz uygulanır, 4
dosya çakışır (`include/atlas/gwpolicy.h`,
`src/gw/gateway.c`, `src/gw/gwpolicy.c`, `tests/test_gateway.c`). Çakışan dörtlü tam
olarak operatörün "tek başına yeterli değil" dediği global politika satırı
(`remote_submit_memory`) parçalarıdır.

| Parça | Karar | Gerekçe |
| --- | --- | --- |
| `gwpolicy.h/.c`, `gateway.c`, `test_gateway.c` (politika satırı) | **At** | Gereksinim 1 istek alanı istiyor; politika satırı ikinci bir mekanizma olur. Zaten çakışıyor. |
| `deploy/a9/gateway.conf.template`, `docs/backlog.md` §, `docs/remote-submission.md` §8, `docs/orchestration.md` § | **At**, yeniden yaz | Metin politika satırını anlatıyor; T5 doğru metni yazar. |
| `src/daemon/writer.c` pack bastırma | **Uyarla** | Fikir doğru, yüklem yanlış: `remote_allowed_count > 0` yola bağlı; yerel açık OFF pack'i almaya devam ederdi. Yüklem `op->memory_select == OFF` olacak. |
| `src/ipc/server_orch_remote.c` mod atama | **Uyarla** | Politika boolean'ı yerine istek alanı. |
| `src/ipc/server_orch.c` pack raporu | **Al**, `context_pack_present` açık boolean ekle | "Uygulanan durum" gereksinimini karşılar. |
| `include/atlas/service.h`, `service_orch.c`, `render_human.c`, `render_json.c` | **Al**, `memory_requested` ekle | Aynı. |
| `tests/test_orch_memory.c` tarama testi | **Al** | Her iki renderer'da alan varlığını tarar. |
| `tests/test_memory_pack_live.c` iki vaka | **Al, uyarla** | `submit_remote` seçim alanını kursun; üçüncü vaka (UNSET pack'i korur) eklensin. |
| `tests/test_orch_remote_rpc.c` | **Uyarla** | Politika yerine istek alanı; kötü değer reddi eklensin. |

Kısmi işin iddia ettiği test sonuçları (124/125, `test_plugin` yürütme-biti hatası) bu planda
kanıt sayılmaz; her şey bu ağaçta yeniden koşulur.

---

## C. Küresel kısıtlar

- Migration **33** eklenir (`ATLAS_SCHEMA_VERSION 32 → 33`, `include/atlas/db.h:24`). Yedi
  test dosyası 32'yi sabitler: `tests/test_migrate7.c:288`, `test_migrate8.c:294`,
  `test_migrate9.c:62`, `test_migrate29.c:263`, `test_migrate31.c:264`,
  `test_migrate32.c:218`, `test_migrate_scanner_uid.c:45` — hepsi 33 olur.
- Yeni iş parçacığı, süreç, zamanlayıcı, arka plan döngüsü yok. Yeni RPC yöntemi yok. Yeni
  MCP aracı yok (var olan `atlas_job_submit` bir alan kazanır). Yeni gateway rotası yok.
  Yeni scope yok. Yeni yetki fiili yok. `atlas_decision_apply_in_tx` üç çağırıcıda kalır.
- `ATLAS_ORCH_SPEC_DOMAIN` yerinden oynamaz: seçim `atlas_orch_op` üzerinde taşınır,
  `atlas_orch_spec` üzerinde değil (A10.1 kuralı).
- `job.remote_submit` isteğinde `memory` yalnızca `off` | `on`; başka her yazım MALFORMED
  reddi. `driver`, `mode`, `validation`, `parallel`, `parent` FORBIDDEN'da kalır.
- Uygulanan durum raporu **bayt ve özet** taşır, paket içeriğini asla (A10.1 kuralı).
- `ATLAS_WERROR=ON`; uyarı bastırma yok. Python/Node yok. `atlas_proc_run` dışında süreç yok.
- Depo adı/yolu ürün mantığına girmez.
- Hiçbir adım commit, push, kurulum, politika düzenleme veya servis restart'ı yapmaz.
- Ücretli model koşusu bu planın hiçbir görevinde başlatılmaz; §E'deki deney operatör
  kararıyla, ayrıca çalıştırılır.

---

## D. Executor görevleri (sıralı, sınırlı)

Dağıtım: T1 → T2 → (T3 ‖ T4 ‖ T6) → T5 → T7. T3/T4/T6 birbirinden bağımsızdır, tek
dağıtımda çıkar. İnceleme yalnızca T2 (güven sınırı: FORBIDDEN, rota tablosu, MCP şeması) ve
T3'ün `job.remote_get` parçası (Decision 7 revizyonu) için; diğerleri test + diff okumasıyla
kapanır. Model: Sonnet yazar; T2 incelemesi Opus.

### T0: Temel kopya ve derleme sağlığı

**Dosyalar:** yok (yalnızca scratchpad).

- [ ] **Adım 1:** `git status --short` çıktısını kaydet; `git diff HEAD > <scratchpad>/a14r-baseline-2026-09-07.patch`;
      `cp include/atlas/orch_jobresult.h src/orch/jobresult.c <scratchpad>/`.
- [ ] **Adım 2:** `make` → `build/atlas --version` `phase A14` yazmalı. `cd build && ctest -L unit --output-on-failure` → tamamı geçmeli.
      Beklenen: geçer (15:58'de 125/125 geçmişti).

### T1: Seçim sözlüğü, op alanı, yazma noktası, migration 33, pack bastırma

**Dosyalar:**
- Değiştir: `include/atlas/orch_memory.h` (enum + iki fonksiyon bildirimi)
- Değiştir: `src/orch/memory.c` (ad/parse tabloları)
- Değiştir: `include/atlas/orch_ops.h` (`atlas_orch_op.memory_select`, yeni okuyucu bildirimi)
- Değiştir: `src/ipc/server_orch.c:295-317` (`job.submit` parametre yorumu)
- Değiştir: `src/cli/cli.c:620-625` ve yardım metni `:60,66` (`--memory off|on|bounded`)
- Değiştir: `src/db/migrate.c` (M33), `include/atlas/db.h:24`
- Değiştir: `src/db/db_orch_memory.c` (INSERT'e `requested`; yeni `atlas_db_orch_memory_requested`)
- Değiştir: `src/db/db_orch.c:954-971` (freeze çağrısına seçim)
- Değiştir: `src/daemon/writer.c:1233-1268` (`run_orch_build_pack` bastırma)
- Test: `tests/test_orch_memory.c`, `tests/test_migrate33.c` (yeni; `tests/CMakeLists.txt`'e ekle + `unit` etiketi), `tests/test_memory_pack_live.c`
- Yedi migration testinde 32 → 33 (§C).

**Arayüzler (sonraki görevler bunlara dayanır):**

```c
/* include/atlas/orch_memory.h */
typedef enum atlas_orch_memory_select {
    ATLAS_ORCH_MEMORY_SELECT_UNSET = 0, /* not stated: today's behaviour, unchanged */
    ATLAS_ORCH_MEMORY_SELECT_OFF,       /* both packages suppressed */
    ATLAS_ORCH_MEMORY_SELECT_ON         /* A10.1 BOUNDED; Context Pack built as today */
} atlas_orch_memory_select;
const char *atlas_orch_memory_select_name(atlas_orch_memory_select s); /* "UNSET" "OFF" "ON" */
/* Accepts "off", "on" and, for A10.1 compatibility, "bounded" (= ON). Nothing else. */
bool atlas_orch_memory_select_parse(const char *s, atlas_orch_memory_select *out);

/* include/atlas/orch_ops.h, on atlas_orch_op beside memory_mode */
atlas_orch_memory_select memory_select;

/* include/atlas/orch_ops.h */
atlas_status atlas_db_orch_memory_requested(atlas_db *db, const char *run_uid,
                                            atlas_orch_memory_select *out, bool *have,
                                            atlas_err *err);
```

Yazma noktası kuralı (tek yerde, `server_orch.c` `job.submit` ve T2'de `job.remote_submit`
aynı iki satırı kullanır):

```c
op->memory_mode = (op->memory_select == ATLAS_ORCH_MEMORY_SELECT_ON)
                      ? ATLAS_ORCH_MEMORY_MODE_BOUNDED
                      : ATLAS_ORCH_MEMORY_MODE_OFF;
```

Migration 33:

```c
static const char M33_MEMORY_REQUESTED[] =
    "ALTER TABLE orch_run_memory ADD COLUMN requested TEXT NOT NULL DEFAULT ''"
    " CHECK(requested IN ('','OFF','ON'));";
static const char *const M33_STATEMENTS[] = {M33_MEMORY_REQUESTED, NULL};
/* table entry: */
{33, "what a submission asked of memory, beside what the run was given", M33_STATEMENTS, false},
```

`''` = M33 öncesi satır veya UNSET; `atlas_db_orch_memory_requested` `''`'ı UNSET olarak okur.

Pack bastırma (`writer.c` `run_orch_build_pack`, mevcut guard'ın hemen altına):

```c
/* An explicit OFF suppresses the Context Pack whatever the channel. UNSET keeps
 * today's rule: built whenever the repository has a generation. */
if (op->memory_select == ATLAS_ORCH_MEMORY_SELECT_OFF) {
    return;
}
```

- [ ] **Adım 1 (kırmızı):** `tests/test_orch_memory.c`'ye ekle:

```c
static void test_memory_select_parses_three_spellings_and_nothing_else(void) {
    atlas_orch_memory_select s = ATLAS_ORCH_MEMORY_SELECT_ON;
    T_CHECK(atlas_orch_memory_select_parse("off", &s) && s == ATLAS_ORCH_MEMORY_SELECT_OFF);
    T_CHECK(atlas_orch_memory_select_parse("on", &s) && s == ATLAS_ORCH_MEMORY_SELECT_ON);
    T_CHECK(atlas_orch_memory_select_parse("bounded", &s) && s == ATLAS_ORCH_MEMORY_SELECT_ON);
    T_CHECK(!atlas_orch_memory_select_parse("ON", &s));
    T_CHECK(!atlas_orch_memory_select_parse("", &s));
    T_CHECK(!atlas_orch_memory_select_parse("unset", &s));
    T_CHECK(strcmp(atlas_orch_memory_select_name(ATLAS_ORCH_MEMORY_SELECT_UNSET), "UNSET") == 0);
}
```
      TESTS tablosuna `{"memory select parses three spellings and nothing else", ...}` ekle.
- [ ] **Adım 2:** `cd build && cmake --build . --target test_orch_memory` → derleme hatası (sembol yok). Beklenen: FAIL.
- [ ] **Adım 3:** Enum, ad ve parse tablolarını `src/orch/memory.c:20-45`'teki mod tablolarının yanına yaz. Derle, test geçsin.
- [ ] **Adım 4 (kırmızı):** `tests/test_migrate33.c`'yi `tests/test_migrate32.c`'yi şablon alarak yaz: (a) taze veritabanı 33'e ulaşır ve `PRAGMA table_info(orch_run_memory)` `requested` kolonunu gösterir; (b) 32'de durmuş bir veritabanı (M32 sonrası bir `orch_run_memory` satırı ile) 33'e kayıpsız geçer ve o satırda `requested = ''` okunur. `T_EQ_INT((int)ATLAS_SCHEMA_VERSION, 33)`. CMake'e ekle (ATLAS_TESTS + `unit` etiketi).
- [ ] **Adım 5:** Yedi migration testindeki `32` sabitlerini `33` yap; `ATLAS_SCHEMA_VERSION 33`; M33'ü tabloya ekle. `ctest -R 'test_migrate' --output-on-failure` → hepsi geçer.
- [ ] **Adım 6:** `orch_ops.h`'e `memory_select` ekle; `server_orch.c` `job.submit`: `"memory"` varsa `atlas_orch_memory_select_parse` ile oku (geçersiz → mevcut "unrecognised" reddi), yoksa UNSET; ardından iki satırlık mod kuralı. `:296-306` arasındaki "holds memory at OFF" yorumunu T2'yi bekleyerek "see T2" demeden, doğru cümleyle güncelle: uzak yolun aynı alanı kendi sözlüğüyle aldığını yazacaksın (T2'de).
- [ ] **Adım 7:** `db_orch.c` freeze çağrısına `op->memory_select` geçir; `atlas_db_orch_memory_freeze` imzasına `atlas_orch_memory_select requested` parametresi ekle; INSERT'e `requested` bağla. `atlas_db_orch_memory_requested` okuyucusunu yaz (tek SELECT).
- [ ] **Adım 8:** CLI: `--memory off|on|bounded` kabul et (`on` yeni), değeri olduğu gibi gönder (mevcut koşullu gönderim korunur). Yardım metnini güncelle.
- [ ] **Adım 9 (kırmızı):** `tests/test_memory_pack_live.c`: worker'ın `submit_remote` yardımcısını al ama adını `submit_with_select` yap, `remote_*` alanlarını **kurma**, yalnızca `op->memory_select = sel;` ve mod kuralını uygula. Üç vaka:
      - `test_unset_select_keeps_the_pack`: UNSET → lease `context_pack.len > 0` (bugünkü davranış korunur).
      - `test_off_select_delivers_no_pack_and_no_package`: OFF → `context_pack.len == 0`, `context_pack_status.len == 0`, `atlas_db_orch_memory_get` modu OFF, `atlas_db_orch_memory_requested` OFF.
      - `test_on_select_delivers_the_pack_and_freezes_bounded`: ON → pack var, mod BOUNDED, requested ON.
      Yorumlarda worker'ın "remote_allowed_count" gerekçesini **taşıma**; yeni gerekçe: yüklem yoldan bağımsızdır.
- [ ] **Adım 10:** `writer.c` bastırmasını yaz. `ctest -R 'test_memory_pack_live|test_orch_memory' --output-on-failure` → geçer.
- [ ] **Adım 11:** `ctest -L unit` ve `ctest -R 'test_orch_rpc|test_orch_run|test_orch_driver' --output-on-failure` → geçer. Diff'i oku; yalnızca listelenen dosyalar değişmiş olmalı.
- [ ] **Adım 12 (bulgu, kod değil):** Birleşik istem tek bir argv öğesi olarak `claude`'a
      gider (`driver.c:495-543`, son öğe); Linux tek argv dizesini 131072 baytla sınırlar.
      `ATLAS_ORCH_TASK_MAX` 65536 + `ATLAS_ORCH_MEMORY_MAX_BYTES` 12288 + Context Pack'in
      derlenmiş üst sınırı (`include/atlas/memory.h`'de bul) toplamını 131072 ile karşılaştır.
      Aşıyorsa bu, ON kolunun exec'te ölüp OFF kolunun koşması demektir; `fake` sürücüsü
      exec yapmadığı için Faz 0 bunu göremez. Sonucu `docs/backlog.md`'ye (T5) ve §E.1'e
      bir cümleyle yaz; burada çözme.

### T2: Uzak yol — `job.remote_submit`, gateway rotası, MCP şeması

**Dosyalar:**
- Değiştir: `src/ipc/server_orch_remote.c:190-191` (FORBIDDEN'dan `"memory"` çıkar), `:339` (sabit OFF yerine istek alanı)
- Değiştir: `src/gw/gateway.c:1073` (`{"repo","task","key","memory",NULL}`)
- Değiştir: `src/mcp/mcp_tools.c:3627-3658` (şema `memory` enum, `ALLOWED[]`)
- Test: `tests/test_orch_remote_rpc.c` (`:462` listesinden `"memory"` çıkar), `tests/test_gw_submit.c`, `tests/test_mcp.c`
- Docs: `docs/remote-submission.md` Decision 4 açıklaması (T5'te; burada yalnızca kod)

**Arayüz:** İstek `"memory"`: yalnızca `"off"` | `"on"`; `"bounded"` **kabul edilmez** (uzak
sözlük A10.1 eş anlamlısını taşımaz — iki yazım, tek anlam olmasın). Yok → UNSET → bugünkü
uzak davranış (mod OFF, pack bugünkü kuralla).

```c
/* server_orch_remote.c, after the repo/task/key reads. One parser for one field:
 * the same atlas_orch_memory_select_parse job.submit uses, then one explicit
 * refusal of the A10.1 synonym, so the remote vocabulary is exactly off|on. */
const char *memsel = NULL;
op->memory_select = ATLAS_ORCH_MEMORY_SELECT_UNSET;
if (atlas_ipc_param_str(req, "memory", &memsel) && memsel != NULL) {
    if (strcmp(memsel, "bounded") == 0 ||
        !atlas_orch_memory_select_parse(memsel, &op->memory_select)) {
        return atlas_err_set(err, ATLAS_ERR_USAGE,
                             "memory on a remote submission is \"off\" or \"on\"");
    }
}
op->memory_mode = (op->memory_select == ATLAS_ORCH_MEMORY_SELECT_ON)
                      ? ATLAS_ORCH_MEMORY_MODE_BOUNDED
                      : ATLAS_ORCH_MEMORY_MODE_OFF;
```

`gateway.c:958` `GW_API_MAX_PARAMS 8`; rota satırı dört parametreyi alır, yine de eklemeden
önce `:1090` döngüsünün dördüncü girişi gezdiğini okuyarak doğrula.

MCP şeması: `prop_str`/enum yardımcılarıyla `"memory"`, `enum: ["off","on"]`, isteğe bağlı;
`ALLOWED[] = {"repo","task","key","memory",NULL}`; `run_job_submit` alanı olduğu gibi iletir.

- [ ] **Adım 1 (kırmızı):** `tests/test_orch_remote_rpc.c`: worker'ın `test_remote_submit_memory_policy_selects_the_run_mode`'unu al, adı `test_remote_submit_memory_field_selects_the_run_mode`; gateway politikasından `remote_submit_memory` satırını **çıkar**; istek gövdesine `"memory":"on"` koy; okunan mod BOUNDED ve `atlas_db_orch_memory_requested` ON. İkinci vaka `test_remote_submit_refuses_a_memory_spelling_outside_off_on`: `"memory":"bounded"` → yanıt `ok:false` ve mesaj `"off\" or \"on"` içerir; hiçbir iş oluşmadı (`job.remote_list` boş). Üçüncü vaka: alan yok → mod OFF, requested UNSET. `:462`'deki FORBIDDEN listesinden `"memory"`'yi çıkar.
- [ ] **Adım 2:** `ctest -R test_orch_remote_rpc` → FAIL (memory hâlâ reddediliyor).
- [ ] **Adım 3:** `server_orch_remote.c` değişikliğini yaz. Test geçsin.
- [ ] **Adım 4 (kırmızı):** `tests/test_gw_submit.c`: mevcut HTTP gönderim vakasını şablon alarak `test_submit_route_forwards_memory`: gövde `{"repo":..,"task":..,"key":..,"memory":"on"}` → 2xx ve daemon veritabanında mod BOUNDED; `"memory":"yes"` → 4xx. `ctest -R test_gw_submit` → FAIL.
- [ ] **Adım 5:** `gateway.c:1073` parametre listesine `"memory"` ekle. Test geçsin.
- [ ] **Adım 6 (kırmızı):** `tests/test_mcp.c`: `atlas_job_submit` şemasının `"memory"` özelliğini ve `"additionalProperties":false`'u içerdiğini doğrulayan bir vaka (var olan şema-tarama vakalarını şablon al). FAIL.
- [ ] **Adım 7:** `mcp_tools.c` şema + `ALLOWED[]`. `ctest -R 'test_mcp|test_orch_rpc|test_gw_remote'` → geçer (`test_orch_rpc` yasak yöntem adlarını tarar; `job.remote_apply` vb. eklenmedi).
- [ ] **Adım 8:** `server_orch.c:296-306` yorumunu düzelt: "`job.remote_submit` holds memory at OFF" artık yanlış; doğru cümle: uzak yol aynı alanı `off|on` sözlüğüyle alır, `parallel` 1 ve `parent` boş kalır.
- [ ] **Adım 9 (inceleme, Opus):** Şu üç soruya cevap: (1) FORBIDDEN'dan çıkan tek ad `memory` mi? (2) Rota tablosu dışında bir alan soket mesajına girebilir mi? (3) MCP şeması ile `ALLOWED[]` aynı kümeyi mi söylüyor? Bulgu yoksa kapanır.

### T3: Rapor yüzeyleri — `run_status`, renderer'lar, `job get` → `run`, `job.remote_get`

**Dosyalar:**
- Değiştir: `src/ipc/server_orch.c:979-1013` (memory bloğuna `memory_requested`, `context_pack_present` + üç alan; `job.get`'e `"run"`)
- Değiştir: `include/atlas/service.h:1248` civarı (alanlar), `src/core/service_orch.c:1013` civarı (okuma), `src/cli/render_human.c:686`, `src/cli/render_json.c:738`
- Değiştir: `src/ipc/server_orch_remote.c:448-597` (`method_remote_get` → `memory` nesnesi)
- Test: `tests/test_orch_memory.c` (tarama), `tests/test_orch_remote_rpc.c` (remote_get okur)

**Tel biçimi (`job.run_status`, memory bloğunun içinde):**

```
"memory_requested": "UNSET"|"OFF"|"ON",
"context_pack_present": true|false,
"context_pack_generation": <int>, "context_pack_digest": "<sha256>", "context_pack_bytes": <int>   (yalnızca present)
```

`job.get` (yerel): `"run": "<run_uid>"` (boş ise `""`; M21 öncesi işler için boş).

`job.remote_get`: 
```
"memory": {"requested": "...", "mode": "OFF"|"BOUNDED",
           "package_status": "EMPTY"|"PRESENT", "package_bytes": N, "package_digest": "...",
           "context_pack_present": bool, "context_pack_bytes": N, "context_pack_digest": "..."}
```
Bayt ve özet; içerik asla. Bu Decision 7'nin okuma yüzeyini genişletir (durum, neden,
deneme, maliyet + memory özeti) — T5 metni "Decision 7R2" olarak yazar.

**service.h alanları:**
```c
const char *memory_requested;     /* "UNSET"/"OFF"/"ON"; NULL when the block is absent */
bool context_pack_present;
const char *context_pack_digest;
int64_t context_pack_bytes;
int64_t context_pack_generation;
const char *run_uid;              /* job get: the run this job belongs to, "" if none */
```

- [ ] **Adım 1 (kırmızı):** `tests/test_orch_memory.c`: worker'ın `test_both_renderers_carry_every_context_pack_field`'ını al; FIELDS listesine `"memory_requested"` ekle; WIRE listesine `"memory_requested"`, `"context_pack_present"` ekle. FAIL.
- [ ] **Adım 2:** `server_orch.c` memory bloğu: `atlas_db_orch_memory_requested` ile `memory_requested`; `atlas_db_memory_pack_get` ile pack; `context_pack_present` **her zaman** yazılır. `service_orch.c`: `context_pack_present` boolean'ından dallan (worker'ın "generation varsa present" çıkarımını **kullanma**). İki renderer: insan biçimi `memory requested %s` ve `context pack  none` / `context pack  generation %lld, %lld bytes, sha256 %s`; JSON aynı anahtarlar. Test geçsin.
- [ ] **Adım 3:** `job.get`'e `"run"`; `service.h`/`service_orch.c`/iki renderer'a `run`. `tests/test_orch_rpc.c` içindeki `job.get` vakasına `"run"` anahtarı beklentisi ekle (kök iş için boş olmayan `r` + 32 hex).
- [ ] **Adım 4 (kırmızı):** `tests/test_orch_remote_rpc.c`: T2'nin ON vakasının devamı olarak `job.remote_get` çağır; `"memory"` nesnesinde `requested":"ON"`, `mode":"BOUNDED"`, `context_pack_present` bir boolean; `"package"` metni veya `"rendered"` **yok**. FAIL.
- [ ] **Adım 5:** `method_remote_get` genişletmesi. Test geçsin.
- [ ] **Adım 6:** `ctest -R 'test_orch_memory|test_orch_rpc|test_orch_remote_rpc|test_cli' --output-on-failure` → geçer. Kurulu olmayan daemon'a karşı `build/atlas job get <uid> --json` çalıştırmaya gerek yok; `job get` yeni komut değil.

### T4: `logs/stdout.log` yerel erişimi ve araç kullanım sayımının veri kaynağı (yalnızca doğrulama)

**Dosyalar:** yok (bulgu, T6'nın girdisi).

- [ ] **Adım 1:** `src/orch/dispatch.c` `build_complete` içinde `res->log`'un tamamlanmaya artefakt olarak eklenip eklenmediğini oku; `job.artifact` yerel RPC'sinin (`server_orch.c:2551`) `logs/stdout.log` adını sunup sunmadığını kaydet. İki cümlelik bulguyu T6 betiğinin başlık yorumuna yaz: "başarılı bir işin stream logu Atlas'ta [saklanır/saklanmaz]".

### T5: Belgeler

**Dosyalar:** `docs/remote-submission.md` (yeni §8 — worker metni **yerine**), `docs/orchestration.md` (A10.1 bölümüne `memory_requested` ve `context_pack_*`), `docs/extending.md` (`atlas_orch_memory_select` sözlüğü için giriş: üçüncü değer eklemenin bedeli), `CLAUDE.md` A14 satırı, `docs/backlog.md` (üç giriş).

- [ ] **Adım 1:** §8 içeriği: (a) neden istek alanı — Decision 4'ün listesi sürücü, mod, gate tabanı, denemeler, bütçeler ve `dispatch.` adlarıdır; memory bunların hiçbiri değildir, worker'a **ne gösterildiğini** değiştirir, **neye mal olabileceğini** değil (duvar, deneme, gate değişmez); (b) sözlük ve UNSET'in anlamı; (c) yerel açık `--memory off`'un artık pack'i de kapattığı (davranış değişikliği, nedeni A10.1'in "OFF hiçbir şey eklemez" kuralı); (d) Decision 7R2: uzak okuma yüzeyi memory özetini (bayt/özet) taşır; (e) "A10.1 korpusu M23'te sabitlendi" gerçeği ve sonucu: ON kolunun A10.1 paketi bu kurulumda büyük olasılıkla EMPTY'dir, rapor bunu söyler.
- [ ] **Adım 2:** `docs/backlog.md` girişleri: (1) stream-json çıktısı operatör HOME'u altında ≈ 2 KB/s büyür; 16 MiB tavanı ≈ 2 s 15 dk; 3 saatlik duvar tavanı hâlâ aşabilir — driver'ın son N baytı tutması bir tasarım sorusudur, çözülmedi; (2) Atlas worker'ın araç kullanımını kaydetmez; kanıt Atlas dışındadır; (3) yerel operatörün saklanmış uzak iş sonucunu okuyacak CLI yok (T7 isteğe bağlı).
- [ ] **Adım 3:** `CLAUDE.md` A14 bölümüne bir satır: "`memory` uzak istekte `off|on`; UNSET bugünkü davranış; açık OFF iki paketi de bastırır; talep edilen değer `orch_run_memory.requested` (migration 33)." "Bir migration" ifadesi "iki migration (32, 33)" olur.
- [ ] **Adım 4:** `grep -n 'holds memory at OFF\|no memory, one slot' src docs CLAUDE.md` → sıfır eşleşme.

### T6: Operatör betikleri (ürün kodu değil)

**Dosyalar:**
- Oluştur: `scripts/memexp-report.sh` — RUN kimlikleri listesi alır, her biri için `atlas job run-status RUN --json` çağırır ve §E.4 tablosunu basar; eksik alan → `unknown`.
- Oluştur: `scripts/memexp-toolcensus.sh` — bir işin çalışma-alanı anahtarından `~/.claude/projects/-home-nocdem--local-state-atlas-model-jobs-<JOB>-1-work/*.jsonl` dökümünü (varsa `logs/stdout.log`'u da) okur; `"name":"mcp__plugin_atlas_memory__<araç>"` sayımını araç adına göre basar; §E.3'teki memory araç kümesinden biri OFF kolunda görülürse `CONTAMINATED` yazar.
- Değiştir: `/opt/atlas/deploy.local.sh` (gitignore'lu, operatör dosyası) — §F'deki genişletme.

Betikler bash; JSON anahtarları `grep -oE '"anahtar":[^,}]*'` ile okunur (Atlas'ın anahtarları
sabit ve düz). Python yok.

- [ ] **Adım 1:** `memexp-report.sh`: girdi `RUN...`; çıktı satırı: `run state memory_requested memory_mode package_status package_bytes package_digest[0:8] ctx_present ctx_bytes ctx_digest[0:8] usage_status in out cache_w cache_r cost_micro_usd worker_ms turns`. Anahtar yoksa `unknown`. `usage_status != AVAILABLE` ise token/süre kolonları `unknown` (PARTIAL'da mevcut olanlar yazılır, eksikler `unknown`).
- [ ] **Adım 2:** `memexp-toolcensus.sh JOB [--arm off|on]`: döküm yoksa `no transcript` yazıp 2 ile çıkar; sayımı basar; `--arm off` ve memory kümesinden sayım > 0 → son satır `CONTAMINATED`.
- [ ] **Adım 3:** İki betiği `sh -n` ile söz dizimi kontrolünden geçir; `memexp-report.sh` için var olan bir RUN'la (daemon'a karşı, salt okunur) bir satır üret; `memexp-toolcensus.sh`'ı `jffa5ae580b1efc85312fe891b46ebeb7` ile çalıştır → `mcp__` sayımı 0, Bash 88, Read 45, Edit 33 (bu oturumda ölçülen değerler).
- [ ] **Adım 4:** `deploy.local.sh` §F.1'deki gibi genişlet; `bash -n deploy.local.sh`. **Çalıştırma.**

### T7 (isteğe bağlı, deneyi engellemez): yerel `atlas job result JOB [--out DIR]`

Yerel operatörün, gönderen uid'nin kendi işine ait saklanmış üç adlı sonucu (`result.txt`,
`changes.patch`, `validations.txt`) okuması. Mevcut `job.artifact` yerel RPC'si üzerinden
(yeni yöntem yok); beş yere dokunur (CLAUDE.md "Adding a command"): `service_orch.c`,
`render.h`, iki renderer, `cli.c` + `COMMANDS[]`. Bu görev yalnızca operatör tarayıcı
Jobs görünümünü yeterli bulmazsa yapılır; yapılırsa `build/atlas job result <uid>` bir kez
gerçek binary'den çalıştırılır (`is_a_command` tuzağı).

---

## E. Doğrulamalar ve gerçek OFF/ON deney düzeni

### E.0 Faz 0 — sıfır maliyetli sayım (kod kurulduktan sonra, ücretli koşudan önce)

Amaç: ON kolunun gerçekten bir şey alıp almadığını ölçmek. `fake` sürücüsü ücretsizdir.

**A10.1 yarısı bugün, kod değişmeden ölçülebilir:** kurulu binary `--memory bounded`'ı zaten
alır; `atlas job submit --repo atlas --driver fake --memory bounded --task "<metin>"` ve
`atlas job run-status RUN` → `memory_package_status`. Context Pack yarısının cevabı zaten
bilinir: canlı `system.conf`'ta `memory_source` yok → `false`. Bedeli: her `fake` koşusu canlı
veritabanına kalıcı bir ACTIVE run yazar (çalışma alanı köklü koşular hiç settle olmaz);
bu tur çalıştırılmadı, operatör §G.1'i veriyle kapatmak isterse çalıştırır.

```sh
atlas job submit --repo atlas --driver fake --memory on  --task "<deney görev metni>"   # RUN_ON yazdırır
atlas job submit --repo atlas --driver fake --memory off --task "<deney görev metni>"   # RUN_OFF
atlas job submit --repo atlas --driver fake              --task "<deney görev metni>"   # RUN_UNSET
sh scripts/memexp-report.sh RUN_ON RUN_OFF RUN_UNSET
```

Okunacaklar: RUN_ON → `memory_mode BOUNDED`, `package_status` (PRESENT ise
`memory_sources` sayısı; EMPTY ise §G.1'e bakın), `context_pack_present`
(`memory_source` kayıtlı ve nesil > 0 değilse false). RUN_OFF → OFF, EMPTY, false.
RUN_UNSET → OFF ve pack bugünkü kural. **Kapı:** RUN_ON'da iki paketten en az biri gerçek
bayt taşımıyorsa OFF/ON kolları aynı baytları alır ve karşılaştırma boştur; ücretli koşu
başlatılmaz, §G.1 cevaplanır.

### E.1 Faz 1 — ön koşullar (operatör, makinede)

1. HEAD == çalışma ağacı (`git status --short` boş). Nedeni A.3'te: snapshot HEAD'dir, indeks
   ağaçtır. Nasıl sağlanacağı §G.2.
2. `/etc/atlas/orchestration.conf`: `max_output_bytes = 16777216` (mutlak tavan; A.4'teki
   ölüm), `executor_model = <seçilen model>` (aynı model **belirtilmiş** olsun). Değeri
   lease taşır (`src/orch/dispatch.c:1125`), yine de her iki dispatcher da yeniden başlatılır.
3. `/etc/atlas/system.conf`: `memory_source = ...` (§G.1). Daemon restart; watcher tiki
   memory geçişini üretir; Faz 0'ı **tekrar** koş ve `context_pack_present = true` gör.
4. `/opt/atlas` deney boyunca dondurulur: düzenleme yok, commit yok, scanner çalışmaya
   devam eder. Her gönderimden hemen önce `atlas_repo_overview` (veya `atlas status --json`)
   kaydı: `scanned_head`, `dirty`, `index_current`, `code_generation`, semantik nesil.
   Kollar arasında bu değerler **eşit** olmalı (`generation` her reconcile'da artar, eşitlik
   şartı değildir).
5. Terminal olmayan başka iş yok (`atlas job list --json`; §F.1 ön kontrolüyle aynı sözlük).
6. T1 Adım 12'nin argv toplamı bulgusu okunmuş; 131072'yi aşıyorsa ON kolu için görev metni
   o kadar kısaltılır ve rapor bunu söyler.

### E.2 Faz 2 — eşleştirilmiş koşular

- Çift sayısı §G.3. Sıra dengeli: çift 1 `OFF → ON`, çift 2 `ON → OFF`, çift 3 `OFF → ON`.
  Kollar **ardışık** koşar (aynı anda değil): iki worker aynı makinede CPU/IO paylaşırsa süre
  ölçümü bozulur; A10.1 paketi zaten gönderimde donduğu için sıra memory'yi etkilemez.
- Her kol: aynı görev baytları (dosyadan; `sha256sum` raporda), ayrı idempotency anahtarı
  (`memexp.p1.off`, `memexp.p1.on`, …), aynı `commit` (`job get` → `commit` eşit), aynı
  `executor_model`, aynı politika, aynı HOME (aynı eklenti kümesi — iki kolda da sabit).
- Gönderim yolu: steward'ın uzak MCP'sinden `atlas_job_submit(repo="atlas", task=<metin>,
  key="memexp.p1.off", memory="off")`; ya da operatörün yerelinden `atlas job submit --repo
  atlas --task "$(cat task.txt)" --memory off --idempotency-key memexp.p1.off` (bayrak adı
  `cli.c`'de doğrulanır). İki yol aynı yazma noktasına iner; bir deney içinde tek yol kullanılır.
- Her gönderimden sonra yanıtın `run` kimliği kaydedilir (yerel: `job get` artık `run` verir).
- Kol bittiğinde: `memexp-report.sh RUN` satırı; `memexp-toolcensus.sh JOB --arm off|on`.
- ON kolları arasında `package_digest` ve `context_pack_digest` **eşit** olmalı (sabit
  memory kanıtı). Eşit değilse çift `MEMORY_DRIFTED` olarak işaretlenir, atılmaz.
- OFF kolunda memory aracı görülürse çift `CONTAMINATED` olarak raporlanır; zorlama yok.
- Başarısız bir ücretli koldan sonra tekrar **sorulmadan** koşulmaz (operatörün kayıtlı
  kuralı); harcanan usage satırdan raporlanır.

### E.3 Araç kümeleri (sayım için)

- Memory kümesi (OFF kolunda görülmesi kirlenme): `atlas_memory_search`,
  `atlas_context_build`, `atlas_decisions`, `atlas_decision`, `atlas_decision_history`,
  `atlas_file_context`, `atlas_session_state`, `atlas_verify_*`, `atlas_record_*`,
  `atlas_propose_decision`, `atlas_revise_decision`.
- İndeks kümesi (her iki kolda serbest; gereksinim 3): `atlas_repo_overview`,
  `atlas_status`, `atlas_search`, `atlas_changed_files`, `atlas_code_*`, `atlas_sem_*`,
  `atlas_gate_check`.
- Zorunluluk **yoktur**: rapor "gözlendi: N çağrı / gözlenmedi" der, "kullandı" iddiası
  sayımdan büyük olamaz.

### E.4 Faz 3 — rapor

Kol başına satır; sütunlar: `run`, `state`, `requested`, `applied(mode, package bayt/özet,
pack bayt/özet)`, `input_tokens`, `output_tokens`, `cache_creation_tokens`,
`cache_read_tokens`, `cost_usd`, `worker_ms` (`usage_worker_duration_ms`, CLI'nin kendi
ölçümü), `wall_ms` (`terminal_at − created_at`, kuyruk dâhil), `queue_ms`
(`wall_ms − worker_ms`, yaklaşık), `turns`, `correctness`, `tool_census`, `flags`.

Kurallar:
- `usage_status != AVAILABLE` → ilgili hücreler `unknown`; hiçbir yerde 0 yazılmaz.
- Toplam **tek sayıya indirgenmez**: bağlam girdisi = `input + cache_creation + cache_read`
  (fiyatları farklı), çıktı ayrı. Önbellek alanları `input_tokens` içinde **değildir**
  (stream'de `input_tokens` önbellek dışı kalanı verir); rapor bunu başlıkta söyler.
- Maliyet yalnızca sağlayıcının `total_cost_usd`'ünden (`cost_micro_usd`); tahmin yok.
- Memory hazırlığı/indeksleme maliyeti ayrı satır: Faz 1'de memory geçişinin süresi
  (daemon günlüğünden) ve pack baytları; kol maliyetine eklenmez.
- Doğruluk: her kolun `changes.patch`'i pinlenen commit'teki temiz bir klona `git apply`;
  `make` + görev için önceden yazılmış tek doğrulama komutu (görevle birlikte sabitlenir);
  sonuç `PASS`/`FAIL`/`NO_PATCH`. Aynı komut iki kola da.
- Birim testleri performans kanıtı olarak **yazılmaz**; rapordaki her sayı bir `orch_usage`
  satırından veya yukarıdaki üç komuttan gelir.

### E.5 Bu planın kod doğrulaması (deneyden bağımsız)

- `make && cd build && ctest -L unit` → tamamı.
- `ctest -R 'test_orch_memory|test_migrate|test_mcp|test_gateway'` → tamamı.
- `ctest -R 'test_memory_pack_live|test_orch_remote_rpc|test_gw_submit|test_orch_rpc|test_gw_remote'` → tamamı (daemon etiketi, seri).
- `make smoke`.
- Sonra tam `make test`; 15:58'deki 125 + yeni `test_migrate33` = 126 geçer.

---

## F. İlk kurulum/restart süreci ve sonrasında bu oturumdan yönetim

### F.1 İlk kurulum (operatör, makinede; her adım `sudo` ister ya da root-owned dosya düzenler)

1. Diff'i oku; `make && make test`. (Bu plan commit etmez; kurulum çalışma ağacından yapılır.)
2. Politika düzenlemeleri (root): `orchestration.conf` → `max_output_bytes = 16777216`,
   `executor_model = …`; `system.conf` → `memory_source = …` (§G.1). Yedek kopya adları mevcut
   `.pre-*` geleneğiyle.
3. `bash /opt/atlas/deploy.local.sh` — T6'da genişletilmiş hâli:
   - ön kontrol: `atlas job list --json` içinde terminal olmayan hiçbir durum yoksa devam
     (sözlük `atlas_orch_state_is_terminal`'dan alınır: QUEUED ve LEASED de dâhil, yalnız
     RUNNING değil); varsa dur — dispatcher restart'ı kiralanmış ya da koşan worker'ı
     öldürür, deneme FAILED olur;
   - kurulum + chown + içerik doğrulaması (mevcut);
   - `sudo systemctl restart atlas.service atlas-gateway.service atlas-dispatcher.service atlas-scanner.service`;
     `systemctl --user restart atlas-model-dispatcher.service` (sudo'suz);
   - beş birim için `is-active` ve `readlink /proc/<MainPID>/exe` sonunun `(deleted)`
     olmadığı; `atlas daemon ping`; `atlas gateway status` (politika okunuyor).
4. `atlas scanner run --once`; `atlas status --json` → `index_current: true`.
5. Faz 0 sayımı (§E.0). `context_pack_present = true` görülene kadar ücretli koşu yok.

### F.2 Sonrasında bu oturumdan (uzak MCP, mevcut kimlik bilgisiyle)

Yapılabilir: `atlas_job_submit(repo, task, key, memory=off|on)`, `atlas_job_status`,
`atlas_job_result` (`result.txt`, `changes.patch`, `validations.txt`, usage), `atlas_job_list`,
`atlas_job_cancel`. Güncelleme akışı: steward görevi gönderir → worker `patch` modunda çalışır
→ steward `changes.patch`'i okur ve operatöre "hazır" der.

Yapılamaz, tasarım gereği: yamayı `/opt/atlas`'a uygulamak, `make install`, herhangi bir birimi
yeniden başlatmak, politika düzenlemek. Bunlar F.1'in 1–4 adımlarıdır; her güncellemede
operatör makinede `git apply --3way <patch>` (veya `patch -p1`), `make && make test`,
`bash deploy.local.sh` çalıştırır. Yamayı operatörün eline ulaştıran mevcut yollar: tarayıcı
Jobs görünümü (LAN, `http://192.168.0.198:8799`) ve steward'ın sohbete yapıştırması; T7 yapılırsa
`atlas job result`.

Söylenmesi gereken gerçek: worker operatör hesabında, sandbox'sız ve `NOPASSWD: ALL` ile
koşar; bir worker'ın `sudo` çalıştırmasını bugün engelleyen tek şey görev metni ve model
davranışıdır. Bu plan bunu bir kanal hâline **getirmez** (A14R: `job.remote_apply` yasak;
yeni yetki fiili yok). Hesap seçimi §G.4.

---

## G. Yalnızca operatör kararı gerektiren açık noktalar

1. **Hangi `memory_source` kaydedilecek?** Seçenekler `REPO_FILE:CLAUDE.md` (commit ile
   donar; worker snapshot'ta zaten görür — pack'in katkısı iddialar ve doğrulama hükümleri
   olur) ya da `EXTERNAL_DIR@atlas:/home/nocdem/.claude/projects/-opt-atlas/memory`
   (depoda olmayan geri bildirim; scanner taşır; deney boyunca dizin düzenlenmemeli). Kayıt
   yoksa ON kolu pack almaz ve A10.1 korpusu M23'te sabitlenmiş olduğundan ON büyük olasılıkla
   OFF ile aynı baytları alır; Faz 0 bunu ölçer.
2. **HEAD ≠ çalışma ağacı.** Worker HEAD'i düzenler, indeks ağacı anlatır. Deney için ikisi
   eşit olmalı. Eşitliğin nasıl sağlanacağı (A14R + bu planın işini commit etmek, ya da
   deneyi zaten temiz bir ağaçta koşmak) operatörün kararı; plan yalnızca ön koşulu ve
   nedenini yazar.
3. **Çift sayısı.** Kol başına ≈ 5 USD (kayıtlı gözlem); 2 çift ≈ 20 USD, 3 çift ≈ 30 USD.
   Faz 0 ücretsizdir. Görev metni de burada seçilir; aday: `docs/backlog.md`'deki "`--json`
   liste komutu daemon yokken hata belgesini sonuç belgesinin içine yazar" ailesi
   (`job list`, `plan list`) — sınırlı, gerçek, tek komutla doğrulanabilir
   (`build/atlas job list --json` daemon yokken tek geçerli JSON belgesi).
4. **Model dispatcher hesabı.** Bugün worker `nocdem` olarak, sandbox'sız, parolasız root ile
   koşar (A8.1 istisnasının bedeli "OS izolasyonu" değil, root eşdeğerliğidir). Böyle kalıp
   kalmayacağı bu planın dışında ama raporun içindedir.

Varsayım (karar değil, açıkça yazıldı): gereksinim 1 gönderim başına seçimi istediği için
`memory` FORBIDDEN'dan çıkar; Decision 4'ün listesi memory'yi zaten saymıyor ve seçim hiçbir
sınırı (duvar, deneme, gate, bütçe) oynatmıyor.

---

## Öz denetim

- Kapsam: A.1 → T1+T2; A.2 → T1 (bastırma, migration) + T3 (rapor); A.3 → E.1–E.2 + G.2;
  A.4 → E.4 + T6 + F.1 adım 2; A.5 → T4 + T6 + E.3; A.6 → F + T6 + T7.
- Yer tutucu taraması: "TBD/TODO/uygun hata işleme/benzer şekilde" yok; her test adı ve
  anahtar yazılı.
- Tip tutarlılığı: `atlas_orch_memory_select` (T1) T2/T3'te aynı adla; `memory_requested`,
  `context_pack_present/generation/digest/bytes`, `run` anahtarları T3 ve T6 betiklerinde aynı.
