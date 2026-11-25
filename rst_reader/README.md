# ANSYS RST Reader

Программа для чтения файлов результатов ANSYS MAPDL (`.rst` / `.rnn`) **без использования ANSYS**.

## Описание

Реализован прямой бинарный парсинг формата ANSYS RST на основе анализа:
- `pymapdl-reader` (ansys/pymapdl-reader) — Python-библиотека для чтения RST
- `ResRdDemo.F` — Fortran-демо из ANSYS Programmer's Reference
- Документации ANSYS Programmer's Reference (Format of Binary Data Files)

### Ключевые подходы из PADT и pymapdl-reader

1. **Стандартный заголовок** — первые 100 слов (400 байт) читаются как сырые данные БЕЗ FORTRAN маркеров
2. **FORTRAN записи** — после стандартного заголовка используются записи формата `[int32: N] [N слов] [int32: N]`
3. **Словесные указатели** — все указатели в заголовках указаны в словах (word = 4 байта) от начала файла
4. **Result header** — запись на словесном смещении 103 содержит 80 слов с указателями на все секции
5. **Сжатие ANSCOMP** — определяется по магическому слову `'ANS-'` в данных записи

## Формат RST файла

```
┌─────────────────────────────────────────────────────────┐
│  Стандартный заголовок (100 слов = 400 байт, БЕЗ маркеров) │
│  [file_num=100] [pad] [file_fmt=12] [pad] [time] [pad]  │
│  [date] [pad] [units] [pad] ... [sys_record_size]       │
├─────────────────────────────────────────────────────────┤
│  FORTRAN записи (после байта 400):                       │
│  [int32: N] [N слов данных] [int32: N]                  │
│                                                         │
│  Record at word 103 = Result Header (80 слов):          │
│  [nnod, nelm, ptrNOD, ptrELM, ptrGEO, ptrDSI, ...]     │
├─────────────────────────────────────────────────────────┤
│  DSI: Data Set Index — указатели на наборы результатов   │
├─────────────────────────────────────────────────────────┤
│  GEO: Геометрия (узлы, типы элементов)                   │
├─────────────────────────────────────────────────────────┤
│  Для каждого набора результатов:                         │
│    Solution Header → NSL (nodal solution) → ESL (elem)  │
└─────────────────────────────────────────────────────────┘
```

### Сжатые записи (ANSCOMP)

Если данные записи начинаются с `'ANS-'`, запись сжата:
```
[ANS- version] [reserved] [orig_len] [nchunks] [chunk_lens...] [compressed data...]
```

Распаковка встроенным DEFLATE-декодером (RFC 1951 + zlib RFC 1950).

## Сборка

### Linux (g++ / clang)

```bash
cd rst_reader
make
```

### Windows (MinGW)

```bash
cd rst_reader
make WIN=1
```

### Требования

- C++ компилятор с поддержкой C++17 (g++ >= 7, clang >= 5)
- Make

### Ручная сборка

```bash
g++ -std=c++17 -O2 -Wall -I. -c rst_reader.cpp -o rst_reader.o
g++ -std=c++17 -O2 -Wall -I. -c deflate.cpp -o deflate.o
g++ -std=c++17 -O2 -Wall -I. -c main.cpp -o main.o
g++ -std=c++17 -o rst_reader_demo rst_reader.o deflate.o main.o -pthread
```

## Использование

```bash
./rst_reader_demo <file.rst> [опции]
```

### Опции

| Опция | Описание |
|-------|----------|
| `--list` | Показать информацию о файле (наборы, компоненты) |
| `--set N` | Прочитать набор с индексом N (0-based) |
| `--substep N` | Найти набор по номеру подшага |
| `--nodes` | Вывести координаты узлов |
| `--elements` | Вывести элементы |
| `--displacements` | Вывести перемещения |
| `--stresses` | Вывести узловые напряжения |
| `--mises` | Добавить напряжения Мизеса |
| `--diagnose` | Показать диагностику формата файла |
| `--csv FILE` | Сохранить результат в CSV |

### Примеры

```bash
# Диагностика формата файла
./rst_reader_demo model.rst --diagnose

# Информация о файле
./rst_reader_demo model.rst --list

# Узловые напряжения для первого набора
./rst_reader_demo model.rst --set 0 --stresses --mises

# Координаты узлов и элементы
./rst_reader_demo model.rst --nodes --elements
```

## Архитектура

```
rst_reader/
├── rst_reader.h      — заголовочный файл (структуры данных, API)
├── rst_reader.cpp    — реализация ридера (парсинг RST, чтение данных)
├── deflate.cpp       — DEFLATE декодер (RFC 1951 + zlib обёртка)
├── main.cpp          — демонстрационная программа (CLI)
├── Makefile          — скрипт сборки
└── README.md         — документация
```

## Отличия от предыдущей версии

1. **Убрано автоматическое определение формата** (reclen в словах/байтах, 32/64 бит) — формат фиксирован
2. **Стандартный заголовок** читается как сырые данные (без FORTRAN маркеров)
3. **FORTRAN записи** используют стандартный формат `[int32: N] [N слов] [int32: N]`
4. **Указатели** — все в словах от начала файла (как в pymapdl-reader)
5. **Result header** на фиксированной позиции (word 103)

## Ссылки

- [PADT: Reading ANSYS RST from C/C++](https://www.padtinc.com/2016/02/19/reading-ansys-mechanical-rst-c-cpp-part1/)
- [pymapdl-reader](https://github.com/ansys/pymapdl-reader) — Python библиотека
- [ANSYS Programmer's Reference](https://ansyshelp.ansys.com/) — Format of Binary Data Files
- [RFC 1951](https://tools.ietf.org/html/rfc1951) — DEFLATE формат
- [RFC 1950](https://tools.ietf.org/html/rfc1950) — zlib формат

## Лицензия

Свободное использование. Программное обеспечение предоставляется "как есть", без гарантий.
