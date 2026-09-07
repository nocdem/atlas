# Otonom tur günlüğü

Operator 2026-09-05'te tek seferlik bir yetki devri verdi
(`docs/plans/2026-09-05-role-orchestration-CONTRACT.md` §9): Steward, A
sezonunun planını kendi adına onaylayabilir — ama önce charter'ın istediği
incelemeyi yapıp bulgularını yazmak şartıyla. Devrin gerekçesi planın kendisi
değildi: *"normalde böyle çalışmayacak ama burada elde edeceğimiz deneyimleri
bir yere kaydetmeyi istiyorum."*

Bu dosya o kayıttır. Operator makinede değilken ne olduğu, neyin işe yaradığı,
neyin yaramadığı ve otonom çalışmanın nerede insan kararına çarptığı burada
durur. Başarıyı olduğundan iyi göstermek charter §9'un yasakladığı şeydir; bu
dosya da ona tabidir.

## Nasıl okunur

Her giriş bir olayı ve **sebebini** taşır. Sebebini yazamadığım bir olayı
gerçekmiş gibi yazmam — projenin kendi raporlama kuralı.

---

## 2026-09-05 — devir alındı, tur başladı

- Operator makineden ayrıldı. Elde: A sezonunun planını yazan bir Planner
  (Fable), kendi temposunu ayarlayan bir loop, ve commit yetkisi.
- Devrin sınırı yazıldı: plan onayı evet; push, deploy, sezon kapanışı, gerçek
  model işi ve kapsam büyütme hayır.
- Bu noktada henüz hiçbir plan gelmedi. Sonraki girişler buradan devam eder.

## 2026-09-05 — A sezonunun planı geldi; Steward incelemesi

Planner (Fable) `docs/plans/2026-09-05-a-channel-and-signin.md`'yi yazdı: 1264
satır, beş görev, durumu `PROPOSED`, ağaçta başka hiçbir dosyaya dokunulmamış.

### Charter §4'ün sorduğu yedi soru, sırayla

1. **Operator hedefini karşılıyor mu?** Evet. Üç şeyin üçü de var: TLS önde,
   açık metin kabulünün kalkması, girişin bir kez yapılıp hatırlanması.
2. **Kapsam dışına çıkıyor mu?** İki ekleme var ve ikisi de haklı.
   `Secure` çerez niteliği için test — TLS'e geçen bir sezonun çerezinin o
   niteliği taşıması gerekir ve bugün bunu hiçbir test yoklamıyor. Ve
   `trust_forwarded_for` belgelerinin düzeltilmesi — vekil sunucu tam da o
   başlığın önemli olduğu şey, ve belgeler yanlış söylüyor.
3. **Gereksiz iş üretiyor mu?** Hayır. C'de kod değişikliği yok; sezonun
   ağırlığı dağıtımda.
4. **Mevcut invariant'larla çelişiyor mu?** Hayır, ve çelişmediğini altı
   maddede kendisi savunuyor. En dürüst yeri: nginx'i yolun içine giren
   **ikinci bir principal** olarak adlandırıyor — her isteğin her baytını ve
   özel anahtarı gören, `www-data` olarak çalışan bir süreç. Gizlemek yerine
   maliyet olarak yazmış.
5. **Varsayımlar açık mı?** Evet, ve ağaç `81e71b1`'de yeniden doğrulanmış.
6. **Test ve kanıt yaklaşımı yeterli mi?** Evet. Ayrıca kendi ilk taslağının
   `Secure` testini hiç giriş yapmayan bir teste bağladığını, incelemede
   yakalandığını ve düzeltildiğini plana yazmış. Bir planın kendi hatasını
   kaydetmesi, bu projede istenen şeyin ta kendisi.
7. **Operator kararı gizlenmiş mi?** Hayır. Üç karar satırı ve dokuz
   "belirsiz" sorusu açıkta duruyor; hiçbiri kendi tercihine dönüştürülmemiş.

### Steward'ın kendi doğruladıkları

Planın üç olgusal iddiasını kabul etmedim, yokladım:

- **`trust_forwarded_for` hiçbir yerde tüketilmiyor.** Doğru. `gwpolicy.c`
  ayrıştırıp bir alana yazıyor, `gateway.c`'deki tek geçiş bir yorum, istek
  işleyen hiçbir yolda kullanılmıyor. Üç belge ve iki başlık yorumu aksini ima
  ediyor.
- **`Secure` niteliğini hiçbir test yoklamıyor.** Doğru; `tests/test_gw*.c` ve
  `test_gateway.c` içinde tek bir eşleşme yok.
- **nginx kurulu ve boşta, apache 80/443'ü tutuyor.** Doğru; ikisi de `active`,
  gateway `192.168.0.198:8799`'da dinliyor.

### Bulgu: Steward'ın kendi bıraktığı bayat olgu

Planner'ın 9. sorusu haklı ve cevabı bende: `docs/remote-submission.md` ikinci
gönderim anahtarını `key_01364e94e1dcbad4` diye adlandırıyor, canlı politikada
`key_1515cefd1f9f71a4` yazıyor. Sebep, belge yazıldıktan **sonra** Steward'ın o
anahtarı döndürmesi — gizli dizgenin bir parçası ekrana düştüğü için. Yani
belge yanlış değil, bayat, ve bayatlatan Steward. T5'in işi.

### Devrin ürettiği asıl bulgu

Plan bunu kendi yazmış ve doğru: **§9 bu sezonda tek bir görev açıyor.** T1
dağıtılabilir; T2 iki Operator satırı bekliyor; T3 dağıtımın kendisi ve
Operator'ın makine başında olmasını gerektiriyor; T4 cihazları istiyor; T5
dağıtılmış olguları anlatıyor.

Bunun deneyim olarak değeri şu: **otonom bir tur, işin cinsine göre çok farklı
uzunlukta oluyor.** Kod sezonunda (A14) on iki görevin onu otonom yürüdü. Kanal
sezonunda beş görevin biri yürüyor. Aradaki fark yetenek değil; A14 dosya
değiştiriyordu, bu sezon makine değiştiriyor, ve makineyi değiştirmek charter'ın
Operator'a ayırdığı şey.

### Karar

Plan, charter §4'ün yedi sorusunu da geçti. Sözleşme §9'un verdiği tek seferlik
devirle **Steward tarafından onaylandı** — Operator adına değil, kendi adına.
Operator satırları cevapsız duruyor ve devir onları kapsamıyor.

Sıradaki adım: T1 dağıtılır, sonucu doğrulanır, ve tur orada durur.

## 2026-09-05 — T1 yürüdü, tur durdu

Executor (Sonnet) T1'i uyguladı: `c9b4130`, ardından Steward'ın geri
gönderdiği tek satırlık kapanış `5766384`.

**Ne yapıldı.** Oturum çerezinin `Secure` niteliği ilk kez iddia edildi — biri
`REVERSE_PROXY` altında doğru, diğeri `tls_mode = NONE` altında yanlış olmak
üzere iki yönlü, yani hiçbir şey yapmayan bir kod bu testi geçemez. nginx
referans bloğu `deploy/a9/nginx-atlas.conf.example` olarak yazıldı — Atlas'ın
asla kurmadığı, operatörün kurduğu bir dosya. `trust_forwarded_for`'un bir şey
vaat eden dört cümlesi düzeltildi ve gerçek durum `docs/backlog.md`'ye geçti.
Atlas'a değişiklik gerekçesi `MODEL_PROPOSAL` olarak kaydedildi — sözleşme
§10'un ilk kez uygulandığı yer.

**Steward'ın doğruladıkları.** Commit yalnızca planın adlandırdığı yedi dosyaya
dokunuyor. İki iddia ilk çalıştırmada geçti, ki plan bunu bir kapı olarak
yazmıştı: geçmeselerdi planın olgu bölümü yanlış olurdu ve plan önce
düzeltilirdi. Dört test paketi ve uyarısız derleme.

### Bulgu: bir düzeltme kusuru kapatmak yerine taşıyabilir

Executor gövdeyi düzeltti, başlığı bırakti — çünkü plan dört yeri adlandırıyordu
ve başlık onlardan biri değildi. Sonuç, dört satır arayla kendi kendiyle çelişen
bir yorum: ilk satır "bir eşin dakikada yapabileceği istek", gövde "toplam oran".
Öncesinde yorum baştan sona yanlıştı; sonrasında iki cevap veriyordu ve okuyanın
aklında kalacak olan ilki.

Executor bunu **düzeltmedi, bildirdi** — charter §6'nın istediği tam olarak bu,
ve doğru davranış. Steward geri gönderdi, çünkü bu yeni kapsam değil: bir görevin
kendi bıraktığı çelişkiyi kapatmak o görevin işidir. Tek kelime değişti.

Deneyim olarak kaydı: **rol sınırları çalıştı, ama kendiliğinden değil.**
Executor doğru yerde durdu ve Steward'ın bakması gerekti. Kimsenin bakmadığı bir
kurulumda o çelişki, "düzeltildi" diye kapanmış bir görevin içinde kalırdı.

### Turun durduğu yer

T1 bitti. Kalan dört görevin hiçbiri dağıtılamıyor:

- **T2** — iki Operator satırı bekliyor (giriş şekli, hangi anahtarlar).
- **T3** — dağıtımın kendisi: root düzenlemeleri, iki servis yeniden başlatma,
  sertifika. Charter §1'in Operator'a ayırdığı şey.
- **T4** — Operator'ın cihazlarını istiyor.
- **T5** — T3 ve T4'ün gözlediklerini anlatıyor; henüz gözlenmedi.

**Devir tamamlandı ve yenilenmedikçe bitti.** Sözleşme §9: A sezonunun planı
onaylanıp görevleri dağıtıldığında biter. Dağıtılabilir tek görev dağıtıldı.

### Bu turdan çıkan üç şey

1. **Bir kanal sezonunun otonom yürüyebilen kısmı küçüktür.** Beşte bir. Sebep
   yetenek değil, yetki: iş makineyi değiştiriyor.
2. **Rol ayrımı gerçek bir kusur yakaladı** — ama yakalayan sınırın kendisi
   değil, sınırda duran Executor'ın raporu ve onu okuyan Steward'dı. Otomatik
   değil.
3. **Sözleşme §10 ilk meyvesini verdi:** görev bittiğinde Atlas'ta bir
   `MODEL_PROPOSAL` var. Operator makineye döndüğünde bakacağı yer sohbet değil,
   elden çıkarılabilir bir kayıt.

## 2026-09-07 — Steward rolü devrediliyor

Operator, Steward rolünü bu oturumdan alıp telefondaki ChatGPT'ye veriyor; Atlas
bağlantısını o ele alacak. Bu, sözleşme §7'nin (5.2) tarif ettiği şeyin ilk kez
gerçekleşmesi: rolü kimin doldurduğu değişiyor, rolün yetkisi değişmiyor.

**Devralan neyi okumalı, sırayla.** `docs/authority-and-workflow.md` — yetkisi.
`docs/plans/2026-09-05-role-orchestration-CONTRACT.md` — hedef, ölçülen
kısıtlar, Operator'ın cevapları, §9'un devri ve §10'un Atlas kullanma şartı.
`docs/plans/2026-09-05-a-channel-and-signin.md` — onaylanmış A planı. Ve bu
günlük.

**Nerede duruyoruz.** A planı `PROPOSED` ve Steward onaylı; T1 girdi ve
`src/` altında hiçbir şey değiştirmedi; T2 iki Operator satırı bekliyor; T3
dağıtımın kendisi ve Operator'ın makine başında olmasını istiyor; T4 cihazları;
T5 henüz gözlenmemiş olguları. Dokuz commit push edilmedi — Operator commit
yetkisi verdi, push vermedi.

**Devralanın devralmadıkları.** §9'un devri tükendi: A planı onaylandı ve
dağıtılabilir görevi dağıtıldı. B sezonunun planı yine Operator onayı bekler.
Push, deploy, sezon kapanışı, gerçek model işi ve plandaki üç karar Operator'ın.

**Pratik bağlantı.** Dış model Atlas'a `/mcp` üzerinden bağlanıyor;
`chatgpt-tunnel` kimlik bilgisi hem okuma kapsamlarını hem de politikada
adlandırılmış gönderim hakkını taşıyor, yani iş kuyruğa sokabilir. Kuyruğa
soktuğu her iş para harcar ve günlük sınır altıdır. Gönderim bugün açık metin
bir kanaldan geçmiyor — tünelin kimlik bilgisi bu ana kadar ağ segmentini hiç
geçmiyor; onu geçen tarayıcı anahtarıdır ve A sezonu tam olarak bunun içindir.

**Ve bir uyarı, kendi deneyimimden.** Bu oturumda Steward iki kez rol dışına
çıktı (sözleşme §8): arayüz kodunu kendi yazdı, politika satırlarını kendi
yazdı. İkisi de "hızlı olsun" diye oldu ve ikisi de charter'ın yasakladığı şeydi.
Devralan aynı basınca girecek, çünkü basınç modelden değil işin kendisinden
geliyor: bir şeyi yapmak, onu yaptıracak birini bulmaktan hep daha hızlı görünür.
