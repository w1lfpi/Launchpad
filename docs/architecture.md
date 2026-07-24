# Архитектура Windows Launchpad

## Цель

Нативный полноэкранный лаунчер приложений для Windows 10/11, визуально и
поведенчески близкий к классическому Launchpad в macOS: сетка крупных иконок,
поиск, страницы, запуск приложений, плавное появление и матовый фон, через
который читаются персональные обои пользователя.

## Выбранный стек

Для проекта выбран Win32 + Direct2D + DirectWrite + Windows Imaging Component.

- Direct2D аппаратно ускоряет сетку, фон и анимации.
- DirectWrite даёт качественный текст при любом DPI.
- WIC и `IShellItemImageFactory` используются для получения и декодирования
  иконок Windows-приложений.
- Управляемый каталог `Applications/` является единственным источником
  приложений. Он хранится в
  `%LOCALAPPDATA%\WindowsLaunchpad\Applications`; Windows Shell API извлекает
  иконки и запускает выбранный элемент.
- Перед показом окна снимается содержимое текущего монитора. Оно уменьшается,
  проходит три box-blur pass и растягивается под тёмной полупрозрачной tint-
  плёнкой. Поэтому фон похож на macOS material и сохраняет цвета реальных обоев.
- На Windows 11 дополнительно запрашивается системный
  `DWMSBT_TRANSIENTWINDOW` — Desktop Acrylic — как compositor fallback.
- CMake создаёт обычный C++20 target для Visual Studio 2022 без Windows App SDK,
  NuGet и MSIX runtime.
- В EXE встроена многоразмерная иконка, а процесс и создаваемые Shell-ярлыки
  используют один `DaniilGorchakov.WindowsLaunchpad` AppUserModelID.

WinUI 3 не выбран для первой версии: unpackaged C++-приложению потребовались бы
Windows App SDK runtime/bootstrapper и MSBuild/NuGet-интеграция, что усложнило бы
запрошенную CMake-сборку.

## Компоненты

```text
Launchpad.exe
  |
  +-- ApplicationsCatalog
  |     +-- %LOCALAPPDATA%/WindowsLaunchpad/Applications
  |     +-- ярлыки .lnk/.url/.appref-ms/.exe
  |
  +-- IconCache
  |     +-- IShellItemImageFactory
  |     +-- WIC -> ID2D1Bitmap
  |     +-- eager preload + persistent GPU cache
  |
  +-- LaunchpadWindow
  |     +-- Direct2D renderer
  |     +-- wallpaper snapshot + blur + dark tint
  |     +-- DirectWrite text
  |     +-- search / pages / folders / keyboard / pointer
  |     +-- OLE IDropTarget -> Shell drop-target Applications/
  |
  +-- launchpad_model / launchpad_layout
        +-- Unicode-поиск и фильтр расширений
        +-- расчёт и постоянные границы страниц
        +-- порядок, drag-and-drop, папки и извлечение детей
        +-- LaunchpadLayout.store v2 (записи A / F / P)
```

## Потоки

- HWND, каталог и Direct2D-ресурсы принадлежат одному STA UI-потоку.
- Каталог `Applications/` сканируется до показа окна, автоматически при
  изменениях и вручную по `F5`.
- Shell-иконки загружаются до показа первого кадра и кэшируются как
  `ID2D1Bitmap`. Размер запроса выбирается по DPI начиная с 256 px; bitmap
  рисуется напрямую с сохранением alpha и пропорций.
- При пересоздании render target device-dependent кэш очищается.
- Раскладка загружается до показа, согласуется со списком существующих ярлыков
  и сохраняется через временный файл с заменой целевого.
- Формат `WindowsLaunchpadLayout/2` хранит приложения, папки и явные границы
  страниц; загрузчик остаётся совместимым с файлами версии 1.
- Удаление из UI ограничено каноническими обычными файлами внутри
  `Applications/`. Встроенная тёмная панель собирает подтверждение без
  системного `MessageBox`, затем `IFileOperation` отправляет объект в Корзину.
  Если Корзина недоступна, объект переносится в локальную резервную папку
  `Removed Items`, после чего каталог и раскладка согласуются заново.
- При выходе folder-drag за границу панели модель атомарно извлекает ребёнка,
  а UI переводит то же действие в root-drag. До отпускания хранится полный
  snapshot раскладки: `Esc` и потеря capture восстанавливают папку без записи,
  успешный drop сохраняется ровно один раз.
- UI-поток инициализируется через `OleInitialize`, регистрирует HWND через
  `RegisterDragDrop` и при уничтожении обязательно вызывает `RevokeDragDrop`.
  Внешний `IDataObject` проверяется как массив Shell items, включая виртуальные
  элементы Корзины, а не только `CF_HDROP`. После фильтра расширений операция
  делегируется системному `IDropTarget` каталога `Applications/`: для Корзины
  сохраняется штатная restore/move-семантика, для обычного Проводника
  разрешается только copy. Успешный drop запускает отложенный rescan.
- На Windows временный файл раскладки заменяет текущий через
  `MoveFileExW` с `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`;
  существующий store не удаляется заранее при ошибке замены.
- Анимационный frame pump ждёт однократный high-resolution waitable timer
  вместе с Win32-сообщениями через `MsgWaitForMultipleObjectsEx`. Дедлайны
  считаются в QPC и выравниваются по данным `DwmGetCompositionTimingInfo`.
- Запуск выполняется `ShellExecuteExW`; окно скрывается, останавливает frame
  pump и сохраняет прогретые иконки в процессе.

Такой вариант минимален и предсказуем для прототипа. Если каталог станет
существенно больше, сканирование и декодирование иконок можно вынести в MTA
worker с передачей готовых пикселей через `PostMessage`.

## Формат раскладки

`LaunchpadLayout.store` начинается с заголовка
`WindowsLaunchpadLayout/2` и содержит три типа записей:

- `A` — отдельное приложение;
- `F` — папка с упорядоченным списком дочерних приложений;
- `P` — явная граница страницы.

Файлы версии 1 по-прежнему читаются, а следующее сохранение переводит их в
версию 2. Загрузчик и модель удаляют ведущие, завершающие и соседние `P`,
поэтому пустые страницы не остаются после отмены переноса или удаления
последнего элемента.

## Режимы процесса

- Обычный запуск показывает окно и скрывает его по `Esc`, сохраняя кэш.
- `--background` создаёт скрытое окно и регистрирует `Win+Alt+Space`.
- `--exit-on-close` завершает процесс по `Esc` для отладки.
- Повторный запуск не создаёт второй экземпляр, а показывает существующий.
- Автозапуск использует только пользовательский ключ `HKCU\...\Run`.
- `--create-shortcuts` создаёт ярлык меню «Пуск» с AppUserModelID;
  `--desktop-shortcut` добавляет такой же ярлык на рабочий стол.
- `--remove-shortcuts` удаляет оба ярлыка при деинсталляции.

## Установка и закрепление

Per-user установщик копирует Release EXE в постоянный каталог
`%LOCALAPPDATA%\Programs\Windows Launchpad`, безопасно мигрирует прежний
каталог приложений и `LaunchpadLayout.store` в локальное хранилище и создаёт
Shell-ярлыки через `IShellLinkW + IPropertyStore`. Благодаря одинаковому
AppUserModelID закреплённая кнопка не отделяется от запускаемого процесса.

Окно сохраняет `WS_EX_TOOLWINDOW`: оно не засоряет `Alt+Tab` и не создаёт
вторую временную кнопку панели задач. Постоянной кнопкой запуска служит
закреплённый ярлык. Само закрепление выполняет пользователь через меню «Пуск»,
поскольку Windows 11 требует явного пользовательского решения.

## Официальные источники

- Apple Support: [View and open apps on Mac](https://support.apple.com/guide/mac-help/open-apps-in-spotlight-mh35840/mac)
- Microsoft Learn: [Direct2D overview](https://learn.microsoft.com/windows/win32/direct2d/direct2d-overview)
- Microsoft Learn: [Direct2D and DirectWrite](https://learn.microsoft.com/windows/win32/direct2d/direct2d-and-directwrite)
- Microsoft Learn: [Known Folder IDs](https://learn.microsoft.com/windows/win32/shell/knownfolderid)
- Microsoft Learn: [IShellItemImageFactory](https://learn.microsoft.com/windows/win32/api/shobjidl_core/nn-shobjidl_core-ishellitemimagefactory)
- Microsoft Learn: [ShellExecuteExW](https://learn.microsoft.com/windows/win32/api/shellapi/nf-shellapi-shellexecuteexw)
- Apple HIG: [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- Apple HIG: [Search fields](https://developer.apple.com/design/human-interface-guidelines/search-fields)
- Microsoft Learn: [DWM system backdrop types](https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)
- Microsoft Learn: [CMake projects in Visual Studio](https://learn.microsoft.com/cpp/build/cmake-projects-in-visual-studio)
- Microsoft Learn: [Application User Model IDs](https://learn.microsoft.com/windows/win32/shell/appids)
- Microsoft Learn: [Pin your app to the taskbar](https://learn.microsoft.com/windows/apps/develop/windows-integration/pin-to-taskbar)
