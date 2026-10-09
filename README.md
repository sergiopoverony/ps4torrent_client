# ps4torrent client

Экранное приложение (PKG) для PS4, которое показывает на ТВ список раздач демона **ps4torrentd** и управляет ими геймпадом. Само ничего не качает: это обычное userland-приложение, которое общается с демоном по HTTP.

Прошивка 13.02, GoldHEN, сборка на OpenOrbis.

## Как это работает

```
ps4torrentd (payload)  <--HTTP 127.0.0.1:8787-->  ps4torrent client (PKG, экран + геймпад)
```

1. Демон `ps4torrentd` отправляется на консоль как payload (BinLoader GoldHEN, порт 9090) и сам скачивает торренты.
2. Клиент раз в короткий интервал запрашивает `GET /status` и рисует таблицу.
3. Кнопки геймпада вызывают `/api/pause`, `/api/resume`, `/api/delete`.

Без запущенного демона клиент покажет `NO PAYLOAD`.

## Возможности

- Таблица раздач: название, размер, прогресс с тонкой полоской, ETA, статус, пиры.
- Шапка: `IP:8787 | HDD: … free | USB: … free | скорость | пиры | параллельных`.
- Состояния связи в шапке:
  - `IP:port` — всё в порядке;
  - `NO INTERNET` (жёлтым) — демон жив, но у консоли нет внешней сети;
  - `NO PAYLOAD` (красным) — демон не отвечает (с запасом 10 с, чтобы не мигать при смене сети).
- Предупреждение жёлтым цветом, если на диске заканчивается место.
- Интерфейс полностью на английском.

## Управление

| Кнопка | Действие |
|---|---|
| Вверх / Вниз | выбор раздачи |
| Cross | пауза / продолжить |
| Triangle | удалить (откроется диалог) |

В диалоге удаления: **Cross** — удалить, **Square** — удалить вместе с файлами, **Circle** — отмена.

## Структура репозитория

```
ps4tc_client/     исходники (C++)
  main.cpp        запуск, чтение конфига, главный цикл
  api.*           вызовы API демона
  httpc.*, net.*  HTTP-клиент поверх сокетов
  json.*          небольшой JSON-парсер
  ui.*, scene.*   отрисовка, шрифт, геймпад
  image.*         загрузка PNG (stb_image)
  connstate.h     логика состояний связи
assets/fonts/     DejaVuSansMono.ttf (лицензия рядом)
assets/images/    title.png — баннер в интерфейсе
sce_sys/icon0.png иконка приложения 512×512
gfx_src/          исходники графики (PSD)
```

Не входят в репозиторий (берутся из OpenOrbis): `sce_module/`, `sce_sys/about/`.

## Сборка

Нужен [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) и переменная `OO_PS4_TOOLCHAIN`.

Параметры в `Makefile`:

```make
TITLE      := ps4torrent client
TITLE_ID   := BREW00088
CONTENT_ID := IV0000-BREW00088_00-PS4TC00000000000   # ровно 36 символов
LIBS       := -lc -lkernel -lc++ -lSceNet -lSceVideoOut -lSceSysmodule -lSceFreeType -lScePad -lSceUserService
EXTRAFLAGS := -O2
```

Для `create-fself` в Makefile задан `--paid`; если dotnet новее нужного, поможет `DOTNET_ROLL_FORWARD=LatestMajor`. Затем `make` и установка получившегося `.pkg` на консоль.

## Настройка

Файл не обязателен. По умолчанию клиент подключается к `127.0.0.1:8787`. Чтобы изменить, создайте `/data/ps4tc_client/config.txt`:

```
host=127.0.0.1
port=8787
token=
```

`token` нужен, только если в демоне включён `web_token`. Лог клиента: `/data/ps4tc_client/log.txt`.

## Ограничения

- Клиент только показывает и управляет; добавлять торренты нужно через веб-интерфейс демона (`http://IP:8787/`).
- Установка PKG, работа с USB и сама загрузка торрентов в клиенте не реализованы — этим занимается демон.

## Лицензии

Шрифт DejaVu Sans Mono — см. `assets/fonts/LICENSE-DejaVu.txt`. PNG разбирается через stb_image (public domain).

License
Copyright (C) 2026 SergioPoverony and Mr.Claude.

Licensed under the GNU General Public License v3.0 (see LICENSE). Anyone may use, modify and redistribute it; the copyright notice must be kept, and modified versions must be released under the same license with source code.

Created by SergioPoverony and Mr.Claude.
