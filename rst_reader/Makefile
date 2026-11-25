# Makefile для ANSYS RST Reader
#
# Сборка: make            (g++ >= 9 / clang >= 12, Linux)
#         make WIN=1      (MinGW, Windows)
#
# Требования: C++17 (для structured bindings)
#
# Подход: прямой бинарный парсинг ANSYS RST файлов.
# Стандартный заголовок (100 слов) + FORTRAN записи с флагами.
# Аналогично pymapdl-reader (ansys-mapdl-reader).

CXX      ?= g++
CXXFLAGS += -std=c++17 -O2 -Wall -Wextra -I.
LDFLAGS  += -pthread

SRC := rst_reader.cpp deflate.cpp main.cpp
OBJ := $(SRC:.cpp=.o)
BIN := rst_reader_demo

# Windows-сборка
ifdef WIN
  CXX = g++
  LDFLAGS += -static
  BIN := rst_reader_demo.exe
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp rst_reader.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN) rst_reader_demo.exe

# Быстрый тест
test: $(BIN)
	@if [ -f file_no_compression.rst ]; then \
		echo "=== Тест: несжатый файл ==="; \
		./$(BIN) file_no_compression.rst --list --diagnose; \
	elif [ -f file_compressed.rst ]; then \
		echo "=== Тест: сжатый файл ==="; \
		./$(BIN) file_compressed.rst --list --diagnose; \
	else \
		echo "RST файл не найден. Поместите .rst файл в текущую директорию."; \
	fi

.PHONY: all clean test
