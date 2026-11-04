---
name: sonix-addon-port
description: "Use when moving an existing page or module of the Sonix Player (or of another sonix fork) into an addon on the card instead of compiling it into the binary: Gearboy and Flappy Bird are the worked examples. Covers the inventory (`tools/addon_port.py`), the src/ mirroring layout, the manifest key that keeps a tile in place, `init()` running exactly once, state that has to move to the card, what the player used to know about the module, and the host/device checks. Trigger keywords: перенести страницу в аддон, вынести игру в плагин, портировать клиент из форка, spotify addon, dino addon, addon.json order, addon.mk, init runs once, gearboy port, flappybird port."
---

# Перенос существующей страницы или модуля в аддон

Это продолжение скилла `sonix-addon` (как написать аддон с нуля) и `docs/ADDONS.md`
(справочник по интерфейсу). Здесь — то, что стоит времени при переносе **готового**: что
именно тянет за собой модуль, что плеер о нём знал и где это теперь спрашивать.

Рабочие примеры в дереве: `gearboy` (C++, три экрана, свои сейвы и настройки) и
`flappybird` (одна страница, свои ассеты, состояние на карте). Оба перенесены из
`src/` 2026-10-29.

## Шаг 0. Приложение или сервис

Сначала ответить, потом двигать файлы:

- **приложение** — страница со своим состоянием: экран, таймер, файлы на карте. Переносится
  как есть (Gearboy, Flappy).
- **сервис** — вплетён в чужие экраны и в менеджеры плеера: `player.c` (бейдж источника,
  избранное), `playlistpage.c` (синк плейлистов), `quickpanel.c` (звёздочка), `power.c`
  (не спать во время загрузки), `device_state.c` (USB), `factoryreset.c`/`powermenu.c`
  (`*_clear_on_exit`). Это не перенос, а решение: держать сервис в бинаре и вынести только
  страницы, или добавить в плеер маленький интерфейс (`docs/ADDONS.md`, «Porting a service
  page»). Tidal и Qobuz — ровно этот случай: 68 ссылок из `player.c`, 53 из `playlistpage.c`.

## Шаг 1. Инвентаризация одной командой

```sh
python3 tools/addon_port.py <id> \
    --src src/gui/<page>.c --src src/system/<module>.c \
    [--section more|streaming] [--order N] [--scaffold]
```

Печатает (и это ровно то, что нужно решить до `git mv`):

| раздел | как читать |
|---|---|
| Size | сколько кода и сколько из него C++. C++ работает, но `.so` повезёт свой libstdc++ |
| It drags these along | `#include` вне набора: что уже публикует плеер (заголовки shell/fonts/core/net/image/audio/library/playback/device и `lvgl/`) и что придётся тащить в аддон |
| Symbols | «in the published base» — ничего не делать; «outside that base» — экспортируется автоматически (`sonix-player/src/addon/exports.list`), нужен только `make && make target`; «defined nowhere in the player» — **блокер**, код надо собрать внутрь аддона |
| State it keeps | ключи `device_config.ini` → на карту; `streamkeys_*` → токен пользователя; абсолютные `/data`-пути |
| Audio | `radio_play` переносим; `audio_play(file)`/`play_file` — нет (нужен кэш и очередь); `audio_external_*` — игры, работает |
| Lifecycle | `switch_screen`, `SCREEN_LOADED`, `pthread_create`, и `*_clear_on_exit`/`deinit` — у аддона **нет deinit** |
| Who calls into it | число ссылок из остального дерева: < 60 — приложение, > 200 — сервис |
| Verdict | сводка причин |

Символьная часть читает `build_target/*.o` кросс-`nm`; без `make target` она честно
пропускается. `--scaffold` пишет `<id>/{addon.c,addon.mk,addon.json}` и печатает
`git mv` — файлы не двигает.

## Шаг 2. Раскладка: пути как в плеере

```sh
git mv src/gui/gearboy      gearboy/src/gui/gearboy
git mv src/system/gearboy   gearboy/src/system/gearboy
git mv src/gb               gearboy/src/gb
```

`addon.mk` даёт `-I$(CURDIR)/<id>`, и из этой точки дерево выглядит как дерево плеера:
`#include "src/system/gearboy/gbdb.h"` резолвится без единой правки в перенесённых файлах.
Короткие include внутри Gearboy (`"Video.h"`) — отдельные `-I` в том же `addon.mk`.

## Шаг 3. Манифест, тайл, иконка

- `order` = место, которое тайл занимал (`morepage.c`: dac 1, gearboy 2, books 3,
  file_explorer 4, flappy 5, metronome 6). Без него тайл уедет в конец страницы.
- Убрать: строку из `build_entries()` в `morepage.c`/`streaming.c`, `#include` там же,
  `lv_obj_create(NULL)` для экрана в `gui_init()`, вызов `<page>_init(cfg)` там же, имя
  экрана в массивах `player_swipe_screens`/`back_swipe_screens` (`gui.c`).
- Иконка: `cp assets/icons/<name>.png <id>/icon.png`; строки в `icons*.c` можно не
  трогать — они остаются мёртвыми ~4 КБ и `--gc-sections` их не убирает (внешняя линковка),
  но и никому не мешают. Перегенерировать наборы иконок ради этого не стоит.
- Ассеты (PNG/OGG игры): `<id>/assets/`, `make pack` копирует папку; путь даёт
  `host->dir`. Из `sonix-packer/assets/<board>/usr/resource/...` их тогда убрать — если копии
  по платам побайтово равны, останется одна в аддоне (так уехал Flappy).

## Шаг 4. `init()` — единственный вызов, и в нём две ловушки

1. **Состояние грузится на каждый вход, а не в init.** `init()` вызывается один раз; при
   повторном тапе `addon_open()` просто переключает уже существующий экран. Всё, что
   страница освобождает в `SCREEN_UNLOADED`, надо грузить в `SCREEN_LOADED`
   (`flappybird/.../flappybird.c`, `game_begin()` — иначе второй вход даёт пустой
   экран).
2. **`init()` не вызывает `switch_screen()` для своего экрана.** Переключает оболочка. Второй
   `switch_screen()` на уже активный экран — no-op внутри LVGL
   (`lv_display.c`, `lv_screen_load_anim` выходит раньше загрузки), поэтому всё, что страница
   делает из `SCREEN_LOADED` (спрятать статус-бар), происходит до него и им же отменяется:
   Flappy вернулся из переноса с полосой поверх игры.

## Шаг 5. Всё, что запоминается, — на карту

- Настройки: `common/store.h`, файл `<host->dir>/settings.ini` (`gbconf.h` в Gearboy,
  `fstate.h` в Flappy, `session.ini` в Subsonic). Никогда `config_set()`/`config_save()` —
  это файл плеера, перезаписывается целиком и остаётся на устройстве.
- Совместимость: ключа нет в файле → прочитать старую секцию `config_get_int("<модуль>", …)`
  один раз. Так палитра и рекорд пережили перенос.
- Сейвы и данные игры — на карту и **не** через store, если их читают другие инструменты:
  Gearboy пишет `Games/Saves` (`.sav`, `.state1`), BIOS `Games/Bios`, ромы `Games/GB`,
  `Games/GBC` — всё из `host->sd_root`.
- Поток, читающий настройки из UI-потока и пишущий их же из своего: мьютекс в обёртке
  (`gbconf.c`). Ключи, которые живут только в LVGL-таймерах, лока не требуют (`fstate.c`).

## Шаг 6. Что плеер знал о модуле

| было в бинаре | стало в аддоне |
|---|---|
| `gbdb_set_root()` в `main.c` и `system.c` | `gbdb_set_root(host->sd_root)` в `init()`; база ROM'ов пересканируется при открытии списка |
| `gearboy_running()` в `player.c` (sleep timer) и `power.c` | `audio_external_active()` — кто держит DAC, тот и хозяин |
| три экрана в `gui.c` + `switcher_attach_back_gesture` | создаются в `init()`; жест вешается там же (страница может вешать его сама — gearboypage так и делает) |
| иконки в `icons*.c` | `icon.png` |
| порядок тайлов | `order` в манифесте |
| `z`-порядок и `volume_overlay_allow_outside_player()` | остаётся кодом страницы, ничего не меняется |

Звук: `radio_play()` — для стримов; игры берут DAC через `audio_external_begin_latency()` /
`audio_external_write()` / `audio_external_end()`, и `audio_external_active()` сообщает об этом
sleep timer'у и менеджеру питания. `audio_stop()` перед этим — норма (игра не саундтрек).

`*_clear_on_exit()` и любые `deinit` — блокер: аддон не выгружается никогда. Работу переносить
в «после каждой записи» или в колбэк, который плеер уже умеет звать
(`audio_external_release_cb()`).

## Шаг 7. Сборка и проверки

```sh
make && make target && make checks && make pack
# только docker: docker/build.sh ; docker/build.sh make pack
```

- `exports.base` трогать не надо: `make` добавляет UND-символы аддонов в
  `--dynamic-list`, `make target` перелинковывает плеер. **Порядок обязателен.**
- `exports.base` устаревает незаметно (`theme_style_*`, `font_ui_*` в нём не было, хотя
  `theme.o`/`fonts.o` в его списке) — если отчёт показывает странные «не экспортируется»,
  сначала `make` (после `make target`), потом выводы.
- C++ в аддоне: `<id>/addon.mk` с
  `<id>_LD_target = $(CROSS_CXX) -static-libstdc++ -static-libgcc`. Цена — Gearboy: 2.5 MB
  `.so` (1.3 MB кода + libstdc++ целиком, `libstdc++.a` без `-ffunction-sections` тянет
  iostream/locale). Динамический вариант — 690 KB, но требует GLIBCXX_3.4.21 на устройстве.
- `make pack` делает `--strip-debug`: 16.9 MB в `build/target` против 2.5 MB в папке для карты.

## Шаг 8. Проверка — хост, потом устройство

Хост (дёшево, ловит почти всё):

```sh
make host && make host
mkdir -p /tmp/opencode/sd/Addons/<id> /tmp/opencode/simrun/usr
cp <id>/addon.json build/host/<id>.so /tmp/opencode/sd/Addons/<id>/   # addon.so!
ln -s "$PWD/../sonix-packer/assets/BLAZE/usr/resource" /tmp/opencode/simrun/usr/resource
cd /tmp/opencode/simrun && SONIX_SD_ROOT=/tmp/opencode/sd /path/sonix_player_host
```

Тапы — `printf 'x y' > /tmp/sonix/tap` **через rename**, скриншоты — `touch /tmp/sonix/shot`.
Проходить поимённо: тайл на месте, первый вход, выход, **повторный вход** (init once),
статус-бар, настройки после перезапуска, файлы на карте (`sd/Addons/<id>/*.ini`). Один
симулятор за раз — два пишут скриншоты в один каталог, и «новый» кадр может быть чужим
(и окно у второго бывает 240x360).

Устройство: см. скиллы `v3-firmware`/`v3-adb-flash`. Коротко: `SKIP_PLAYER_BUILD=1` после
копирования `sonix_player`/`sonix_launch` в `sonix-packer/`, `adb push` образа, `echo recovery >
/proc/jz/reset/reset`, `md5sum /usr/bin/sonix_player`, аддоны в `/mnt/sd_0/Addons/`, лог —
`/mnt/sd_0/.local/sonix_player.log`:

```
addon: gearboy  'Gearboy' (more)          нашёлся при скане
addon: gearboy opened                     dlopen + init + switch_screen
addon: dlopen ...: undefined symbol: X    плеер не экспортирует X (см. check-exports)
gearboy: settings /mnt/sd_0/Addons/gearboy/settings.ini (read|new)
```

## Клиент из чужого форка (Spotify, Yandex, Zvuk, «дино» из чрома)

- Ключи сервисов (`streamkeys_*`) вкомпилированы в плеер. Аддон, собранный **этим** деревом,
  их получит (`build_gen/streamkeys_key.h`); чужой аддон — нет. Значит: импорт токена
  пользователем, как у Subsonic.
- Их воспроизведение обычно «скачать в кэш и `audio_play(file)`» — здесь такого пути нет:
  либо `radio_play()` (стрим), либо новая публичная функция в плеере.
- Чужая ветка тянет свои `svcui`/`svclist`/`streamerr` — их либо тащить в аддон, либо менять на
  `settingsrow_*` этого дерева.
- Дата: такой форк стоит прогнать через `tools/addon_port.py` **в его собственном клоне**
  (`--tree /path/to/clone`), чтобы увидеть связи до переноса файлов.

## Грабли, уже стоившие времени

- `CROSS_CXX` не был объявлен в `Makefile`: правило `.opp` разворачивалось в пустой
  компилятор, и make печатал `make: O2: No such file or directory` — похоже на проблему shell.
- `init()` менял экран (см. шаг 4) — статус-бар поверх Flappy.
- Состояние грузилось только в `init()` — второй вход давал пустую игру.
- Настройки писались в `device_config.ini` — переезд на `store` потребовал однократного чтения
  старой секции, иначе палитра и рекорд терялись.
- Пак без `strip`: 16.9 MB вместо 2.5 MB в папке, которую копируют на карту.
- Исходники под `src/` нельзя (`APP_SRCS := find src -name '*.c'`) — они попадут в бинарь.
