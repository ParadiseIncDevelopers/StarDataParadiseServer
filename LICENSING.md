# Server licensing / Lisans kapsamı

The license follows the component and its provenance, not the executable name.
The root [LICENSE](LICENSE) establishes the following scopes.

| Component | Applicable terms |
| --- | --- |
| Independently authored server, storage, SQL, jobs, ML, backup and supporting code, except for the exclusions below | [MIT](licenses/MIT.txt) |
| Original wire contract definitions in `contracts/` and the original content of project contract archives | [MIT](licenses/MIT.txt) |
| `compatibility/upstream/`, including the MySQL source patch and native integration | GPLv2 and the applicable [MySQL upstream license and notices](compatibility/upstream/LICENSE.mysql-server.txt) |
| `compatibility/ai/native/` and `compatibility/ml/cpu_udf.cpp` | [GPL version 2](licenses/GPL-2.0-only.txt) for original integration contributions; retain applicable upstream and file-specific terms |
| Independently licensed dependencies and other third-party material | Their own licenses and notices |

Original StarDataParadise modifications in the designated GPL integration
scope are GPLv2. Existing file-specific licenses take precedence over a
general directory description for the material they identify.

For copyright-controlled StarDataParadise contributions in the identified GPL
scope, the [additional linking permission](licenses/StarDataParadise-GPL-Linking-Exception.txt)
expressly preserves linking with the separately licensed software covered by
the corresponding upstream release's permission. It grants rights only in
those original contributions and does not change third-party terms.

## MySQL source base

The repository carries patches and integration code, not a complete MySQL
source distribution. The pinned source base is
[mysql/mysql-server@561753af986c3c006c4b6c7caa8359748fb55b22](https://github.com/mysql/mysql-server/tree/561753af986c3c006c4b6c7caa8359748fb55b22)
(`mysql-26.10.0`). Its full [LICENSE](https://github.com/mysql/mysql-server/blob/561753af986c3c006c4b6c7caa8359748fb55b22/LICENSE)
is preserved verbatim in this repository.

That upstream document includes additional linking permissions and the
Connector/C exception within their stated scopes. This project does not
extend those permissions to unrelated code or replace them with MIT.
Patch files and their existing checksum manifests remain unchanged.

## Redistribution

MIT permits commercial use, modification and redistribution of the
MIT-covered code while retaining its copyright and permission notice.
For a covered derivative of MySQL, the applicable GPL terms govern the
combined work; an MIT license on separate original components does not
remove those obligations.

When distributing GPL-covered binaries, provide the required corresponding
source for the exact build, including modifications and required build
material, through a method permitted by the applicable GPL. Include the
upstream and third-party notices. A link to unmodified upstream source alone
does not satisfy obligations for a modified binary.

The older independent-runtime dependency list in
[packaging/THIRD-PARTY.md](packaging/THIRD-PARTY.md) supplements these notices.
It is not a replacement for the MySQL license. Each shipped dependency keeps
its own license. This scope document is not a complete binary compliance audit.

## Products and branding

The MIT grant applies to the identified code, including commercial reuse;
it does not grant access to separately offered hosted services, credentials,
support or separately licensed software. StarDataParadise names and marks
are not licensed as endorsements. Previously granted rights remain intact.

## Türkçe özet

Özgün Server ve wire sözleşmesi kodları MIT kapsamındadır. MySQL yamaları,
native AI entegrasyonu ve MySQL UDF bağlantısı ayrı GPLv2 kapsamındadır.
MySQL tabanlı bir ikiliyi dağıtırken o sürümün kaynak sağlama ve bildirim
yükümlülükleri devam eder. Ayrı ticari hizmetler için ayrıca yetki gerekir;
bu ayrım MIT kapsamındaki kodun ticari kullanım iznini daraltmaz.
