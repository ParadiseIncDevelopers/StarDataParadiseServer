# Managed SDK contract tests

Run the Windows scripts from a PowerShell process with Git and the pinned .NET SDK on PATH. Java requires JDK 17, Gradle 9.3.1, and the Gson/JUnit files listed by the JDBC repository. The Framework feed contains the offline NuGet dependencies declared by the Framework project. Each build reads a Git archive of the specified repository's HEAD and creates its own source and package directories.

~~~powershell
./tests/sdk_contract/build_managed.ps1 -Sdk dotnet -Repository $dotnetRepo -Output $dotnetBuild -FrameworkFeed $frameworkDependencies

./tests/sdk_contract/build_managed.ps1 -Sdk java -Repository $javaRepo -Output $javaBuild -Gradle $gradle -JavaHome $jdk -JavaDependencies $javaDependencies
~~~

Set each build and consumer output to a new directory under `out/`. Python 3.12 runs the shared TLS fixture. The Server executable and ledger fixture executable come from the independent Server build. The OpenSSL executable generates isolated trust material. Use the pinned wire v1 contract archive.

~~~powershell
$fixture = @{
    Server = $serverExecutable
    OpenSsl = $opensslExecutable
    Contract = $wireArchive
    Python = $pythonExecutable
}

./tests/sdk_contract/run_managed.ps1 @fixture -Sdk dotnet -Mode current -Build $dotnetBuild -Output $dotnetResult -ExpiryHelper $ledgerFixture
./tests/sdk_contract/run_managed.ps1 @fixture -Sdk java -Mode current -Build $javaBuild -Output $javaResult -ExpiryHelper $ledgerFixture -JavaHome $jdk
~~~

The .NET runner builds separate .NET 10 and .NET Framework 4.8 consumer executables. Each restores the exact package version from the selected feed. The JDBC runner compiles the observed adapter against the installed JDBC and Gson JARs. The small source fragments extend the existing SDK smoke clients at fixed anchors in the clean source copy.

To exercise an earlier artifact, use `-Mode frozen -FrozenArtifact $frozen`. Pass a directory of NuGet packages for .NET and the release JAR for JDBC. Use a new output directory for each artifact source. Every invoked command records a log and exit code. The consumer output includes hashes of the exact tested artifacts.