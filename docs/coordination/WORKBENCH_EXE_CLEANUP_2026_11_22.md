# Güncel Workbench EXE teslimi ve eski kopyaların temizliği

22 Kasım 2026. Kullanıcı son yapılan uygulamanın EXE olarak bırakılmasını ve
karışıklık yaratan diğer sürümlerin silinmesini istedi. Ürün kodu ve sürüm
değişmedi; kabul edilmiş Workbench 0.3.1 stable paketi esas alındı.

Güncel giriş noktası Workbench deposunda
`out/release/StarDataParadise-v0.3.1/StarDataParadise.exe` olarak korunur.
Masaüstündeki `StarDataParadise.lnk` bu EXE'yi ve aynı klasörü çalışma dizini
olarak kullanır; hedef ve çalışma dizini kısayol yeniden okunarak doğrulandı.
EXE'nin yanındaki DLL, Python, eklenti ve diğer destek dosyaları gereklidir.
EXE SHA-256:
`ec61e6a1a02ebf70464166faacf95c4abb37aa4e8a73da0668a4de1d0efe1168`.

Workbench/Server/CLI/Native SDK depolarının ignore edilen `out/` dizinlerinde
79 Workbench EXE'si bulundu. Güncel dosya bırakıldı; 78 eski/tekrarlanan
`StarDataParadise.exe` ve üç eski Workbench ZIP'i tek tek silindi. Güncel
0.3.1 ZIP'i korundu. Silme listesinde kaynak, Git tarafından izlenen dosya,
kullanıcı ayarı, veri, Server/CLI/worker EXE'si veya başka DLL bulunmaz.
Silmeden önce her hedefin çözülmüş yolu, depo sınırı, reparse point içermemesi
ve envanterdeki SHA-256 değeri kontrol edildi. Özyinelemeli klasör silinmedi;
destek dosyaları, geçmiş test kanıtları ve ertelenen çalışmalar korundu.

Kaynak tabanı: Workbench `94ede3ba82f5be3338759b9acc77672d88da6b8e`,
Server `133500f551f70d72a48f16e2f79aded0a9159c15`.
Başlangıçtaki Server `PROGRESS.md` değişikliği ve untracked PyPI kaydı korundu.

Doğrulamalar ve ham kanıtlar Workbench deposunda
`out/version-cleanup-20261122/` altındadır:

| Sonuç | Doğrulama |
| --- | --- |
| PASS | `payload-sha256.json` içindeki 2.644 dosyanın SHA-256 değeri ve EXE ProductVersion 0.3.1 eşleşti; `payload-verification.json`. |
| PASS, exit 0 | `python -B packaging/classic-workbench/tests/artifact_runtime_acceptance.py --runtime out/release/StarDataParadise-v0.3.1 --output out/version-cleanup-20261122/runtime-bindings`: 126 PE dosyası, 1.318 import, 19 CLR assembly ve 146 assembly başvurusu; `runtime-bindings.log`, `runtime-bindings.exit`. |
| PASS, exit 0 | `python -B packaging/classic-workbench/tests/workspace_live_acceptance.py --runtime out/release/StarDataParadise-v0.3.1 --mysqld ../StarDataParadiseServer/out/ai-native-check/build18-runtime/mysqld.exe --output out/version-cleanup-20261122/live.json`: ayrı geçici Server/profil ile sekiz ekran, gerçek kataloglar, Knowledge diyagramı/timeline, action önizleme, ayrı AI SQL ve TLS; `live.json`, `live.log`, `live.exit`. Kullanıcı verisi kullanılmadı. |
| PASS | Güncel EXE sağlaması değişmedi; silinen dosya listesi ve sağlamaları `cleanup-plan.json`/`cleanup-result.json`, kısayol hedefi `shortcut-installed.json`. |
| NOT_RUN | Ürün kodu değişmediği için yeniden derleme yapılmadı. Gerçek OpenAI/Claude inference, normal GUI kapanışı ve Windows logoff kabulü bu temizlikte yeniden sınanmadı. |

Canlı testin CLI kapanışı önceki kabulde olduğu gibi açık kaldı; tam ve başarılı
JSON alındıktan sonra harness yalnız kendi Workbench PID'sini kapattı.
`live.lifecycle.json` bunu `forced_owned_cleanup=true` ve
`normal_gui_shutdown=false` olarak ayrı kaydeder. Geçici Server temiz kapandı.
Eksik çalışma zamanı/derleme önkoşulu görülmedi.

İlk salt okunur PowerShell envanter komutunda `foreach` çıktısının doğrudan
pipe edilmesi parse hatası verdi; diziye alınarak yeniden çalıştırıldı.
`Get-CimInstance Win32_Process` sandbox erişim reddi verdi; çalışan Workbench
kontrolü `Get-Process` ile yapıldı. Bunlar ürün veya kabul testi hatası değildir.
Uzak yayın, etiket değişikliği, servis veya worker yapılandırması yapılmadı.
