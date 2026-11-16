Store - an addon for the Sonix Player (TempoTec V3 Analog)

INSTALL
  These files belong in <card>/Addons/addonstore/. The tile appears in More.

WHAT IT IS
  A list of the addons on this card and of what one GitHub repository
  publishes, with one tap between them: install, update, delete, and read the
  addon's README. Rows are grouped by the player page the addon belongs to
  (Music, Streaming, Wireless, More); a page with nothing on it is absent.

  Updates are checked against the release's own files, not against a version
  string typed into a manifest. For an addon the store installed, the published
  file's size and sha256 against what was recorded; for one copied by hand, the
  release's addon.so against the file on the card. If they match, the row says
  "up to date".

  An update replaces only addon.so, icon.png, README.txt and assets/; any other
  file is written only when the card does not have it, so addon.json, settings,
  tokens, saves and caches stay as they were.

REPOSITORIES
  <card>/Addons/addonstore/repos.txt holds one "owner/name" per line; the first
  start writes it with the default repository. Edit it on a computer, then open
  the addon again. Each repository is read at its latest published release, and
  every .zip asset in it is offered as an addon.

UPDATING ITSELF
  When the repository publishes addonstore.zip, the store appears in its own
  list. Update replaces only the code and keeps addon.json, repos.txt and the
  records; the new version starts when the tile is opened again. The store
  cannot delete itself.

AFTER AN INSTALL
  A newly installed addon gets its tile when the player next reads the card:
  take the card out and put it back (or restart). The store says so as well.
  A delete is the same from the other side: the tile goes at the next read.

NETWORK
  GitHub answers sixty anonymous requests an hour per address, so the list is
  cached and refreshed at most once per open (or on "Check for updates").

FILES
  installed.tsv    which release each installed addon came from
  repos.txt        the repositories to read
  cache/           the release list, downloaded zips and READMEs

Store - аддон для Sonix Player (TempoTec V3 Analog)

УСТАНОВКА
  Эти файлы кладутся в <card>/Addons/addonstore/. Плитка появляется в More.

ЧТО ЭТО
  Список аддонов на карте и того, что публикует репозиторий на GitHub, с одним
  касанием между ними: установить, обновить, удалить и прочитать README аддона.
  Строки сгруппированы по странице плеера (Music, Streaming, Wireless, More);
  пустая страница не показывается.

  Обновление проверяется по файлам самого релиза, а не по строке версии,
  вписанной в манифест. Для аддона, поставленного магазином, — размер и sha256
  опубликованного файла против записи об установке; для скопированного вручную —
  addon.so из релиза против файла на карте. Если они совпадают, строка говорит
  «актуально».

  Обновление заменяет только addon.so, icon.png, README.txt и assets/; любой
  другой файл пишется, лишь когда его на карте нет, поэтому addon.json,
  настройки, токены, сохранения и кэш остаются как были.

РЕПОЗИТОРИИ
  <card>/Addons/addonstore/repos.txt хранит по одному "owner/name" в строке;
  при первом запуске файл создаётся с репозиторием по умолчанию. Правьте его
  на компьютере и откройте аддон заново. Из каждого репозитория берётся
  последний релиз, и каждый .zip в нём предлагается как аддон.

ОБНОВЛЕНИЕ САМОГО МАГАЗИНА
  Когда в репозитории появится addonstore.zip, магазин появится в собственном
  списке. Обновление заменит только код, сохранив addon.json, repos.txt и
  записи; новая версия начнёт работать при следующем открытии плитки. Удалить
  себя магазин не может.

ПОСЛЕ УСТАНОВКИ
  У нового аддона плитка появляется, когда плеер снова прочитает карту:
  выньте карту и вставьте обратно (или перезапустите). Магазин об этом пишет.
  Удаление — то же с другой стороны: плитка исчезнет при следующем чтении.

СЕТЬ
  GitHub отвечает на шестьдесят анонимных запросов в час на адрес, поэтому
  список кэшируется и обновляется не чаще раза за открытие (или по кнопке
  "Проверить обновления").

ФАЙЛЫ
  installed.tsv    из какого релиза взят каждый установленный аддон
  repos.txt        какие репозитории читать
  cache/           список релизов, скачанные zip и README
