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
