Store - an addon for the Sonix Player (TempoTec V3 Analog)

INSTALL
  These files belong in <card>/Addons/addonstore/. The tile appears in More.

WHAT IT IS
  A list of the addons on this card and of what the repositories publish, with
  one tap between them: install, update, delete, and read the addon's README.
  Rows are grouped by the player page the addon belongs to (Music, Streaming,
  Wireless, More); a page with nothing on it is absent.

  Above the rows:
    Search              narrows the list by name or id, either case
    Update all (n)      every addon with a newer release, in one batch
    Repositories        the repositories to read, added and removed here
    Check for updates   asks again without waiting for the next open

  Updates are checked against the release's own files, not against a version
  string typed into a manifest. For an addon the store installed, the published
  file's size and sha256 against what was recorded; for one copied by hand, the
  release's addon.so against the file on the card. If they match, the row says
  "up to date".

  An update replaces only addon.so, icon.png, README.txt and assets/; any other
  file is written only when the card does not have it, so addon.json, settings,
  tokens, saves and caches stay as they were. The version shown is the one the
  release carried: addon.json on the card is never rewritten, so its own string
  may be older. "Update all" follows the same rules and updates the store itself
  last.

REPOSITORIES
  <card>/Addons/addonstore/repos.txt holds one "owner/name" per line; the first
  start writes it with the default repository. The Repositories page edits the
  same file from the device: type owner/name (a pasted github.com URL works too)
  and the store starts reading it at once. Where the repository publishes an
  index.json catalogue, the whole list arrives in one request and no archive is
  downloaded; that release also carries the archives themselves, so a list comes
  back even when the catalogue file is gone. Otherwise the newest hundred
  releases are read and the newest one that carries each addon is that addon's.

UPDATING ITSELF
  When the repository publishes addonstore.zip, the store appears in its own
  list. Update replaces only the code and keeps addon.json, repos.txt, the
  records and the cache; the new version starts when the tile is opened again.
  The store cannot delete itself.

AFTER AN INSTALL
  A newly installed addon gets its tile when the player next reads the card:
  take the card out and put it back (or restart). The store says so as well.
  A delete is the same from the other side: the tile goes at the next read.

NETWORK
  The catalogue first: <repository>/releases/download/latest/index.json is one
  request and no zip downloads. That release also carries a copy of every current
  archive, so when the file itself cannot be read the same list is answered from
  its assets in one call. A repository with neither is read through the GitHub
  API, which allows sixty anonymous requests an hour per address; that list is
  cached and refreshed at most once per open (or on "Check for updates").

FILES
  installed.tsv    which release each installed addon came from
  repos.txt        the repositories to read
  cache/           the catalogue, downloaded zips, READMEs
  cache/local.tsv  the sha256 of each installed library, so a list does not
                   re-read the card

Store - аддон для Sonix Player (TempoTec V3 Analog)

УСТАНОВКА
  Эти файлы кладутся в <card>/Addons/addonstore/. Плитка появляется в More.

ЧТО ЭТО
  Список аддонов на карте и того, что публикуют репозитории, с одним касанием
  между ними: установить, обновить, удалить и прочитать README аддона. Строки
  сгруппированы по странице плеера (Music, Streaming, Wireless, More); пустая
  страница не показывается.

  Над строками:
    Поиск               сужает список по имени или id, в любом регистре
    Обновить всё (n)    все аддоны с релизом новее, одной пачкой
    Репозитории         какие репозитории читать: здесь же добавляются и
                        удаляются
    Проверить обновления  спросить заново, не дожидаясь следующего открытия

  Обновление проверяется по файлам самого релиза, а не по строке версии,
  вписанной в манифест. Для аддона, поставленного магазином, — размер и sha256
  опубликованного файла против записи об установке; для скопированного
  вручную — addon.so из релиза против файла на карте. Если они совпадают,
  строка говорит «актуально».

  Обновление заменяет только addon.so, icon.png, README.txt и assets/; любой
  другой файл пишется, лишь когда его на карте нет, поэтому addon.json,
  настройки, токены, сохранения и кэш остаются как были. Версия, которую видно,
  — из релиза: addon.json на карте не перезаписывается, поэтому его собственная
  строка может быть старше. «Обновить всё» подчиняется тем же правилам и
  обновляет сам магазин последним.

РЕПОЗИТОРИИ
  <card>/Addons/addonstore/repos.txt хранит по одному "owner/name" в строке;
  при первом запуске файл создаётся с репозиторием по умолчанию. Страница
  «Репозитории» правит тот же файл с устройства: наберите owner/name
  (вставленный адрес github.com тоже понимается), и магазин сразу начнёт его
  читать. Если репозиторий публикует каталог index.json, весь список приходит
  одним запросом и ни один архив не скачивается; в том же релизе лежат и сами
  архивы, так что список придёт даже без файла каталога. Иначе читаются
  последние сто релизов, и для каждого аддона берётся самый новый из них.

ОБНОВЛЕНИЕ САМОГО МАГАЗИНА
  Когда в репозитории появится addonstore.zip, магазин появится в собственном
  списке. Обновление заменит только код, сохранив addon.json, repos.txt,
  записи и кэш; новая версия начнёт работать при следующем открытии плитки.
  Удалить себя магазин не может.

ПОСЛЕ УСТАНОВКИ
  У нового аддона плитка появляется, когда плеер снова прочитает карту:
  выньте карту и вставьте обратно (или перезапустите). Магазин об этом пишет.
  Удаление — то же с другой стороны: плитка исчезнет при следующем чтении.

СЕТЬ
  Сначала каталог: <репозиторий>/releases/download/latest/index.json — один
  запрос и ни одного скачанного zip. В том же релизе лежит копия каждого
  текущего архива, поэтому если сам файл прочитать не удалось, тот же список
  приходит одним запросом из его ассетов. Репозиторий без каталога читается
  через API GitHub, а он разрешает шестьдесят анонимных запросов в час на адрес;
  этот список кэшируется и обновляется не чаще раза за открытие (или по кнопке
  «Проверить обновления»).

ФАЙЛЫ
  installed.tsv    из какого релиза взят каждый установленный аддон
  repos.txt        какие репозитории читать
  cache/           каталог, скачанные zip и README
  cache/local.tsv  sha256 библиотеки каждого установленного аддона, чтобы
                   список не перечитывал карту
