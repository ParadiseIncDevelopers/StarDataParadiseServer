# StarDataParadise çalışma alanı başlangıcı

Windows x64, MSBuild 18.11.1.51303, MSVC 19.51.36260 (v145) ve Windows SDK 10.0.26100.0 ile incelendi. Dört mevcut Git deposu başlangıçta `master` dalında temizdi. Staged, unstaged ve ignore edilmeyen untracked dosya sayısı her depoda 0 idi.

| Plan adı | Yerel klasör | Başlangıç HEAD | Doğrulanan kopyalar | Başlangıç sonucu |
| --- | --- | --- | ---: | --- |
| StarDataParadise | StarDataParadise | 769721228600dd21429cfb2ec41fa5d9b119641d | 14 | Win32 şablonu derlendi. |
| StarDataParadiseServer | StarDataParadiseServer | bec2e0c4ada12d1db49a73f7e1229a9ce08e4d26 | 6 | MSBuild 0 döndü. Proje kaynak dosyası içermiyor ve EXE kontrolü 1 döndü. |
| StarDataParadiseCLI | StarDataParadiseCli | 792a803f6521c749d1b668a80fde81062cff1afb | 7 | EXE derlendi ve şablon mesajıyla 0 döndü. |
| StarDataParadiseDLLler | StarDataParadiseDLLler | c1bb628f62036bd418bbe8905e4b30ed48200ad0 | 10 | DLL derlendi ve yükleme/serbest bırakma testi geçti. |
| StarDataParadise.SDK.NET | MISSING | — | — | SDK klasörü eksik. |
| StarDataParadise.SDK.Java | MISSING | — | — | SDK klasörü eksik. |
| StarDataParadise.SDK.Python | MISSING | — | — | SDK klasörü eksik. |

Mevcut CLI klasörünün ve Visual Studio dosyalarının yazımı `StarDataParadiseCli`. Plan adı `StarDataParadiseCLI` aynı Windows klasörüne karşılık geliyor.

Her mevcut depoda bir `.slnx` ve bir `.vcxproj` vardı. CMake veya otomatik test tanımı yoktu. Aşağıdaki başlangıç derlemesi her deponun kökünde o depoya ait çözümle çalıştırıldı.

```powershell
MSBuild.exe <depo>.slnx /nologo /t:Build /verbosity:minimal /p:Configuration=Release /p:Platform=x64 /p:IntDir=out/sdp-m1/baseline/build/obj/ /p:OutDir=out/sdp-m1/baseline/build/bin/
```

Her deponun ignore edilen `out/sdp-m1/baseline/` dizinindeki `checkpoint.json` ve `checkpoint/` izlenen kaynakları, iki ikon dosyası dahil binary girdileri ve mevcut `.vcxproj.user` dosyasını içerir. 37 kopyanın SHA-256 değerleri kaynak dosyalarıyla karşılaştırıldı. Üretilmiş derleme çıktıları ve Visual Studio önbellekleri kaynak kopyasına dahil edilmedi.

Gerçek komutlar, çıkış kodları ve log SHA-256 değerleri her depodaki `out/sdp-m1/baseline/commands.json` dosyasında. Yerel mutlak yollar ve bulunan/eksik durumları Server deposundaki `out/sdp-m1/baseline/workspace-map.json` dosyasında. Ayrıntılı derleyici envanteri aynı dizindeki `toolchain.json` dosyasında.
