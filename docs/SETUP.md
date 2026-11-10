# Setting up an addon on the card

What a released addon needs on the card, and how to sign a service in.
Every section is written twice: English first, then the same in Russian.

Что нужно выпущенному аддону на карте и как войти в сервис. Каждый раздел
написан дважды: сначала по-английски, потом то же по-русски.

This is the user's side. The interface, the card layout and the internals are in
[`ADDONS.md`](ADDONS.md); this file does not repeat them.

Это сторона пользователя. Интерфейс, раскладка карты и внутренности — в
[`ADDONS.md`](ADDONS.md); здесь они не повторяются.

## Contents / Содержание

1. [Installing / Установка](#1-installing--установка)
2. [Qobuz](#2-qobuz)
3. [Yandex Music](#3-yandex-music)
4. [Subsonic](#4-subsonic)
5. [Gearboy](#5-gearboy)
6. [Flappy Bird](#6-flappy-bird)
7. [Dino](#7-dino)
8. [Piped](#8-piped)
9. [What is on the card / Что лежит на карте](#9-what-is-on-the-card--что-лежит-на-карте)
10. [When it does not work / Если не работает](#10-when-it-does-not-work--если-не-работает)

## 1. Installing / Установка

**English.** Every addon is one zip in the repository's Releases, named after
its folder: `qobuz.zip`, `subsonic.zip`, `yandex.zip`, `gearboy.zip`,
`flappybird.zip`, `dino.zip`, `piped.zip`. The files inside sit at the root of
the archive — `addon.json`, `addon.so`, `icon.png`, `README.txt`, and `assets/`
where an addon has one.

1. Copy `<id>.zip` to a computer the card is readable from.
2. On the card, make the folder `<card>/Addons/<id>/` — the id is the archive's
   name: `Addons/qobuz`, `Addons/yandex`, and so on.
3. Unzip the archive **into that folder**, so that `addon.json` ends up at
   `<card>/Addons/<id>/addon.json`. Unzipping into `Addons/` itself, or letting
   the archiver create a second `<id>/` inside the folder, gives a card the
   player silently ignores: it looks for `Addons/<id>/addon.json` and nothing
   deeper. `README.txt` in the archive is the short instructions for this addon;
   it can stay in the folder, the player does not read it.
4. Eject the card properly, put it in the player. The tile appears in Streaming
   (services) or More (games) — `addon.json` decides which and in what order.
5. A card the player sees arrive while it runs is read again there and then, so
   a restart is normally not needed. Restart anyway if a tile is missing.

To update an addon, replace its folder's `addon.so` (and any file the new
version ships). Its own settings are read from the same folder and are not part
of the archive, so they survive. To remove an addon, delete its folder — the
player unloads the library when the card changes.

The card must be writable for a sign-in to survive a restart; on a read-only
card a service still works for the run and says so on its page.

**Русский.** Каждый аддон — один zip в Releases репозитория, названный по
папке: `qobuz.zip`, `subsonic.zip`, `yandex.zip`, `gearboy.zip`,
`flappybird.zip`, `dino.zip`, `piped.zip`. Файлы внутри лежат в корне архива:
`addon.json`, `addon.so`, `icon.png` и `assets/`, если он есть.

1. Скопировать `<id>.zip` на компьютер, с которого видна карта.
2. На карте создать папку `<card>/Addons/<id>/`, где id — имя архива:
   `Addons/qobuz`, `Addons/yandex` и так далее.
3. Распаковать архив **в эту папку**, чтобы `addon.json` оказался по пути
   `<card>/Addons/<id>/addon.json`. Если распаковать в сам `Addons/` или дать
   архиватору создать внутри папки ещё одну `<id>/`, плеер такой аддон молча
   пропустит: он ищет `Addons/<id>/addon.json` и глубже не смотрит.
   `README.txt` из архива — короткая инструкция к этому аддону; её можно
   оставить в папке, плеер её не читает.
4. Извлечь карту штатно, вставить в плеер. Плитка появится в Streaming (сервисы)
   или в More (игры) — это решает `addon.json`, он же задаёт порядок.
5. Карту, появление которой плеер увидел на ходу, он перечитывает сразу, так
   что перезагрузка обычно не нужна. Если плитки нет — перезагрузить.

Обновление: заменить в папке аддона `addon.so` (и остальные файлы, которые
несёт новая версия). Настройки аддона лежат в той же папке и в архив не входят,
поэтому они сохраняются. Удаление: удалить папку аддона — при смене карты плеер
выгружает библиотеку.

Карта должна быть доступна для записи, иначе вход в сервис не переживёт
перезапуск; на карте только для чтения сервис всё равно работает в этом сеансе
и пишет об этом на своей странице.

## 2. Qobuz

### English

**What you need:** a Qobuz account with a streaming subscription, the card, and
Wi-Fi on the player.

**The way in is a token, not a password.** Qobuz's own `/user/login` refuses the
application pair this addon has, so there is no password form and none should be
expected. The account's `user_auth_token` is what works:

1. On a computer, sign in at <https://play.qobuz.com>.
2. Open the browser's developer tools, the Network tab, and reload the page so
   the requests appear.
3. Click any request to Qobuz's API — `user/login` is the one, a `POST` under
   `play.qobuz.com/api.json/0.2/` — then Headers, Request Headers, and copy the
   value of `x-user-auth-token`. The same value also sits in Local Storage under
   `user_auth_token`; the header is the one that is right there on the request.
4. On the card, open `Addons/qobuz/token.txt`. The addon writes a commented
   template there on its first start; if the file is not there, create it.
5. Put the token in it — either after `user_auth_token =` on that line, or as
   the whole content of the file, on a line of its own. Spaces, quotes and the
   newline are dropped by the reader.
6. Open the addon. A token file the addon has not checked yet is checked as the
   page is built: the line under the account becomes "Checking the token with
   Qobuz…", and the account's name appears when Qobuz accepts it. **Sign in from
   the card** starts the same check on demand.

What Qobuz accepted is kept in `Addons/qobuz/session.ini`; that file, not the
token file, is what the next start reads. Signing out writes an empty session and
stops the token file from being read again on its own — coming back is the same
**Sign in from the card** row.

The application pair (`app_id`, `app_secret`) is derived by the addon from the
Qobuz web player and cached in `Addons/qobuz/settings.ini`; nothing has to be
typed. A derived pair goes stale and is taken again. If you have your own pair,
put `app_id`, `app_secret` and `by_user = 1` into that same file and it will be
used as it is and never refreshed.

Quality is the gear in the corner (only shown once signed in): MP3 320 kbps,
FLAC 16/44.1, FLAC 24/96, FLAC 24/192. The rest of the page: favourite tracks,
favourite albums, my playlists, new releases, and a search behind the magnifier.

When the token expires, take a new one the same way and tap **Sign in from the
card** again.

### Русский

**Что нужно:** аккаунт Qobuz с подпиской на стриминг, карта и Wi-Fi на плеере.

**Вход — по токену, а не по паролю.** Собственный `/user/login` Qobuz
отказывает той паре приложений, что есть у аддона, поэтому формы с паролем здесь
нет и не будет. Работает `user_auth_token` аккаунта:

1. На компьютере войти на <https://play.qobuz.com>.
2. Открыть инструменты разработчика браузера, вкладку Network, и перезагрузить
   страницу, чтобы запросы появились.
3. Нажать любой запрос к API Qobuz — нужен `user/login`, это `POST` по адресу
   `play.qobuz.com/api.json/0.2/` — затем Headers, Request Headers, и
   скопировать значение `x-user-auth-token`. (То же значение лежит и в Local
   Storage под именем `user_auth_token`; заголовок удобнее тем, что виден прямо
   на запросе.)
4. На карте открыть `Addons/qobuz/token.txt`. При первом запуске аддон сам
   кладёт туда файл-шаблон с комментариями; если файла нет — создать.
5. Вписать токен: либо после `user_auth_token =` в этой строке, либо как всё
   содержимое файла отдельной строкой. Пробелы, кавычки и перевод строки
   читатель отбрасывает.
6. Открыть аддон. Файл с токеном, который аддон ещё не проверял, проверяется
   при построении страницы: строка под аккаунтом станет «Checking the token with
   Qobuz…», а когда Qobuz примет токен — появится имя аккаунта.
   **Sign in from the card** запускает ту же проверку по требованию.

То, что Qobuz принял, сохраняется в `Addons/qobuz/session.ini`; именно этот файл,
а не файл с токеном, читает следующий запуск. Выход из аккаунта пишет пустую
сессию и запрещает читать файл с токеном сам по себе — вернуться можно той же
строкой **Sign in from the card**.

Пару приложений (`app_id`, `app_secret`) аддон получает из веб-плеера Qobuz и
хранит в `Addons/qobuz/settings.ini`; вводить ничего не нужно. Полученная пара
стареет и берётся заново. Если у вас есть своя пара, впишите в тот же файл
`app_id`, `app_secret` и `by_user = 1` — она будет использоваться как есть и
никогда не обновляться.

Качество — шестерёнка в углу (появляется после входа): MP3 320 kbps,
FLAC 16/44.1, FLAC 24/96, FLAC 24/192. Остальное на странице: любимые треки,
любимые альбомы, мои плейлисты, новинки и поиск за лупой.

Когда токен истечёт, взять новый тем же способом и снова нажать
**Sign in from the card**.

## 3. Yandex Music

### English

**What you need:** a Yandex account (Plus for lossless), the card and Wi-Fi.
Nothing has to be typed in.

**The keys are already there.** `Addons/yandex/settings.ini` in the release zip
ships filled in — `client_id`, `client_secret`, `sign_key`, `client_header` and
`quality` — so **Sign in with a code** works as it is. The four values are the
official client's own, and the addon only carries what is already public: the
`MarshalX/yandex-music-api` Python library publishes them, since it talks to the
same API:

- `client_id` and `client_secret` —
  [`yandex_music/_client/device_auth.py`](https://github.com/MarshalX/yandex-music-api/blob/main/yandex_music/_client/device_auth.py),
  the Android application's defaults (`_DEFAULT_CLIENT_ID`,
  `_DEFAULT_CLIENT_SECRET`);
- `sign_key` and `client_header` —
  [`yandex_music/utils/sign_request.py`](https://github.com/MarshalX/yandex-music-api/blob/main/yandex_music/utils/sign_request.py),
  `DESKTOP_SIGN_KEY` and `DESKTOP_CLIENT` (`DEFAULT_SIGN_KEY` there is the
  Android one and does not fit).

A card without that file is "no keys": the page says so and the service stays
off. That is the only thing that state means — the values are not asked of you.

Two rules, both of them the service's, not this addon's:

- `sign_key` and `client_header` are a pair from the same client. `get-file-info`
  is signed with the key and checked together with that header, so a key from one
  client beside another client's header is refused;
- `client_header` is the whole header line — name and value,
  `X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.95.0` — because it goes
  into the request as it stands. It carries the client's version, and the version
  ages: when playback starts being refused, bump it before suspecting the key. A
  newer release zip carries a newer one, but it also carries the whole file — if
  you have edited `settings.ini`, do not overwrite it when updating.

`client_id`/`client_secret` are only used for the sign-in and the refresh; they
play no part in playback.

**Signing in — a code, nothing to type into a file.**

1. Tap **Sign in with a code**. The player asks the service for a code and puts
   two things on the page: **Where** — the address, `ya.ru/device` — and
   **Code** — eight characters.
2. Open that address on a phone already signed in to the account, type the code,
   confirm.
3. The addon asks the service every five seconds and signs in by itself.
   **Waiting** counts down the code's lifetime ("waiting (…s left)") and stops on
   "the code has expired"; **Stop waiting** ends it earlier.

What this leaves on the card is a real session with a refresh token
(`Addons/yandex/yandex_session.ini`), which renews before it expires. The
account's name then appears on the page.

There is no token to copy for this way, and no token file is needed.

**A token file, only if the card already carries one.** An older card may have
`Addons/yandex/token.txt`: the account's token, one line, a whole
`Authorization: OAuth …` line as pasted from a browser is fine. **Sign in from
the card** imports it. Such a session has no refresh token, so it never renews;
the code above is the way that does.

**Quality** cycles through Lossless (the default), High and Normal — a ceiling,
not a promise. Without Plus the station still answers and everything comes back
AAC around 192 kbit/s; the page says so.

**What is on the page:** My Wave and Liked tracks are the two ways into the
music; the quality pills; the account half — **Keys**, **Token file**, **Code**,
**Where**, **Waiting** — and the actions under it. The full Yandex page —
playlist rows, album pages, track menus — is not ported to this addon yet, so
browsing is limited to those two actions.

### Русский

**Что нужно:** аккаунт Яндекса (Plus — для lossless), карта и Wi-Fi. Вписывать
ничего не нужно.

**Ключи уже лежат.** В релизном zip `Addons/yandex/settings.ini` идёт
заполненным — `client_id`, `client_secret`, `sign_key`, `client_header` и
`quality`, — поэтому **Sign in with a code** работает как есть. Четыре значения
принадлежат официальному клиенту, и аддон несёт только то, что и так
опубликовано: их публикует питоновская библиотека `MarshalX/yandex-music-api`,
работающая с тем же API:

- `client_id` и `client_secret` —
  [`yandex_music/_client/device_auth.py`](https://github.com/MarshalX/yandex-music-api/blob/main/yandex_music/_client/device_auth.py),
  значения Андроид-приложения по умолчанию (`_DEFAULT_CLIENT_ID`,
  `_DEFAULT_CLIENT_SECRET`);
- `sign_key` и `client_header` —
  [`yandex_music/utils/sign_request.py`](https://github.com/MarshalX/yandex-music-api/blob/main/yandex_music/utils/sign_request.py),
  `DESKTOP_SIGN_KEY` и `DESKTOP_CLIENT` (`DEFAULT_SIGN_KEY` там андроидный и не
  подходит).

Карта без этого файла — это «no keys»: страница так и говорит, сервис выключен.
Только это то состояние и означает — значения у вас никто не спрашивает.

Два правила, и оба — правила сервиса, а не аддона:

- `sign_key` и `client_header` — пара из одного клиента. Подпись
  `get-file-info` проверяется вместе с этим заголовком, поэтому ключ одного
  клиента рядом с заголовком другого будет отвергнут;
- `client_header` — это вся строка заголовка, с именем и значением:
  `X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.95.0`, потому что она
  подставляется в запрос как есть. В ней зашита версия клиента, и она стареет:
  если воспроизведение начали отклонять, сначала поднимайте версию, а не ищите
  новый ключ. В новом релизном zip строка новее, но в нём же лежит и весь файл —
  если правили `settings.ini`, не перезаписывайте его при обновлении.

`client_id`/`client_secret` нужны только для входа и обновления сессии, в
воспроизведении они не участвуют.

**Вход — по коду, в файл ничего вписывать не нужно.**

1. Нажать **Sign in with a code**. Плеер запрашивает код у сервиса и показывает
   на странице две вещи: **Where** — адрес `ya.ru/device` — и **Code** — восемь
   символов.
2. Открыть этот адрес на телефоне, где уже выполнен вход в аккаунт, ввести код,
   подтвердить.
3. Аддон спрашивает сервис каждые пять секунд и входит сам. **Waiting**
   отсчитывает время жизни кода («waiting (…s left)») и останавливается на
   «the code has expired»; **Stop waiting** прекращает раньше.

На карте остаётся настоящая сессия с refresh-токеном
(`Addons/yandex/yandex_session.ini`), которая обновляется до истечения. Имя
аккаунта появляется на странице.

Для этого способа никакой токен копировать не нужно, и файл с токеном не нужен.

**Файл с токеном — только если он уже лежит на карте.** На старой карте может
быть `Addons/yandex/token.txt`: токен аккаунта одной строкой, целая строка
`Authorization: OAuth …`, скопированная из браузера, тоже годится.
**Sign in from the card** его импортирует. У такой сессии нет refresh-токена,
она не обновляется; обновляется только сессия, полученная кодом.

**Качество** переключается между Lossless (по умолчанию), High и Normal — это
потолок, а не обещание. Без Plus станция всё равно отвечает, и всё приходит AAC
около 192 kbit/s; страница об этом пишет.

**Что есть на странице:** My Wave и Liked tracks — два пути в музыку; пилюли
качества; половина про аккаунт — **Keys**, **Token file**, **Code**, **Where**,
**Waiting** — и действия под ней. Полная страница Яндекса — строки плейлистов,
страницы альбомов, меню треков — в этот аддон ещё не перенесена, так что
просмотр ограничен этими двумя действиями.

## 4. Subsonic

### English

**What you need:** your own Subsonic-compatible server (Subsonic, Navidrome,
Airsonic, Gonic and the like) reachable from the player's Wi-Fi, and the
account's user name and password on it.

**Signing in.** Tap **Sign in** and answer three questions in order:

- **Server** — `192.168.1.10:4533`, `http://box:4533/` and
  `https://music.example.org` are all accepted: the addon builds the base URL
  from what is typed, keeping the scheme and dropping trailing slashes. A server
  behind a proxy that answers plain HTTP with a redirect is asked again over
  https by itself.
- **User name** — the Subsonic account's.
- **Password** — the account's password. It is not stored as typed: the addon
  sends `t = md5(password + salt)`, which is what Subsonic expects. Servers that
  refuse token authentication are retried with the password, and that case is
  logged.

**Several servers.** Every account is one file,
`<card>/Addons/subsonic/servers/<slug>.ini` (server, user, salt, token, password,
quality, label); `servers/active` names the one in use. The **Server** row opens
the list: switch, **Add server**, **Remove server**. Signing out of one leaves the
others alone. The first start of this version renames an older `session.ini` to
`servers/default.ini`, so nothing is typed twice.

**Quality** is the **Quality** row on the home page: Original (the file as it is
on the server), MP3 320, MP3 192, Opus 128.

**Browsing:** Artists, Newest, Random, Playlists, and Search. Lists stop at 100
entries and are not paged. Cover art is not drawn in the list rows (the sheet's
cover comes from the cached file), there is no scrobbling from these pages, and
"Add to playlist" is not offered.

**Playback** goes through the player's own queue: a tap on a track starts the
whole visible list, so shuffle, repeat, the queue page, Play next and Add to
queue all behave as they do for files on the card. Each track is downloaded into
`<card>/Addons/subsonic/cache/` while it plays, which is why the decoder opens it
before the last byte has arrived.

### Русский

**Что нужно:** свой Subsonic-совместимый сервер (Subsonic, Navidrome, Airsonic,
Gonic и подобные), доступный из Wi-Fi плеера, и имя пользователя с паролем на
нём.

**Вход.** Нажать **Sign in** и ответить на три вопроса по порядку:

- **Server** — `192.168.1.10:4533`, `http://box:4533/` и
  `https://music.example.org` — всё принимается: аддон собирает базовый URL из
  введённого, сохраняя схему и убирая завершающие слэши. Сервер за прокси,
  который отвечает по обычному HTTP редиректом, переспрашивается по https сам.
- **User name** — имя пользователя Subsonic.
- **Password** — пароль этого аккаунта. Он не хранится как введён: аддон
  отправляет `t = md5(password + salt)`, как и ожидает Subsonic. Серверы, не
  принимающие аутентификацию по токену, переспрашиваются с паролем, это видно в
  логе.

**Несколько серверов.** Каждый аккаунт — один файл
`<card>/Addons/subsonic/servers/<slug>.ini` (server, user, salt, token, password,
quality, label); `servers/active` указывает используемый. Строка **Server**
открывает список: переключить, **Add server**, **Remove server**. Выход из
одного не трогает остальные. При первом запуске этой версии старый
`session.ini` переименовывается в `servers/default.ini`, так что вводить
заново ничего не нужно.

**Качество** — строка **Quality** на главной: Original (файл как он есть на
сервере), MP3 320, MP3 192, Opus 128.

**Просмотр:** Artists, Newest, Random, Playlists и Search. Списки
останавливаются на 100 записях и не листаются по страницам. Обложки в строках
списка не рисуются (обложка на карточке альбома берётся из кэшированного
файла), скробблинга с этих страниц нет, «Add to playlist» не предлагается.

**Воспроизведение** идёт через очередь плеера: нажатие на трек запускает весь
видимый список, поэтому shuffle, repeat, страница очереди, Play next и Add to
queue ведут себя как для файлов на карте. Каждый трек скачивается в
`<card>/Addons/subsonic/cache/` по ходу воспроизведения — поэтому декодер
открывает его раньше, чем пришёл последний байт.

## 5. Gearboy

### English

**What you need:** your own ROMs, and nothing else.

- `Games/GB` — `.gb` ROMs;
- `Games/GBC` — `.gbc` ROMs. The folder decides, not only the extension: a ROM
  in `Games/GBC` is treated as a Color one;
- `Games/Saves` — battery-backed cartridge RAM (`.sav`) and save states
  (`.state1`), written by the emulator beside the ROMs rather than inside their
  folders, so they can be copied off in one go;
- `Games/Bios` — optional boot ROMs, `dmg_boot.bin` and `cgb_boot.bin`.

The addon's own settings (`<card>/Addons/gearboy/settings.ini`): palette, shader,
colour correction and the boot ROM switch. They are edited from the emulator's
settings screen; hand-editing the file is possible but not needed. A card taken to
another player takes its palette with it.

The game list is rebuilt every time it is opened, so ROMs copied onto the card
appear without a restart. While a game runs the addon takes the touch panel over
for multitouch: a tap is a button, several fingers at once are several buttons.

### Русский

**Что нужно:** свои ROM'ы, больше ничего.

- `Games/GB` — ROM'ы `.gb`;
- `Games/GBC` — ROM'ы `.gbc`. Решает папка, а не только расширение: ROM в
  `Games/GBC` считается Color;
- `Games/Saves` — RAM картриджа с батарейкой (`.sav`) и сохранения-состояния
  (`.state1`), эмулятор пишет их рядом с ROM'ами, а не внутри их папок, чтобы
  всё копировалось за один раз;
- `Games/Bios` — необязательные boot ROM'ы, `dmg_boot.bin` и `cgb_boot.bin`.

Свои настройки аддона (`<card>/Addons/gearboy/settings.ini`): палитра, шейдер,
коррекция цвета и переключатель boot ROM. Их меняют на экране настроек
эмулятора; править файл руками можно, но не нужно. Карта, перенесённая в другой
плеер, забирает палитру с собой.

Список игр пересобирается при каждом открытии, поэтому ROM'ы, скопированные на
карту, видны без перезапуска. Пока игра идёт, аддон забирает тач-панель под
мультитач: касание — кнопка, несколько пальцев сразу — несколько кнопок.

## 6. Flappy Bird

### English

Nothing to set up: it needs no card files, no network and no account. The tile is
in More. The best score and the sound switch are kept in
`<card>/Addons/flappybird/state.ini`; the artwork travels inside the archive
(`assets/`), which is why that folder has to be copied along with the `addon.so`.

### Русский

Настраивать нечего: ни файлов на карте, ни сети, ни аккаунта. Плитка — в More.
Лучший результат и переключатель звука хранятся в
`<card>/Addons/flappybird/state.ini`; картинки и звуки едут внутри архива
(`assets/`), поэтому эту папку нужно копировать вместе с `addon.so`.

## 7. Dino

### English

Nothing to set up: a tap to jump, a finger held down to duck. The best score is
kept in `<card>/Addons/dino/settings.ini`. A run leaves the music playing — the
game does not touch the audio.

### Русский

Настраивать нечего: касание — прыжок, удержание пальца — присесть. Лучший
результат хранится в `<card>/Addons/dino/settings.ini`. Игра не трогает звук:
музыка продолжает играть.

## 8. Piped

### English

**What you need:** nothing from Google, but one public relay. The player cannot
reach YouTube itself — measured: the connection opens and then nothing comes
back — so the addon goes through a **Piped instance**, which fetches from YouTube
on its own address and passes the audio on. Two are built in.

**Using it.** Tap the Piped tile; **Search** opens a keyboard; type a track or
an artist; tap a result. The track is downloaded to
`<card>/Addons/piped/cache/` and then plays like any file: queue, shuffle,
seek, the star, the sleep timer. A four-minute video is about 12 MB on the card,
because the only format the instances offer today is the muxed MP4 that carries
the video as well. There is no video, no playlist and no channel: the instances
cannot list the contents of either.

**Choosing an instance.** The **Instance** row lists the built-in relays and
marks the one in use. **Automatic** takes the first that answers, **Add your
own...** accepts any Piped API address, **Test connection** says what the current
one replies. The one maintained list of public instances is in the Piped
project's documentation — `github.com/TeamPiped/documentation`,
`content/docs/public-instances/index.md`. What goes in is an instance's **API**
address, whose host usually starts with `pipedapi.` or `api.piped.`; most hosts
on that list are dead, answer 502, or say "Piped has shutdown", so check each one
with **Test connection** instead of trusting it. LibreTube keeps no public list
of its own any more: its instance dialog shows only what the user has added.

**When it does not work.** Search works and tracks do not start: YouTube has most
likely blocked the relay's own address for a while — the player's log names it
`SignInConfirmNotBotException`. That is not something this side can fix; another
instance is the remedy until it passes.

### Русский

**Что нужно:** ничего от Google, но один публичный посредник. Плеер до самого
YouTube не достаёт — проверено: соединение открывается, ответа нет — поэтому
аддон работает через **инстанс Piped**, который обращается к YouTube со своего
адреса и отдаёт звук. Два встроены.

**Как пользоваться.** Нажать плитку Piped; **Search** открывает клавиатуру;
ввести трек или исполнителя; нажать результат. Трек скачивается в
`<card>/Addons/piped/cache/` и дальше играет как обычный файл: очередь,
случайный порядок, перемотка, звезда, спящий таймер. Четыре минуты — около
12 МБ на карте, потому что инстансы сегодня отдают только склеенный MP4, в
котором есть и видео. Видео, плейлистов и каналов нет: содержимое плейлиста и
канала инстансы не отдают.

**Выбор инстанса.** Строка **Instance** показывает встроенные посредники и
отмечает используемый. **Automatic** берёт первый ответивший, **Add your own...**
принимает любой адрес Piped API, **Test connection** показывает, что отвечает
текущий. Единственный поддерживаемый список публичных инстансов — в
документации проекта Piped: `github.com/TeamPiped/documentation`,
`content/docs/public-instances/index.md`. Вписывается **адрес API** инстанса,
его хост обычно начинается с `pipedapi.` или `api.piped.`; большинство хостов
из этого списка мертвы, отвечают 502 или "Piped has shutdown", поэтому каждый
проверяется через **Test connection**, а не на веру. LibreTube своего публичного
списка больше не держит: в его выборе инстанса только добавленные вручную.

**Если не работает.** Поиск работает, а треки не начинаются: скорее всего
YouTube временно заблокировал адрес самого посредника — в логе плеера это
`SignInConfirmNotBotException`. С этой стороны не лечится; до тех пор помогает
другой инстанс.

## 9. What is on the card / Что лежит на карте

Written by the addons themselves, never part of a release archive. Deleting any
of these is safe: the worst case is signing in (or choosing a palette) again.
`cache/` is what a factory reset empties; settings are not in it. (`README.txt`
in an addon's folder is the opposite: it arrived in the archive and is only a
copy of the instructions.)

Пишут сами аддоны, в архив релиза эти файлы не входят. Удалять их безопасно:
худшее — войти заново (или выбрать палитру). `cache/` — это то, что очищает
сброс к заводским; настройки в него не входят. (`README.txt` в папке аддона —
наоборот: он приехал в архиве и является всего лишь копией инструкции.)

| path | addon | what it holds |
|---|---|---|
| `Addons/qobuz/token.txt` | Qobuz | the pasted `user_auth_token`; a way in, not the session |
| `Addons/qobuz/settings.ini` | Qobuz | the derived (or your own) `app_id`/`app_secret` |
| `Addons/qobuz/session.ini` | Qobuz | the accepted account: token, name, `signed_out` |
| `Addons/qobuz/cache/` | Qobuz | downloaded tracks |
| `Addons/yandex/settings.ini` | Yandex Music | the four keys, `base_url`, `auth_url`, `quality` |
| `Addons/yandex/yandex_session.ini` | Yandex Music | `access_token`, `refresh_token`, `expires_at`, `uid`, `user` |
| `Addons/yandex/token.txt` | Yandex Music | a token from an older card, imported by "Sign in from the card"; never written by the code flow |
| `Addons/yandex/cache/`, `Addons/yandex/saved.tsv` | Yandex Music | downloads, and the index of what was saved |
| `Addons/subsonic/servers/<slug>.ini`, `servers/active` | Subsonic | one account per file, and which is in use |
| `Addons/subsonic/cache/` | Subsonic | downloaded tracks |
| `Addons/gearboy/settings.ini` | Gearboy | palette, shader, colour correction, boot ROM |
| `Addons/flappybird/state.ini` | Flappy Bird | best score, sound |
| `Addons/dino/settings.ini` | Dino | best score |

`Games/GB`, `Games/GBC`, `Games/Saves`, `Games/Bios` are the user's, not an
addon's: Gearboy reads them and writes only the saves.

`Games/GB`, `Games/GBC`, `Games/Saves`, `Games/Bios` — пользовательские, не
аддона: Gearboy их читает, а пишет только сохранения.

## 10. When it does not work / Если не работает

**No tile.** The path is wrong: the player wants `<card>/Addons/<id>/addon.json`,
one level, no second `<id>/` and no `Addons/Addons/`. Also check that the archive's
`addon.so` came along and that the player's build carries the addon layer at all
(API 1). / **Нет плитки.** Неверный путь: плееру нужен
`<card>/Addons/<id>/addon.json`, один уровень, без второй `<id>/` и без
`Addons/Addons/`. Проверить, что `addon.so` из архива на месте и что сборка
плеера вообще содержит слой аддонов (API 1).

**The page says "no application keys" / "no keys".** Qobuz: no pair yet, and no
network to take one from the web player. Yandex: fewer than four keys in
`settings.ini`. / **Страница пишет «no keys».** Qobuz: пары ещё нет и нет сети,
чтобы взять её из веб-плеера. Yandex: в `settings.ini` меньше четырёх ключей.

**Qobuz did not accept the token.** The token belongs to another account, has
expired, or the file has more than a token in it in a way the reader does not
strip. Take a fresh `user_auth_token` and repeat. / **Qobuz не принял токен.**
Токен от другого аккаунта, истёк, или в файле лишнее, чего читатель не
отбрасывает. Взять свежий `user_auth_token` и повторить.

**Yandex: playback refused, sign-in worked.** The `client_header` version has
aged, or `sign_key` and `client_header` come from different clients. / **Yandex:
вход прошёл, воспроизведение отклоняют.** Устарела версия в `client_header` или
`sign_key` и `client_header` — из разных клиентов.

**Subsonic: "the server refused the user name or password".** The user name or
password is wrong, the account may not stream, or the server is reachable but the
address is missing its path (`https://host/subsonic`). The addon's log line about
TLS says when it retried over https. / **Subsonic: «the server refused the user
name or password».** Неверное имя или пароль, аккаунту запрещён стриминг, или
адрес сервера без пути (`https://host/subsonic`). Строка лога про TLS говорит,
переспрашивал ли аддон по https.

**The session is not kept.** The card is read-only or full; the page says so. /
**Сессия не сохраняется.** Карта только для чтения или заполнена; страница об
этом пишет.
