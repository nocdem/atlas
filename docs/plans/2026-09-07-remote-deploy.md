# Uzaktan Onaylı Güncelleme ve Restart (A17) — Uygulama Planı

> **Yürütücü için:** `superpowers:subagent-driven-development` (önerilen) ya da
> `superpowers:executing-plans` ile görev görev uygulayın. **Commit adımı yoktur**; operatör
> commit/push'u yasakladı. Bu tur canlı kurulum ve restart yoktur. Memory/benchmark planı
> (`2026-09-07-memory-offon-benchmark-and-remote-update.md`) bekler ve **değiştirilmez**;
> bu plan migration **33**'ü alır, o plan koştuğunda kendi migration'ı 34 olur.

**Amaç:** Steward'ın (bu ChatGPT oturumu) Atlas'a yazdırdığı bir değişikliği, operatörün
onayıyla `/opt/atlas`'a uygulatmak, Atlas'ı derleyip kurmak, beş birimi yeniden başlatmak ve
bağlantı geri geldiğinde sonucu aynı oturumdan okumak. Önceki raporun "uygulama ve restart
buradan yapılamaz" sınırı bu planla kalkar; kalkma biçimi aşağıda tek tek gerekçelendirilir.

**Mimari (üç ilke):**

1. **Atlas binary'si hiçbir servisi başlatmaz ve depoyu değiştirmez.** İkisini de operatörün
   bir kez kurduğu, root'a ait bir ajan yapar: `atlas-deploy.path` (systemd yol birimi)
   `/var/lib/atlas/deploy/requests/` dizinini izler; bir istek dosyası düşünce
   `atlas-deploy.service` (root, oneshot) `/usr/local/libexec/atlas/atlas-deploy-agent`'ı
   çalıştırır. Daemon'un katkısı, onay işlemi commit olduktan **sonra** yazılan bir istek
   dosyası ve sonucu geri okumaktır. Testler yol birimini asla kurmaz; kuyruğu, `.res`
   dosyasını kendisi yazan sahte bir tüketiciyle sürer.
2. **Uygulanan bayt Atlas'ın kendi sakladığı `changes.patch`'tir**, teklif anında özeti
   sabitlenir, istek alanı değildir. Ağaç, sahip, birimler ve test komutu daemon'un ve
   gateway'in erişemediği root'a ait `/etc/atlas/deploy.conf`'ta yaşar.
3. **Onaylayan kimlik bilgisi teklif edenden farklıdır** ve onay bir MCP aracı değildir:
   A16'nın tarayıcı kalıbı birebir — `remote_deploy_key`, `deploy.remote_challenge` →
   `deploy.remote_confirm`, Mission Control'de yamanın sha256'sının ilk sekiz hex'inin
   yazılması, defterde `REMOTE_OPERATOR_CONFIRMED` + key id. Oturumdan teklif edilir,
   izlenir, okunur; her güncellemede tek insan adımı, deploy kimlik bilgisini taşıyan
   herhangi bir cihazdaki tarayıcıdan onaydır.

**Kesinleşen operatör kararı (2026-09-07):** Onay yöntemi 3. ilkede yazıldığı gibidir —
her güncellemede, teklif eden anahtardan **ayrı** bir deploy kimlik bilgisiyle, tarayıcıdan,
yamanın sha256'sının ilk sekiz hex'i yazılarak. Operatör bunu kabul etti. **Executor bu
tercihi yeniden açmaz, alternatif önermez, sohbetten ya da MCP aracından onay yolu
eklemez.** Bu karar yalnızca onay yöntemini kesinleştirir: §G'deki diğer açık noktalar
açık kalır ve canlı kurulum/restart için verilmiş bir onay **yoktur**.

**Teknoloji:** C17, SQLite, `atlas_test.h`, bash (ajan; Python yok), systemd path/oneshot.

**Spec:** Operatörün bu turdaki görev tanımı; `docs/remote-submission.md` (A14, A14R
Decision 7R), `docs/browser-disposal.md` (A16), `docs/remote-access.md`, `CLAUDE.md`'nin
"Hard rules" ve A14R/A16 kuralları.

---

## A. Bu neyi tersine çevirir, ve neden daha dardır

A14R `job.remote_apply`, `job.remote_artifact`, `job.remote_log`, `job.remote_run` adlarını
yasakladı ve `tests/test_orch_rpc.c:107` bunları tarar. Gerekçe: bir çağıran, bir worker'ın
çıktısını uygulatabilirdi. Bu yetenek bir worker'ın çıktısını uygular. Bu bir **tersine
çevirmedir** ve `docs/remote-deploy.md` bunu A9.2.4'ün kendi tersine çevirmelerini yazdığı
biçimde yazar: ne reddedildi, neden, ve neyin bunu reddedilenden daha dar yaptığı:

- Uygulanan baytlar isteğin bir alanı değil, Atlas'ın `orch_artifacts`'ta sakladığı
  `changes.patch`'tir; özeti teklif işleminde satıra yazılır ve onay bu özete bağlıdır.
- Hangi ağaç, hangi sahip, hangi birimler, hangi test: root'a ait ajan conf'u; istek
  dosyası yalnızca bilgi taşır, ajan conf ile uyuşmayanı reddeder.
- Teklif eden kimlik (`remote_submit_key`) ile onaylayan kimlik (`remote_deploy_key`) farklı
  olmak zorundadır; politika aynı id'yi iki role yazamaz.
- Root olarak koşan süreç Atlas değildir; operatörün `deploy.local.sh` sınıfındaki bir aracıdır,
  şablon olarak gelir, root bir kez kurar.
- "Hiçbir yöntem uygulamaz" cümlesi testle ayakta kalır: tarayıcıya `deploy.remote_apply`,
  `deploy.remote_install`, `deploy.remote_restart`, `deploy.remote_run` eklenir.

**Zayıflık, açıkça:** A16'da ele geçirilmiş kimlik bilgisi bir kaydı disposal ederdi. Ele
geçirilmiş bir deploy kimlik bilgisi bu makinede **root olarak çalışan kod kurar**; canlı
kurulum `tls_mode = NONE` ile LAN'da düz metindir. Bu yüzden ayrı bir
`operator_accepts_cleartext_deploy = yes` satırı gerekir; var olan iki kabul satırından
hiçbiri bunu ima etmez; cümle onay ekranında görünür.

**Yapısal sorun, adıyla:** Worker snapshot'ı HEAD'dir, `changes.patch` HEAD'e karşıdır; ajan
yamayı çalışma ağacına uygular ve **commit etmez** (kesin kural). Bir deploy sonrası ağaç yamayı
commit edilmemiş taşır; ikinci iş HEAD'i yamasız snapshot'lar; ikinci yama birinciyle
çakışabilir ya da onu yineleyebilir. Bugünkü ağaç zaten 28 dosyada commit edilmemiş A14R taşır;
bunlardan birine dokunan ilk gerçek deploy `apply --check`'te reddedilir. Deploy sonrası kimin
commit edeceği §G.1'dir.

---

## B. Kaynak temeli

Canlı = HEAD `2922214` + commit edilmemiş A14R (önceki planın §B'si; sağlama yeniden
yapılmadı, aynı gün, aynı ağaç). Executor `/opt/atlas` ağacında, A14R'nin üstünde çalışır;
ilk adım yedek yama. Hiçbir toplu commit/reset/üzerine yazma yok. Memory planındaki hiçbir
parça (`job get → run`, `memory` alanı, `orch_run_memory.requested`) buraya sızmaz.

Aynalanacak mevcut kod (executor önce bunları okur):

- Politika anahtarı ve reddi: `include/atlas/gwpolicy.h:222-266` (`remote_dispose_key`,
  `cleartext_disposal_accepted`), `src/gw/gwpolicy.c`'deki `remote_dispose_key` dalı.
- Daemon'un kimlik türetimi: `src/ipc/server_remote.c:112-123, 218` (dispose anahtarı
  boşsa/eşleşmiyorsa); scope tablosu `src/gw/apikey.c:36,41` (`decisions:dispose`,
  `jobs:submit`, ikisi de `grantable = false`).
- İki adımlı onay: `decision.remote_challenge` → `decision.remote_dispose`
  (`src/ipc/server_remote.c:467-534`); challenge'ın bağlandığı alanlar ve TTL **oradan
  alınır**, yeni gramer icat edilmez.
- Rota satırları: `src/gw/gateway.c:1063-1067` (dispose) ve `:1072-1083` (`job.remote_*`).
- Sayfa: `src/gw/ui/mission-control.html` — `dispose` diyaloğunun zayıflık cümlesi ve yazılan
  önek alanı; şekli kopyalanır.
- Kuyruk/ingest için tek yazar kuralı: `src/daemon/writer.c` iş türleri
  (`job_kind_is_unbounded`, `job_kind_is_drainable`, ikisi de `default:`'suz).
- A8 recovery sweep'in daemon döngüsünden sürülmesi: `src/daemon/daemon.c:210`.

---

## C. Küresel kısıtlar

- Migration **33**: `deploys`, `deploy_transitions`, iki kısmi tekil indeks.
  `ATLAS_SCHEMA_VERSION 32 → 33` (`include/atlas/db.h:24`); 32'yi sabitleyen yedi test
  dosyası (`tests/test_migrate7.c:288`, `test_migrate8.c:294`, `test_migrate9.c:62`,
  `test_migrate29.c:263`, `test_migrate31.c:264`, `test_migrate32.c:218`,
  `test_migrate_scanner_uid.c:45`) 33 olur.
- Atlas binary'sinde `systemctl`, `sudo`, `runuser`, `make` çağrısı **yoktur**. Yeni süreç
  oluşturma yolu yoktur (`atlas_proc_run` dışında). Yeni iş parçacığı, zamanlayıcı, arka plan
  döngüsü yoktur; ingest, mevcut watcher tikinde saf bir türetimle (`results/` boş değil)
  kuyruğa alınan sınırlı bir yazar işidir ve daemon başlangıcında bir kez çalışır.
- Root ajanı SQLite dosyasını **açmaz**; yalnızca kuyruk dosyaları. Sonuç metni sınırlıdır
  (≤ 32 KiB) ve veri olarak işlenir, asla yorumlanmaz.
- `"memory"`, `"driver"`, `"mode"`, `"validation"`, `"parallel"`, `"parent"` uzak istekte
  reddedilmeye devam eder. Bu plan `job.*` yöntem yüzeyine dokunmaz; iki gönderim yazma
  noktasına yalnızca "deploy CONFIRMED iken kök gönderim reddi" eklenir (D.1).
- Yeni MCP araç adında yetki fiili yoktur (A15 tarayıcısı: `approve, approval, reject,
  supersede, confirm, sign, resolve, revalidate`). Onay MCP aracı **değildir**.
- `atlas_decision_apply_in_tx` üç çağırıcıda kalır; bu plan karar yaşam döngüsüne dokunmaz.
- Depo adı/yolu ürün mantığına girmez; `/opt/atlas` yalnızca ajan conf'unda ve şablon
  yorumlarında geçer.
- `ATLAS_WERROR=ON`; Python/Node yok; ağ yok.
- Hiçbir görev canlı politika düzenlemez, birim kurmaz, servisi yeniden başlatmaz.

---

## D. Tasarım

### D.1 Durumlar ve kayıt

```
PROPOSED ──confirm──▶ CONFIRMED ──result──▶ SUCCEEDED | FAILED
    └──cancel──▶ CANCELLED
```

- `PROPOSED`: teklif eden submit kimliği; iş SUCCEEDED, `patch` modunda, `changes.patch`
  satır içi saklanmış (`content_stored = 1`) ve boş değil; özet ve bayt sayısı satıra yazılır.
  İstek yalnızca `job` taşır; `dry_run` bir istek alanı **değildir** (yalnızca ajan conf'unda
  yaşar ve sonuçta raporlanır).
- `CONFIRMED`: deploy kimliği + doğru önek + challenge; işlem içinde reddedilenler: herhangi
  bir işin terminal olmaması (`atlas_orch_state_is_terminal` sözlüğü; restart worker'ı
  öldürür), başka aktif deploy, özet uyuşmazlığı. Commit'ten sonra kuyruk dosyası yazılır ve
  `spooled_at` işaretlenir; yazılamazsa satır CONFIRMED kalır ve daemon başlangıç süpürmesi
  yeniden dener. **Aynanın öbür yüzü:** onay ile restart arasında dakikalar vardır; bu sürede
  kabul edilen bir kök gönderim (`job.submit` ya da `job.remote_submit`) restart'ta ölür. Bu
  yüzden her iki gönderim yazma noktası, bir deploy CONFIRMED iken kök gönderimi deploy uid'ini
  cümlede adlandırarak reddeder (tek sorgu, A11.0'ın "her kontrol gönderim işleminin içinde"
  biçimi). Yöntem yüzeyi değişmez; yalnızca yazma noktasına bir ret eklenir.
- `SUCCEEDED`/`FAILED`: yalnızca ajanın `.res` dosyasından, aktör `DEPLOY_AGENT`, aşama ve
  sınırlı metin ile. Daemon gözlemediği bir duruma **geçmez**: sonucu gelmeyen CONFIRMED,
  okumada yaşıyla raporlanır ("confirmed N dakika önce, sonuç yok"), asla zaman aşımına
  düşürülmez.
- `CANCELLED`: yalnızca PROPOSED'dan, yalnızca teklif eden kimlikle.

Migration 33:

```c
static const char M33_DEPLOYS[] =
    "CREATE TABLE deploys ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  deploy_uid TEXT NOT NULL UNIQUE,"          /* 'd' + 32 lowercase hex */
    "  job_uid TEXT NOT NULL,"
    "  repo_id INTEGER NOT NULL,"
    "  base_commit TEXT NOT NULL,"
    "  patch_digest TEXT NOT NULL,"               /* sha256 hex of the stored changes.patch */
    "  patch_bytes INTEGER NOT NULL,"
    "  state TEXT NOT NULL"
    "    CHECK(state IN ('PROPOSED','CONFIRMED','SUCCEEDED','FAILED','CANCELLED')),"
    "  proposed_key_id TEXT NOT NULL,"
    "  confirmed_key_id TEXT NOT NULL DEFAULT '',"
    "  created_at TEXT NOT NULL, created_ms INTEGER NOT NULL,"
    "  confirmed_at TEXT, spooled_at TEXT, terminal_at TEXT,"
    "  result_dry_run INTEGER NOT NULL DEFAULT 0 CHECK(result_dry_run IN (0,1)),"  /* from the .res, never from the request */
    "  result_stage TEXT NOT NULL DEFAULT '',"
    "  result_rollback TEXT NOT NULL DEFAULT '',"
    "  result_head_before TEXT NOT NULL DEFAULT '',"
    "  result_head_after TEXT NOT NULL DEFAULT '',"
    "  result_version TEXT NOT NULL DEFAULT '',"
    "  result_text TEXT NOT NULL DEFAULT ''"
    ");"
    /* One deploy in flight per repository: the schema is the guarantee, the C check names it. */
    "CREATE UNIQUE INDEX idx_deploys_one_active ON deploys(repo_id)"
    "  WHERE state IN ('PROPOSED','CONFIRMED');"
    /* A job's patch is deployed at most once successfully; a FAILED or CANCELLED one may be re-proposed. */
    "CREATE UNIQUE INDEX idx_deploys_one_per_job ON deploys(job_uid)"
    "  WHERE state IN ('PROPOSED','CONFIRMED','SUCCEEDED');";

static const char M33_DEPLOY_TRANSITIONS[] =
    "CREATE TABLE deploy_transitions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"      /* ordering is this id, never a timestamp */
    "  deploy_id INTEGER NOT NULL REFERENCES deploys(id),"
    "  from_state TEXT NOT NULL, to_state TEXT NOT NULL,"
    "  actor TEXT NOT NULL"
    "    CHECK(actor IN ('REMOTE_CREDENTIAL','REMOTE_OPERATOR_CONFIRMED','DEPLOY_AGENT')),"
    "  key_id TEXT NOT NULL DEFAULT '',"
    "  reason TEXT NOT NULL DEFAULT '',"
    "  at TEXT NOT NULL"
    ");";
static const char *const M33_STATEMENTS[] = {M33_DEPLOYS, M33_DEPLOY_TRANSITIONS, NULL};
/* table entry */
{33, "a deploy a credential proposed, a different credential confirmed, and a root agent reported",
 M33_STATEMENTS, false},
```

Her geçiş compare-and-swap'tır (`WHERE state = <gözlenen>`, tam bir satır), A8 kuralı.

### D.2 Yöntemler, rotalar, araçlar

Daemon (`src/ipc/server_deploy_remote.c`, yeni; `server_orch_remote.c`'nin kimlik doğrulama
yolunu çağırır, kopyalamaz):

| Yöntem | Kimlik | Parametreler | Yazar mı |
| --- | --- | --- | --- |
| `deploy.remote_propose` | `remote_submit_key` (türetilmiş `jobs:submit`) | `job` | evet |
| `deploy.remote_get` | submit **veya** deploy kimliği | `deploy` | hayır |
| `deploy.remote_list` | aynı | `cursor?` | hayır |
| `deploy.remote_cancel` | teklif eden submit kimliği | `deploy` | evet |
| `deploy.remote_challenge` | `remote_deploy_key` (türetilmiş `deploys:confirm`) | `deploy` | evet (challenge satırı, A16 gibi) |
| `deploy.remote_confirm` | `remote_deploy_key` | `deploy`, `challenge`, `confirmation` | evet |

`confirmation` = yamanın sha256'sının ilk sekiz hex'i; A16'nın "revizyon özetinin ilk sekiz
hex'i" kuralının aynısı. Challenge deploy uid + patch_digest'e bağlanır, TTL A16'nınki.
Bu iki yöntem ve tarayıcı diyaloğu operatörün kesinleşmiş kararıdır (başlıktaki not);
executor T2/T3'te bunları olduğu gibi uygular.

Gateway rotaları (`gateway.c:1063-1083` satır şekli): `/api/v1/deploy/propose`,
`/api/v1/deploy/get`, `/api/v1/deploy/list`, `/api/v1/deploy/cancel`,
`/api/v1/deploy/challenge`, `/api/v1/deploy/confirm`. Rota tablosu dışında hiçbir alan soket
mesajına girmez. Scope `ATLAS_SCOPE_DEPLOYS_CONFIRM` `"deploys:confirm"`, `grantable = false`,
daemon yalnızca politikanın adlandırdığı anahtar için türetir (`server_remote.c:123,218`
kalıbı); `atlas api-key create --scope deploys:confirm` reddedilir.

MCP (`remote_only = true`, stdio adaptöründe yok): `atlas_deploy_propose` (`writes = true`),
`atlas_deploy_status`, `atlas_deploy_list`, `atlas_deploy_cancel` (`writes = true`).
Challenge/confirm için araç **yok**; `tests/test_mcp.c` ad listesi güncellenir.

Politika (`gwpolicy`): `remote_deploy_key = key_…` (çıplak seçici olarak saklanır, dispose
gibi); `operator_accepts_cleartext_deploy = yes` (tek yasal değer). Redler, hepsi MALFORMED:
deploy anahtarı herhangi bir `remote_submit_key` ya da `remote_dispose_key` ile aynı; kabul
satırı deploy anahtarı olmadan; `tls_mode = NONE` altında deploy anahtarı kabul satırsız;
anahtar iki kez. `atlas gateway status` `deploy:` satırı basar (anahtar, kabul durumu).

### D.3 Kuyruk dosyaları

Dizin: `/var/lib/atlas/deploy/{requests,results}`, daemon başlangıçta `0700` oluşturur (yol
biriminin izleyeceği dizin, birim etkinleştirilmeden önce var olsun diye). Yazma her zaman
`tmp + rename`.

`requests/<deploy_uid>.req` (daemon yazar) ve yanında `<deploy_uid>.patch` (bayt bayt artefakt):

```
atlas-deploy-request 1
deploy d…
job j…
repo_root /opt/atlas
base_commit 2922214…
patch_sha256 <64 hex>
patch_bytes <n>
confirmed_by <key_id>
confirmed_at <iso8601>
```

`results/<deploy_uid>.res` (ajan yazar; daemon parse eder, sınırlı, bozuksa deploy FAILED
aşama `INGEST` neden `result malformed`):

```
atlas-deploy-result 1
deploy d…
outcome SUCCEEDED|FAILED
stage PREFLIGHT|APPLY_CHECK|APPLY|BUILD|TEST|INSTALL|RESTART|VERIFY|DONE|ABANDONED
dry_run yes|no
head_before <sha1>
head_after <sha1>
tree_dirty_before yes|no
installed_version <atlas --version ilk satırı>
units atlas.service=active atlas-gateway.service=active …
rollback none|binary|patch
text_bytes <n>
--
<en çok 32 KiB, ajanın aşama satırları ve son komutun çıktısının sonu>
```

Ingest: dosya okunur, satır CAS ile CONFIRMED→SUCCEEDED/FAILED, geçiş `DEPLOY_AGENT`,
commit, sonra dosya silinir (defter tutuyor).

**Kuyruk operatörün de kanalıdır.** Yol birimi hiç etkinleştirilmemişse ya da ajan sonuç
yazamadan ölmüşse, CONFIRMED satır hiçbir şeyin bitiremeyeceği bir satırdır ve
`idx_deploys_one_active` sonraki her deploy'u engeller. Kaçış yeni kod değildir: root,
`results/<uid>.res` dosyasını elle yazar — `outcome FAILED`, `stage ABANDONED`, kısa bir
metin — ve daemon bir sonraki tikte onu her sonuç gibi işler. §F bunu adımıyla yazar.

**Dosya kipleri ve temizlik, değişmez kurallar:** ajan `.res` dosyasını açıkça `0644`
yapar (root'un umask'ı 077 olabilir; `0600 root` bir dosya `atlasd`'nin dizininde silinebilir
ama okunamaz, deploy sessizce CONFIRMED kalırdı). Ajan `.req` ve `.patch` dosyalarını **her
çıkış yolunda** kaldırır: bitince siler; isteği parse edemiyor ve deploy uid'ini bilmiyorsa
`<ad>.bad` olarak yeniden adlandırır. Aksi hâlde `PathExistsGlob` servis her bittiğinde
yeniden silahlanır ve ajan aynı isteği sonsuza dek yeniden koşar.

### D.4 Root ajanı (`deploy/a17/atlas-deploy-agent.sh` → `/usr/local/libexec/atlas/atlas-deploy-agent`)

Conf `/etc/atlas/deploy.conf` (root 0644, `key = value`, bilinmeyen anahtar = ret):

```
tree = /opt/atlas
owner = nocdem
spool = /var/lib/atlas/deploy
build = make
test = ctest --test-dir build -L unit --output-on-failure
install = cmake --install build --prefix /usr/local
binary = /usr/local/bin/atlas
units_system = atlas.service atlas-gateway.service atlas-dispatcher.service atlas-scanner.service
units_user = nocdem:atlas-model-dispatcher.service
ping = /usr/local/bin/atlas daemon ping
dry_run = no
reverse_on_failure = no
```

Aşamalar (her biri sonuca bir satır yazar; bir aşama başarısız olunca `outcome FAILED`,
`stage <aşama>`):

1. `PREFLIGHT`: conf ve istek parse; `patch_sha256` yeniden hesaplanıp karşılaştırılır;
   `repo_root == tree` değilse ret; `tree` bir git deposu; `head_before`, `tree_dirty_before`
   (`git status --porcelain` boş mu) kaydedilir. HEAD ≠ `base_commit` ise **ret değil**,
   kaydedilir; `--check` karar verir.
2. `APPLY_CHECK`: `runuser -u <owner> -- git -C <tree> apply --check <patch>`; asla `--3way`
   (operatörün ağacında işaret bırakır). Başarısız → çıktı sonuca, ağaç dokunulmamış.
3. `APPLY`: `git apply <patch>`.
4. `BUILD`: `runuser -u <owner> -- <build>` (`-C <tree>`).
5. `TEST`: `runuser -u <owner> -- <test>` (`tree` içinde).
   `dry_run = yes` ise burada durulur: yama **her zaman** `git apply -R` ile geri alınır (kuru
   koşu ağacı kirli bırakmaz; geri alma başarısızsa sonuç bunu bağırır), `outcome SUCCEEDED`,
   `stage TEST`, `dry_run yes`. Gerçek koşuda BUILD/TEST başarısızlığı: varsayılan ağaca
   dokunmamak (`rollback none`); `reverse_on_failure = yes` ise `git apply -R` (`rollback
   patch`) — savunma: yalnızca ajanın dakikalar önce eklediği baytları kaldırır, bir
   `reset`/`checkout` değildir; geri alma başarısızsa ağaç bırakılır ve sonuç söyler.
6. `INSTALL`: `cp -p <binary> <binary>.prev`; root olarak `<install>` (`sudo make install`
   **değil**: root olarak yeniden derler ve `build/`'de root'a ait dosya bırakır — kayıtlı
   tuzak). `<binary> --version` sonuca.
7. `RESTART`: sistem birimleri `systemctl restart …`; kullanıcı birimi için komut makinede
   **doğrulanır** (T7): `systemctl --machine=<owner>@.host --user restart <unit>` ya da
   `runuser -u <owner> -- env XDG_RUNTIME_DIR=/run/user/<uid> systemctl --user restart <unit>`.
8. `VERIFY`: her birim `is-active`; her `MainPID` için `readlink /proc/<pid>/exe` sonu
   `(deleted)` değil; `runuser -u <owner> -- <ping>` ≤ 40 s. Başarısız → `<binary>.prev` geri
   kurulur, birimler yeniden başlatılır, `rollback binary`, `outcome FAILED`, `stage VERIFY`.
9. `DONE`: sonuç yazılır (tmp + rename, `0644`), istek dosyası ve yaması silinir, çıkış 0.

Ajan her zaman bir sonuç yazar (PREFLIGHT'ta bile) ve isteği her yolda kaldırır (D.3'teki
değişmez kural). `set -u`; `pipefail + grep -q` tuzağı yok. `owner == $(id -un)` ise
`runuser` atlanır (testin çalışma biçimi). Root oneshot'tan `runuser -u <owner> --` neredeyse
boş bir ortam verir; ajan `HOME=<owner'ın ev dizini>` ve `PATH`'i açıkça kurar, `git` ve
`ctest`'in bu ortamda davranışı T7'de makinede denenir.

`deploy/a17/atlas-deploy.path`:
```
[Unit] Description=Atlas deploy spool watcher
[Path] PathExistsGlob=/var/lib/atlas/deploy/requests/*.req
       Unit=atlas-deploy.service
[Install] WantedBy=multi-user.target
```
`deploy/a17/atlas-deploy.service`: `Type=oneshot`, `User=root`,
`ExecStart=/usr/local/libexec/atlas/atlas-deploy-agent`, sandbox yok (root olarak kurulum ve
restart yapar; bu birimin varlığı, operatörün root olarak çalışan bir kanalı kabul ettiği
anlamına gelir — şablonun başlığı bunu yazar).

### D.5 Bu oturumdan akış

1. Steward: `atlas_job_submit(...)` → iş SUCCEEDED, `changes.patch` saklı.
2. Steward: `atlas_deploy_propose(job)` → `deploy d…`, `patch_sha256`, `patch_bytes`.
3. Operatör: Mission Control → Deploys → ilgili satır → özetin ilk sekiz hex'ini yazar →
   onaylar. Ekranda: iş, taban commit, tam özet, bayt, zayıflık cümlesi.
4. Ajan: uygular, derler, test eder, kurar, beş birimi yeniden başlatır (gateway dâhil; tünel
   birimi dokunulmaz). Bağlantının geri geldiğinin kanıtı bu oturumda var: 15:58'de beş birim
   yeniden başlatıldı, `atlas-tunnel.service`'in MainPID'si daha eski, ve steward'ın
   `jffa5ae5…` işi 16:24Z'de, restart'tan sonra aynı tünelden kuyruğa girdi.
5. Steward: `atlas_deploy_status(deploy)` → SUCCEEDED/FAILED, aşama, geri alma, sürüm, metin.
   CONFIRMED ve sonuç yoksa yaşı görünür.

Yapılamayanlar, tasarım gereği: ajan conf'unu, yol birimini, politikaları ve ajanın kendisini
değiştirmek (root'a ait, operatör adımı); yalnızca restart (yamasız) — ayrı bir fiil olurdu,
eklenmedi; Atlas dışındaki bir ağacı hedeflemek.

---

## E. Executor görevleri

Dağıtım: T0 → T1 → T2 → (T3 ‖ T4 ‖ T5) → T6 → T7. İnceleme (Opus): T2 (yetki: confirm
işlemi), T3 (rota/politika/scope), T5 (root kodu: apply/rollback/restart doğruluğu). Diğerleri
test + diff okumasıyla kapanır. Sonnet yazar.

### T0: Yedek ve sağlık

- [ ] `git diff HEAD > <scratchpad>/a14r-baseline-2026-09-07.patch`; iki izlenmeyen dosyanın kopyası.
- [ ] `make && cd build && ctest -L unit --output-on-failure` → geçer.

### T1: Migration 33, tür sözlüğü, DB işlemleri

**Dosyalar:** Oluştur `include/atlas/deploy.h`, `src/db/db_deploy.c`; değiştir
`src/db/migrate.c`, `include/atlas/db.h:24`, `CMakeLists.txt` (kaynak listesi), yedi
migration testi; Oluştur `tests/test_migrate33.c`, `tests/test_db_deploy.c`
(`tests/CMakeLists.txt` + `unit` etiketi).

**Arayüz:**

```c
/* include/atlas/deploy.h */
typedef enum atlas_deploy_state {
    ATLAS_DEPLOY_UNKNOWN = 0, ATLAS_DEPLOY_PROPOSED, ATLAS_DEPLOY_CONFIRMED,
    ATLAS_DEPLOY_SUCCEEDED, ATLAS_DEPLOY_FAILED, ATLAS_DEPLOY_CANCELLED
} atlas_deploy_state;
bool atlas_deploy_state_is_terminal(atlas_deploy_state s);          /* SUCCEEDED, FAILED, CANCELLED */
const char *atlas_deploy_state_name(atlas_deploy_state s);
bool atlas_deploy_state_parse(const char *s, atlas_deploy_state *out); /* UNKNOWN never parses */

typedef enum atlas_deploy_actor {
    ATLAS_DEPLOY_ACTOR_UNKNOWN = 0, ATLAS_DEPLOY_ACTOR_REMOTE_CREDENTIAL,
    ATLAS_DEPLOY_ACTOR_REMOTE_OPERATOR_CONFIRMED, ATLAS_DEPLOY_ACTOR_DEPLOY_AGENT
} atlas_deploy_actor;

#define ATLAS_DEPLOY_RESULT_TEXT_MAX (32u * 1024u)
#define ATLAS_DEPLOY_CONFIRMATION_HEX 8u

typedef struct atlas_deploy_row { /* every column, owned strings in atlas_buf */ … } atlas_deploy_row;

atlas_status atlas_db_deploy_propose_in_tx(atlas_db *db, const char *job_uid, const char *key_id,
                                           atlas_buf *deploy_uid_out, atlas_err *err);
/* Asked inside both submit write points: refuses a root submission while a deploy is CONFIRMED. */
atlas_status atlas_db_deploy_confirmed_uid(atlas_db *db, int64_t repo_id, atlas_buf *uid_or_empty, atlas_err *err);
/* CAS transitions; each requires exactly one changed row and records a deploy_transitions row. */
atlas_status atlas_db_deploy_confirm_in_tx(atlas_db *db, const char *deploy_uid, const char *key_id,
                                           const char *confirmation, atlas_err *err);
atlas_status atlas_db_deploy_cancel_in_tx(atlas_db *db, const char *deploy_uid, const char *key_id, atlas_err *err);
atlas_status atlas_db_deploy_mark_spooled_in_tx(atlas_db *db, const char *deploy_uid, atlas_err *err);
atlas_status atlas_db_deploy_finish_in_tx(atlas_db *db, const char *deploy_uid, const atlas_deploy_result *r, atlas_err *err);
atlas_status atlas_db_deploy_get(atlas_db *db, const char *deploy_uid, atlas_deploy_row *out, bool *have, atlas_err *err);
atlas_status atlas_db_deploy_list(atlas_db *db, const char *key_id_or_null, … , atlas_err *err);
atlas_status atlas_db_deploy_confirmed_unspooled(atlas_db *db, atlas_buf *uids_out, atlas_err *err);
```

`propose_in_tx` reddeder: iş yok, iş SUCCEEDED değil, mod `patch` değil, `changes.patch`
`content_stored = 0` ya da boş, işi başka bir key kuyruğa almış (`orch_jobs.submit_key_id`),
aktif deploy var (indeks de yakalar; C cümle verir), iş zaten SUCCEEDED deploy edilmiş.
`confirm_in_tx` reddeder: durum PROPOSED değil, `confirmation` özetin ilk sekiz hex'i değil
(küçük harf, tam uzunluk), terminal olmayan iş var (`orch_jobs` sayımı, sözlük
`atlas_orch_state_is_terminal`'dan; SQL ve C sözlüğü `tests/test_orch_run.c` kalıbıyla
karşılaştırılır).

- [ ] **Adım 1 (kırmızı):** `test_migrate33.c` (`test_migrate32.c` şablon): taze DB 33; 32'de durmuş DB kayıpsız; `PRAGMA index_list(deploys)` iki kısmi indeksi gösterir. Yedi 32→33.
- [ ] **Adım 2:** M33 + `ATLAS_SCHEMA_VERSION 33`. `ctest -R test_migrate` geçer.
- [ ] **Adım 3 (kırmızı):** `test_db_deploy.c`: fixture DB'ye SUCCEEDED bir iş + `changes.patch` artefaktı ekle (var olan `atlas_db_orch_*` yazıcılarıyla, tests/test_orch_*.c'deki tohumlama kalıbı); `propose` → PROPOSED, özet doğru; ikinci `propose` aynı repoda → ret cümlesi "a deploy is already in flight"; `confirm` yanlış önekle → ret, satır PROPOSED; doğru önekle → CONFIRMED, `deploy_transitions` aktörü `REMOTE_OPERATOR_CONFIRMED` ve key id; terminal olmayan bir iş varken `confirm` → ret "N job(s) are not terminal"; `finish` FAILED aşama BUILD → satır FAILED; FAILED sonrası aynı iş yeniden `propose` edilebilir; SUCCEEDED sonrası edilemez; `cancel` yalnızca PROPOSED'dan ve teklif eden key ile.
- [ ] **Adım 4:** `db_deploy.c`; `atlas_deploy_state_is_terminal` ile SQL yükleminin sözlük karşılaştırması testi. `ctest -R 'test_db_deploy|test_migrate33'` geçer.

### T2: Daemon yöntemleri, kuyruk yazımı, ingest

**Dosyalar:** Oluştur `src/ipc/server_deploy_remote.c`, `src/daemon/deploy_spool.c`;
değiştir `src/ipc/server.c` (yöntem tablosu; A16'nın `decision.remote_*` satırlarının yanı),
`src/daemon/writer.c` (iş türleri: `DEPLOY_SPOOL`, `DEPLOY_INGEST`; ikisi de sınırlı ve
drainable, `default:`'suz switch'lere satır), `src/daemon/daemon.c:210` civarı (başlangıçta
ingest + yeniden kuyruklama), `src/daemon/watcher.c` (tik türetimi: `results/` boş değilse
`DEPLOY_INGEST` kuyruğa; saf okuma), `include/atlas/apikey.h`/`src/gw/apikey.c` (scope);
test `tests/test_deploy_rpc.c` (`daemon` etiketi, `RUN_SERIAL`, `TIMEOUT 900`),
`tests/test_orch_rpc.c:107` (yasak adlar), `tests/test_writer_kinds.c` varsa (switch tamlığı).

**Kuyruk yazımı:** confirm işlemi commit olduktan sonra, aynı yazar işi içinde, artefakt
baytları `requests/<uid>.patch`'e, ardından `<uid>.req` tmp+rename; sonra
`mark_spooled_in_tx`. Bir dosya hatası satırı CONFIRMED/`spooled_at NULL` bırakır ve loglanır;
daemon başlangıcı `confirmed_unspooled` ile yeniden dener.

**Ingest:** `results/*.res` sıralı (ad sırası), her biri sınırlı parse
(`atlas_deploy_result_parse(bytes, len, &r, err)`: başlık satırı, bilinen anahtarlar, `--`
sonrası ≤ 32 KiB, aksi `result malformed`), `finish_in_tx`, commit, `unlink`.

- [ ] **Adım 1 (kırmızı):** `test_deploy_rpc.c` (`test_orch_remote_rpc.c` şablon; gateway daemon'u bir submit anahtarı + bir deploy anahtarıyla başlar): (a) tohumlanmış SUCCEEDED iş için `deploy.remote_propose` (submit token) → `deploy`, `patch_sha256`; `"dry_run"` alanı taşıyan teklif → ret; (b) `deploy.remote_challenge` + `deploy.remote_confirm` (deploy token, doğru önek) → CONFIRMED; `requests/<uid>.req` ve `.patch` var, `.patch` sha256 == kayıt; (c) submit token ile confirm → ret (scope); yanlış önek → ret; (d) test `results/<uid>.res` yazar (sahte ajan), daemon yeniden başlatılır (`fx_daemon_stop/start`) → `deploy.remote_get` SUCCEEDED, `stage DONE`, metin; (e) bozuk `.res` → FAILED aşama INGEST; elle yazılmış `outcome FAILED` + `stage ABANDONED` → FAILED, sonraki teklif kabul edilir; (f) tikle ingest: daemon açıkken `.res` yaz, `fx_wait_for_substring` ile `remote_get` SUCCEEDED olana kadar bekle; (g) terminal olmayan bir iş (QUEUED) varken confirm → ret; (h) deploy CONFIRMED iken `job.remote_submit` ve `job.submit` → ret, cümlede deploy uid; sonuç geldikten sonra aynı gönderim kabul; (i) `deploy.remote_apply` vb. → `unknown method`.
- [ ] **Adım 2:** Yöntemler, kuyruk, ingest, scope türetimi. Test geçer.
- [ ] **Adım 3:** `test_orch_rpc.c:107` listesine `deploy.remote_apply`, `deploy.remote_install`, `deploy.remote_restart`, `deploy.remote_run`. `ctest -R 'test_orch_rpc|test_deploy_rpc|test_orch_remote_rpc|test_gw_remote'` geçer.
- [ ] **Adım 4 (inceleme, Opus):** (1) confirm işlemi içinde kontrol edilen dört şart (durum, önek, aktif iş yok, challenge) atomik mi? (2) Kuyruk dosyası commit'ten **sonra** mı yazılıyor, ve yazılamazsa satır ne diyor? (3) Ingest, ajanın metnini hiçbir dalda yorumluyor mu (`strstr` yok)? (4) `deploys:confirm` yalnızca politikanın adlandırdığı anahtar için mi türetiliyor?

### T3: Gateway politikası, rotalar, Mission Control, `gateway status`

**Dosyalar:** `include/atlas/gwpolicy.h`, `src/gw/gwpolicy.c`, `src/gw/gateway.c`
(rota tablosu + status), `src/gw/ui/mission-control.html`, `deploy/a9/gateway.conf.template`
(iki satır, dispose satırlarının yanı); test `tests/test_gateway.c` (politika grameri, status
çıktısı, sayfa grep'i), `tests/test_gw_submit.c` ya da yeni `tests/test_gw_deploy.c`
(rotalar, daemon fixture'ı ile).

- [ ] **Adım 1 (kırmızı):** `test_gateway.c`: `remote_deploy_key` parse; aynı id submit/dispose ile → MALFORMED; kabul satırı anahtarsız → MALFORMED; NONE altında kabulsüz → MALFORMED; iki kez → MALFORMED; `gateway status` insan ve JSON `deploy:` satırı; sayfa baytlarında zayıflık cümlesi ("installs code that runs as root on this machine") ve sekiz hex girişi; sayfada `confirm`'in bir MCP aracı olarak **geçmediği** (A15 tarayıcısı sayfayı zaten tarıyorsa oraya ekle).
- [ ] **Adım 2:** Politika + status + sayfa (dispose diyaloğunun şekli kopyalanır; JS testte çalıştırılmaz). Test geçer.
- [ ] **Adım 3 (kırmızı):** rota testi: `POST /api/v1/deploy/propose` (submit bearer) → 2xx; `POST /api/v1/deploy/confirm` submit bearer ile → 403; deploy bearer + doğru önek → 2xx; rota tablosu dışı alan (`"patch":…`) → 4xx, soket mesajına girmedi (`test_gw_submit.c`'deki "no route becomes a socket message" kalıbı).
- [ ] **Adım 4:** Rotalar. `ctest -R 'test_gateway|test_gw_'` geçer.
- [ ] **Adım 5 (inceleme, Opus):** rota tablosunda `confirm` yalnızca `deploys:confirm` scope'una mı bağlı; propose `jobs:submit`'e mi; sayfa hangi cümleyi gösteriyor?

### T4: MCP araçları

**Dosyalar:** `src/mcp/mcp_tools.c` (dört araç, `remote_only = true`), `tests/test_mcp.c`.

- [ ] **Adım 1 (kırmızı):** `test_mcp.c` beklenen ad listesine dört ad; stdio adaptöründe görünmediklerinin mevcut `remote_only` kontrolü; A15 fiil tarayıcısı geçer (adlarda yetki fiili yok).
- [ ] **Adım 2:** Araçlar (`atlas_job_submit`'in şema/iletim kalıbı). `ctest -R test_mcp` geçer.

### T5: Root ajanı, birimler, conf şablonu ve ajan testi

**Dosyalar:** Oluştur `deploy/a17/atlas-deploy-agent.sh`, `deploy/a17/atlas-deploy.path`,
`deploy/a17/atlas-deploy.service`, `deploy/a17/deploy.conf.template`; test
`tests/test_deploy_agent.c` (`integration`).

- [ ] **Adım 1 (kırmızı):** `test_deploy_agent.c`: fixture git deposu (`Makefile`: `all:` bir dosya üretir; `test` hedefi `true`), conf `owner = $(id -un)`, `build = make`, `test = make test`, `install = true`, `units_system` boş, `units_user` boş, `ping = true`, `spool = <fixture>/spool`; istek + yama (fixture'da üretilmiş gerçek `git diff`) yaz; ajanı `atlas_proc_run` ile `/bin/bash <script> --conf <conf>` argv'siyle çalıştır — mutlak `argv[0]` ve açık argv, `sh -c` değil; `make smoke`'un `scripts/smoke.sh`'ı çalıştırmasıyla aynı sınıf. Vakalar: (a) temiz yama → `outcome SUCCEEDED`, `stage DONE`, ağaçta yama uygulanmış, `.req` ve `.patch` silinmiş, `.res` var ve kipi `0644`; (b) uygulanmayan yama → `stage APPLY_CHECK`, ağaç bayt bayt aynı (`fx_tree_digest`), `.req` silinmiş; (c) `build` başarısız, `reverse_on_failure = no` → `stage BUILD`, `rollback none`, ağaç yamalı; (d) aynı, `= yes` → `rollback patch`, ağaç eski hâlinde; (e) `dry_run = yes` → `stage TEST`, `dry_run yes`, ağaç eski hâlinde; (f) özet uyuşmazlığı → `stage PREFLIGHT`, ağaç dokunulmamış, `.req` silinmiş; (g) `repo_root` conf'taki `tree` ile farklı → `PREFLIGHT` ret, `.req` silinmiş; (h) parse edilemeyen `.req` → `<ad>.bad` olarak yeniden adlandırılmış, `requests/` içinde `.req` kalmamış; (i) sonuç metni 32 KiB'ı aşmaz.
- [ ] **Adım 2:** Ajan, birimler, conf şablonu. `bash -n`; test geçer.
- [ ] **Adım 3 (inceleme, Opus):** apply/rollback sırası; `.prev` geri kurulumunun yalnızca VERIFY başarısızlığında olduğu; `set -u` altında boş `units_user`; `runuser` atlama koşulu; sonucun her yolda yazıldığı.

### T6: Belgeler

`docs/remote-deploy.md` (sezon belgesi: §A'nın tersine çevirme bölümü, zayıflık paragrafı,
akış, kuyruk biçimleri, ajan conf'u, "commit etmez" ve HEAD ≠ ağaç sonucu), `CLAUDE.md`
(A17 satırları; "A14R makes it five of each" cümlesine deploy yöntemlerinin ayrı bir sözlük
olduğu notu; sezon tablosu satırı), `docs/remote-access.md`, `docs/roadmap.md`,
`docs/engineering-rules.md` (A17 kuralları: binary systemctl çağırmaz; ajan SQLite açmaz;
teklif eden ≠ onaylayan; yalnızca saklanan yama; bir uçuşta bir deploy), `docs/extending.md`
(yeni bir aşama/durum eklemenin bedeli), `docs/backlog.md` (yalnızca-restart fiili yok; HEAD ≠
ağaç birikimi; `--3way` neden yok), `deploy/a9/gateway.conf.template`.

- [ ] `grep -n 'remote_apply' CLAUDE.md docs/*.md` → her geçiş A17'nin tersine çevirme notuna işaret eder.
- [ ] `docs/remote-deploy.md`'de bir cümle: `deploy_transitions.actor`'daki
      `REMOTE_OPERATOR_CONFIRMED`, karar defterindeki adla aynı anlamı taşır — bir kanal, bir
      kişi değil — ama ayrı bir sözlüktür; iki enum birleştirilmez.
- [ ] `docs/remote-deploy.md` §F karşılığı: yol birimi etkin değilken ya da ajan ölmüşken
      CONFIRMED kalan deploy'un elle yazılan `ABANDONED` sonucu ile kapatılması, adımıyla.

### T7: Makinede salt okunur doğrulamalar (executor; kurulum yok)

- [ ] `systemctl --version` ilk satırı; root olarak kullanıcı birimini yeniden başlatma komutunun **durum** biçimi denenir (restart değil): `sudo -n systemctl --machine=nocdem@.host --user status atlas-model-dispatcher.service` çalışıyor mu; çalışmıyorsa `sudo -n runuser -u nocdem -- env XDG_RUNTIME_DIR=/run/user/1000 systemctl --user status …`. Çalışan biçim ajan conf şablonunun yorumuna yazılır.
- [ ] `sudo -n runuser -u nocdem -- env | sort` ile root oneshot'ın vereceği ortam okunur; `HOME` ve `PATH` yoksa ajan bunları kurar; `runuser -u nocdem -- git -C /opt/atlas status --porcelain` ve `runuser -u nocdem -- ctest --test-dir /opt/atlas/build -N` (yalnızca liste) bu ortamda çalışıyor mu, sonucu conf şablonunun yorumuna yazılır.
- [ ] `PathExistsGlob`'un var olmayan dizinde silahlanıp silahlanmadığı `man systemd.path` ile okunur; daemon dizini başlangıçta oluşturduğu için sıra "önce yeni daemon, sonra `enable --now`" olarak F'ye yazılır.
- [ ] `atlas api-key create --no-scopes` yardım metni ve `gateway.conf.template:88-122` dispose satırları okunur; F'deki komutlar birebir bunlardan türetilir.

### E.5 Kod doğrulaması

- `make && cd build && ctest -L unit`; `ctest -L integration`; `ctest -R 'test_deploy_rpc|test_orch_rpc|test_gw_|test_orch_remote_rpc'`; `make smoke`; sonra tam `make test` (125 + yeni 4 = 129 geçer).
- `build/atlas gateway status` bir kez gerçek binary'den (politika okur, hiçbir şey bağlamaz).

---

## F. İlk etkinleştirme (operatör, makinede) ve sonrası

### F.1 Bir kerelik, root

1. Bu planın Atlas'ı **son elle kurulum**: `bash /opt/atlas/deploy.local.sh` (yalnızca daemon'u
   yeniden başlatır; ardından elle `sudo systemctl restart atlas-gateway atlas-dispatcher
   atlas-scanner` ve `systemctl --user restart atlas-model-dispatcher`). Yeni daemon
   `/var/lib/atlas/deploy/{requests,results}` dizinlerini oluşturur.
2. Deploy kimlik bilgisi: `atlas api-key create --label deploy --no-scopes`. Gizli değer bir
   kez gösterilir; yalnızca operatörün tarayıcısında/parola yöneticisinde durur, steward'a
   verilmez.
3. `/etc/atlas/gateway.conf` (root, mevcut `.pre-*` yedek geleneğiyle):
   `remote_deploy_key = key_<seçici>` ve `operator_accepts_cleartext_deploy = yes`
   (tls NONE olduğu sürece zorunlu; §G.4).
4. Ajan: `install -o root -g root -m 0755 deploy/a17/atlas-deploy-agent.sh /usr/local/libexec/atlas/atlas-deploy-agent`;
   `install -m 0644 deploy/a17/deploy.conf.template /etc/atlas/deploy.conf` ve değerleri
   makineye göre düzenle (`test =` satırı §G.3); `install -m 0644 deploy/a17/atlas-deploy.{path,service} /etc/systemd/system/`;
   `systemctl daemon-reload && systemctl enable --now atlas-deploy.path`.
5. `sudo systemctl restart atlas.service atlas-gateway.service` (politika yeniden okunsun);
   `atlas gateway status` `deploy:` satırını gösterir.
6. **Kuru koşu:** `/etc/atlas/deploy.conf`'ta `dry_run = yes` iken steward küçük bir işi
   teklif eder, operatör tarayıcıdan onaylar, ajan apply/build/test yapıp yamayı geri alır,
   steward `atlas_deploy_status` ile `dry_run yes`, `stage TEST` okur. Sonra `dry_run = no`.

7. **Takılı kalan deploy:** yol birimi etkin değilken onaylanmış ya da ajanın sonuç yazamadan
   öldüğü bir deploy CONFIRMED kalır ve sonraki teklifleri engeller. Root olarak
   `/var/lib/atlas/deploy/results/<uid>.res` elle yazılır (`atlas-deploy-result 1`,
   `deploy <uid>`, `outcome FAILED`, `stage ABANDONED`, `text_bytes 0`, `--`), kip `0644`;
   daemon bir sonraki tikte işler, steward FAILED/ABANDONED okur.

### F.2 Her güncellemede

Steward: gönder → teklif et → (operatör: tarayıcıdan sekiz hex ile onay) → bağlantı geri
gelince `atlas_deploy_status`. Operatörün makinede yapması gereken: **yok**, §G.1'in cevabına
bağlı olarak deploy sonrası commit hariç.

Operatör adımı gerektirmeye devam edenler: politika ve ajan conf'u düzenlemek, ajanı ve
birimleri güncellemek, `deploy.conf`'un `test =` satırını değiştirmek, yamasız restart.

---

## G. Yalnızca operatör kararı gerektiren açık noktalar

Kapanan: onay yöntemi (ayrı deploy kimlik bilgisi, tarayıcı, yazılan önek) — kabul edildi,
yeniden açılmaz. Aşağıdakiler açık; hiçbiri için karar verilmedi ve canlı kurulum onaylanmadı.

1. **Deploy sonrası kim commit eder?** Ajan commit etmez (kesin kural). Seçenekler: operatör
   her deploy sonrası elle; ya da ajan conf'una yazılı açık yetkiyle (`commit_after_deploy =
   yes`, mesaj şablonu sabit, yalnızca yamanın dokunduğu dosyalar) ajan. İkincisi bu planda
   **yok**; istenirse ayrı bir görevdir. Cevap verilmezse ikinci deploy'dan itibaren
   `apply --check` retleri başlar.
2. **Deploy kimlik bilgisini hangi cihaz taşır?** Tarayıcı onayı bu cihazdan yapılır; steward'ın
   bağlayıcısına verilmez.
3. **Kurulum öncesi hangi testler?** `ctest -L unit` saniyeler; `make test` ~10 dakika ve
   deploy süresince gateway ayakta kalır (kurulum ve restart en sonda). `deploy.conf`'un
   `test =` satırı.
4. **Root olarak kod kuran bir kanal düz metin LAN üzerinde kabul ediliyor mu?** Kabul satırı
   bunun yazılı hâlidir; alternatif deploy onayını yalnızca tünel/TLS arkasından kabul etmektir
   (`tls_mode = REVERSE_PROXY` + proxy), ki bu A9'un kendi belgelenmiş yoludur ve bu planın
   dışındadır.

---

## Öz denetim

- Kapsam: teklif/izleme/okuma → T2+T4; onay → T2+T3; uygulama/kurulum/restart → T5; ilk
  etkinleştirme → F.1; "bağlantı geri gelince oku" → D.5 adım 5 + ingest tik türetimi.
- Yer tutucu yok; her test vakası ve dosya biçimi yazılı.
- Ad tutarlılığı: `deploy.remote_{propose,get,list,cancel,challenge,confirm}`,
  `atlas_deploy_{propose,status,list,cancel}`, `deploys:confirm`, `remote_deploy_key`,
  `operator_accepts_cleartext_deploy`, `atlas-deploy.{path,service}`, `/etc/atlas/deploy.conf`,
  `/var/lib/atlas/deploy/{requests,results}` — D ve E'de aynı.
