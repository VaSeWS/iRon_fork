# Проект iRon

## Назначение

iRon — небольшое Windows-приложение с прозрачными оверлеями для iRacing: Relative, Inputs, Standings и Delta. Основная информация для пользователей — в [`README.md`](../README.md); входная точка приложения — `main()` в [`src/main.cpp`](../src/main.cpp). DDU и Cover сейчас закомментированы и не создаются в рабочем списке оверлеев.

## Сборка и тесты

- Проект использует Visual Studio 2022, MSVC toolset `v143`, стандарт C++17, Windows SDK 10.0; см. [`iron.sln`](../iron.sln) и [`iron.vcxproj`](../iron.vcxproj). Runtime опирается на Win32 и DirectX (D3D11, DXGI, Direct2D, DirectComposition, DirectWrite), а iRacing telemetry — на включённый в репозиторий iRacing SDK. Это не переносимая сборка для macOS/Linux.
- Тестовая цель — [`tests.vcxproj`](../tests.vcxproj), только x64 Debug/Release. Она включает doctest и тесты форматирования, YAML-парсера и логики Delta; Delta-тесты находятся в [`tests/test_delta_logic.cpp`](../tests/test_delta_logic.cpp) и проверяют чистые функции [`src/overlays/delta_logic.h`](../src/overlays/delta_logic.h). Для работы этих unit-тестов не нужен запущенный симулятор; весь интерфейс оверлеев — Windows-specific.
- Обычный путь разработки: открыть `iron.sln` в Visual Studio 2022, собрать нужную конфигурацию; тестовую цель собрать/запустить отдельно из `tests.vcxproj`. В README указано, что могут потребоваться компоненты/библиотеки для DirectX; конкретный состав зависит от установки Visual Studio.

## Запуск и настройки

Запускаемый файл ищет `config.json` в текущей рабочей директории (`Config::m_filename` в [`src/core/Config.h`](../src/core/Config.h)); `main()` загружает его и запускает наблюдение за изменениями (`Config::load`, `Config::watchForChanges` в [`src/core/Config.cpp`](../src/core/Config.cpp)). Отсутствующие ключи получают значения по умолчанию при чтении; созданные значения сохраняются из главного цикла, а изменение файла приводит к повторной загрузке и настройке оверлеев (`handleConfigChange` в [`src/main.cpp`](../src/main.cpp)). Для записи нужен доступ на запись в рабочую директорию.

Оверлеи обычно включаются при статусе `DRIVING`; исключения задаются `canEnableWhileNotDriving()` / `canEnableWhileDisconnected()` ([`src/core/Overlay.cpp`](../src/core/Overlay.cpp), [`src/overlays/OverlayDelta.h`](../src/overlays/OverlayDelta.h)). Горячая клавиша Alt-J включает режим перетаскивания/изменения размера, а остальные горячие клавиши переключают оверлеи; файл настроек можно редактировать во время работы. Доступные пользовательские параметры и общее поведение описаны в [`README.md`](../README.md).

## Источники

- Режимы, пользовательские параметры Delta и ограничения эксплуатации: [`README.md`](../README.md), разделы Delta, Installing & Running, Configuration.
- Фактические runtime-потоки, ключи конфигурации и контракты реализации: [`src/main.cpp`](../src/main.cpp), [`src/core/Config.cpp`](../src/core/Config.cpp), [`src/overlays/OverlayDelta.h`](../src/overlays/OverlayDelta.h), [`src/overlays/delta_logic.h`](../src/overlays/delta_logic.h).
- Утверждения здесь основаны на коде и проектных файлах текущего checkout; это описание не обещает поддержку других ОС/компиляторов или тестирование конкретной конфигурации.