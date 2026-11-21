# Kararlı yerel AI worker yayını: Server 0.5.1 / Workbench 0.3.1

Kullanıcı Windows oturum açılışında otomatik worker başlangıcını açıkça onayladı;
son değişikliklerin commit/push edilmesini, preview olmayan release ve güncel
Workbench EXE teslimini istedi. Önceki veri yedekleri ve kullanıcı profili korunur.

## Son davranış

- Mevcut kullanıcının Startup klasöründe StarDataParadise AI Worker kısayolu
  doğrulandı. Hedef artık `StarDataParadiseAiWorker.exe --deployment <kurulum>`.
  Konsol açmayan WinExe, worker'ı aynı Windows kimliği ve şifreli SQL kasasıyla
  başlatır. Başka hedefte aynı adlı kısayol otomatik sahiplenilmez.
- İlk .ps1 başlatıcı Windows'un imza gerektiren execution policy'si nedeniyle
  açılmadı. Politika değiştirilmedi veya bypass edilmedi. Yerine doğrudan normal
  Windows EXE başlatıcısı kullanıldı. Workbench manuel düğmesi de bu EXE'ye
  geçirildi; artık PowerShell script çalıştırmasına bağlı değildir.
- Yeni script girişinde yerel `secrets.py` dosyasının Python stdlib secrets
  modülünü gölgelemesi bulundu. Girişin modül dizini sys.path'ten çıkarılarak
  doğru package kökü eklendi. Gerçek kısayol → worker → verified TLS → ready
  kabulü geçti. Windows oturumu gerçekten kapatılmadı; test kayıtlı Startup
  kısayolunun doğrudan çağrılmasıyla yapıldı.
- Tek worker kilidi, yeniden bağlantı, OpenAI/Claude kasa bağları ve GGUF
  importu korunur. Startup'tan açılan worker gerçek GGUF import/yükleme ve SQL
  konuşması yanıtını tekrar tamamladı. Mevcut üç tablonun checksum'ları değişmedi.
- Workbench ProductVersion 0.3.1; Classic dosya/assembly tabanı 8.0.47. Server
  ürün sürümü 0.5.1; native mysqld uyumluluk tabanı 26.10.0-StarDataParadise.
  Native engine byte'ları önceki kabul edilmiş build18 ile aynı; bu sürüm worker,
  başlatıcı, katalog/UI ve sürüm metadatasını tamamlar. SDK wire sözleşmesi ve
  SDK sürümleri değişmez. HIST/Git ve public SQL JOB ertelenmiş kalır.

## Yayın ve paket doğrulaması

- PASS: Release_OSS wbprivate.be.dll ve yeni StarDataParadise.exe kaynaklardan
  ayrı runtime'a derlendi. EXE FileVersion 8.0.47, ProductVersion 0.3.1.
  Yalnız mevcut ATL debug PDB LNK4099 uyarıları var.
- PASS: Server wrapper Release build ve help/version/invalid_option üç CTest.
- PASS: 60 Server unit + 28 Workbench workspace + 7 yerel worker unit testi.
- PASS: Nihai Server ZIP'inin CRC ve tüm payload hash'leri; paketten ayrı boş
  test datadir initialize, TLS, bakım katalog kurulumu, temiz yeniden başlatma,
  gerçek AI capability'leri, typed Knowledge ve short SQL execution.
- PASS: Paketlenmiş Workbench EXE'siyle ayrı fixture Server üzerinde sekiz ekran,
  beş katalog, roller, Knowledge diyagramı, action önizleme ve ayrı AI SQL akışı.
- Geniş CTest ilk turu yalnız wrapper hedefi derlendiği için eksik smoke EXE'leri
  bildirdi; history_journal_storage da aynı eksik runner nedeniyle WinError 2
  verdi. Eksik hedeflerin tamamı derlendi; yeniden koşulan 22/22 test geçti.
  İlk turdaki 28 başarılı sonuçla birlikte 50 farklı CTest doğrulandı. Nihai
  sonuç ve exit kapıları yerel release kaydında tutulur.
- NOT_RUN: Gerçek OpenAI/Claude inference, kullanıcı API anahtarı sağlanmadığı
  için yapılmadı. Adapter, Windows kasa ve owner/provider/endpoint sınırları
  testleri bu eksik dış sağlayıcı kabulünün yerine gösterilmez.

Portable paketler yerel ayar, parola, bağlantı profili, kullanıcı veritabanı,
GGUF ağırlıkları, cache ve build debug dosyalarını içermez. Yeni worker launcher
binary'si ve kaynak/kurulum yardımcıları Server paketindedir. Workbench EXE tek
başına taşınmaz; bütün doğrulanmış bağımlılıklarıyla ayrı v0.3.1 klasörüne açılır.

Yayın hedefleri `v0.3.1` ve `v0.5.1` stable/latest'tır. Eski etiketler/asset'ler
değiştirilmez. Yayın betiği temiz HEAD, pushed annotated tag ve exit 0 test
kapıları ister; asset'leri önce draft'a yükler, remote SHA256 eşleştikten sonra
draft=false/prerelease=false yayınlar. Son commit/tag/remote asset kanıtları
ignore edilen `out/release-0.5.1/` altında korunur.
