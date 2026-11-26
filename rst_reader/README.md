# ANSYS RST Reader

Программа для чтения файлов результатов ANSYS MAPDL (.rst) без использования ANSYS.

## Текущий статус

### Что работает
- ✅ Чтение стандартного заголовка (первые 400 байт)
- ✅ Определение формата файла (RST)
- ✅ Чтение result header (запись на word 103)
- ✅ Формат FORTRAN записей: `[int32:N][N слов]` БЕЗ post-marker

### Что в разработке
- 🔧 Парсинг указателей в result header
- 🔧 Чтение наборов результатов
- 🔧 Чтение геометрии (узлы, элементы)
- 🔧 Чтение результатов (перемещения, напряжения)

## Формат файла

### Стандартный заголовок (400 байт)
```
word[0]  = 100              (file number)
word[2]  = 12               (file format: RST)
word[4]  = time
word[6]  = units            (6 = MPA)
word[11] = "2.22"           (version)
word[27] = 16384            (system record size)
```

### FORTRAN записи (после byte 400)
```
[int32: N] [N слов данных]
```
**БЕЗ post-marker** (в отличие от стандартного FORTRAN unformatted sequential)

### Result Header (word 103, 80 слов)
Содержит указатели на все секции файла. Позиции указателей определяются по документации ANSYS Programmer's Reference.

## Использование

```bash
# Сборка
cd rst_reader
make

# Диагностика формата
./rst_reader_demo file.rst --diagnose

# Информация о файле
./rst_reader_demo file.rst --list

# Чтение узлов и элементов
./rst_reader_demo file.rst --nodes --elements

# Чтение напряжений
./rst_reader_demo file.rst --set 0 --stresses --mises
```

## Источники

- [PADT: Reading ANSYS RST from C/C++](https://www.padtinc.com/2016/02/19/reading-ansys-mechanical-rst-c-cpp-part1/)
- [pymapdl-reader](https://github.com/ansys/pymapdl-reader)
- [ANSYS Programmer's Reference](https://ansyshelp.ansys.com/)

## Лицензия

Свободное использование.
