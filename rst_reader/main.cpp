// main.cpp — демонстрационная программа чтения ANSYS RST файлов
//
// Использование:
//   ./rst_reader_demo <file.rst> [опции]
//
// Подход: прямой бинарный парсинг формата ANSYS RST.
// Стандартный заголовок (100 слов) + FORTRAN записи с флагами.
// Аналогично pymapdl-reader (ansys-mapdl-reader).

#include "rst_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <set>
#include <cstdint>

using namespace rst_reader;

// ============================================================================
// Вспомогательные функции вывода
// ============================================================================

static void print_separator(char c = '-', int len = 70) {
    for (int i = 0; i < len; ++i) std::putchar(c);
    std::putchar('\n');
}

static void print_header_info(const RstReader& reader) {
    const auto& hdr = reader.header();
    const auto& std_hdr = reader.std_header();
    std::printf("=== Информация о файле ===\n");
    std::printf("Файл: %s\n", reader.path().c_str());
    std::printf("Версия ANSYS: %s\n", hdr.version.c_str());
    std::printf("Jobname: %s\n", hdr.jobname.c_str());
    std::printf("Заголовок: %s\n", hdr.title.c_str());
    std::printf("Единицы: %d\n", hdr.units);
    std::printf("System record size: %d слов\n", std_hdr.sys_record_size);
    std::printf("Сжатие: %s\n", reader.is_compressed() ? "ANSCOMP (DEFLATE)" : "нет");
    std::printf("Наборов результатов: %d\n", reader.num_sets());
    std::printf("Узлов: %lld\n", (long long)reader.num_nodes());
    std::printf("Элементов: %lld\n", (long long)reader.num_elements());
    std::printf("Компонентов (Name Selections): %zu\n", reader.components().size());
}

static void print_result_sets(const RstReader& reader) {
    std::printf("\n=== Наборы результатов ===\n");
    std::printf("%-6s %-8s %-10s %-15s %-15s\n", "Index", "Step", "Substep", "Time", "Ptr(words)");
    print_separator();
    for (size_t i = 0; i < reader.result_sets().size(); ++i) {
        const auto& rs = reader.result_sets()[i];
        std::printf("%-6zu %-8d %-10d %-15.6g %-15lld\n", 
                    i, rs.step, rs.substep, rs.time, (long long)rs.rpointer_words);
    }
}

static void print_components(const RstReader& reader) {
    std::printf("\n=== Name Selections (компоненты) ===\n");
    if (reader.components().empty()) {
        std::printf("  (нет компонентов)\n");
        return;
    }
    for (const auto& c : reader.components()) {
        if (c.is_elem)
            std::printf("  %-32s [ELEM] %zu элементов\n", c.name.c_str(), c.elems.size());
        else
            std::printf("  %-32s [NODE] %zu узлов\n", c.name.c_str(), c.nodes.size());
    }
}

static void print_nodes(const std::vector<Node>& nodes, int max_print = 20) {
    std::printf("\n=== Узлы (показано %d из %zu) ===\n",
                std::min(max_print, static_cast<int>(nodes.size())), nodes.size());
    std::printf("%-10s %-15s %-15s %-15s\n", "ID", "X", "Y", "Z");
    print_separator();
    int count = 0;
    for (const auto& nd : nodes) {
        if (count++ >= max_print) break;
        std::printf("%-10d %-15.6g %-15.6g %-15.6g\n", nd.id, nd.x, nd.y, nd.z);
    }
    if (static_cast<int>(nodes.size()) > max_print)
        std::printf("  ... ещё %zu узлов\n", nodes.size() - max_print);
}

static void print_elements(const std::vector<Element>& elems, int max_print = 20) {
    std::printf("\n=== Элементы (показано %d из %zu) ===\n",
                std::min(max_print, static_cast<int>(elems.size())), elems.size());
    std::printf("%-10s %-8s %s\n", "ID", "Nodes", "Connectivity");
    print_separator();
    int count = 0;
    for (const auto& el : elems) {
        if (count++ >= max_print) break;
        std::printf("%-10d %-8zu ", el.id, el.nodes.size());
        for (size_t i = 0; i < el.nodes.size() && i < 8; ++i) {
            std::printf("%d ", el.nodes[i]);
        }
        if (el.nodes.size() > 8) std::printf("...");
        std::printf("\n");
    }
    if (static_cast<int>(elems.size()) > max_print)
        std::printf("  ... ещё %zu элементов\n", elems.size() - max_print);
}

static void print_displacements(const std::vector<Displacement>& disps, int max_print = 20) {
    std::printf("\n=== Перемещения (показано %d из %zu) ===\n",
                std::min(max_print, static_cast<int>(disps.size())), disps.size());
    std::printf("%-10s %-15s %-15s %-15s\n", "Node", "UX", "UY", "UZ");
    print_separator();
    int count = 0;
    for (const auto& d : disps) {
        if (count++ >= max_print) break;
        std::printf("%-10d", d.node_id);
        for (int i = 0; i < 3; ++i) {
            if (i < static_cast<int>(d.values.size()))
                std::printf(" %-15.6e", d.values[i]);
            else
                std::printf(" %-15s", "0.0");
        }
        std::printf("\n");
    }
    if (static_cast<int>(disps.size()) > max_print)
        std::printf("  ... ещё %zu записей\n", disps.size() - max_print);
}

static void print_stresses(const std::vector<NodalStress>& stresses, bool show_mises,
                           int max_print = 20) {
    std::printf("\n=== Узловые напряжения (показано %d из %zu) ===\n",
                std::min(max_print, static_cast<int>(stresses.size())), stresses.size());
    if (show_mises)
        std::printf("%-10s %-12s %-12s %-12s %-12s %-12s %-12s %-12s\n",
                    "Node", "SX", "SY", "SZ", "SXY", "SYZ", "SXZ", "MISES");
    else
        std::printf("%-10s %-12s %-12s %-12s %-12s %-12s %-12s\n",
                    "Node", "SX", "SY", "SZ", "SXY", "SYZ", "SXZ");
    print_separator();
    int count = 0;
    for (const auto& s : stresses) {
        if (count++ >= max_print) break;
        std::printf("%-10d %-12.5e %-12.5e %-12.5e %-12.5e %-12.5e %-12.5e",
                    s.node_id, s.sx, s.sy, s.sz, s.sxy, s.syz, s.sxz);
        if (show_mises)
            std::printf(" %-12.5e", s.von_mises());
        std::printf("\n");
    }
    if (static_cast<int>(stresses.size()) > max_print)
        std::printf("  ... ещё %zu записей\n", stresses.size() - max_print);
}

// ============================================================================
// Запись в CSV
// ============================================================================

static bool write_csv(const std::string& path, const std::vector<NodalStress>& stresses,
                      bool show_mises) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    
    if (show_mises)
        std::fprintf(f, "Node,SX,SY,SZ,SXY,SYZ,SXZ,MISES\n");
    else
        std::fprintf(f, "Node,SX,SY,SZ,SXY,SYZ,SXZ\n");
    
    for (const auto& s : stresses) {
        std::fprintf(f, "%d,%.8e,%.8e,%.8e,%.8e,%.8e,%.8e",
                     s.node_id, s.sx, s.sy, s.sz, s.sxy, s.syz, s.sxz);
        if (show_mises)
            std::fprintf(f, ",%.8e", s.von_mises());
        std::fprintf(f, "\n");
    }
    
    std::fclose(f);
    return true;
}

// ============================================================================
// Диагностика формата файла
// ============================================================================

static void diagnose_file(const std::string& rst_path) {
    FILE* f = std::fopen(rst_path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "Не удалось открыть файл для диагностики\n");
        return;
    }
    
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    
    std::fprintf(stderr, "\n=== Диагностика формата файла ===\n");
    std::fprintf(stderr, "Размер файла: %ld байт\n", fsize);
    
    // Читаем стандартный заголовок (первые 400 байт)
    uint8_t hdr[400] = {};
    size_t nread = std::fread(hdr, 1, 400, f);
    std::fprintf(stderr, "Прочитано стандартного заголовка: %zu байт\n", nread);
    
    // Первые 10 слов
    std::fprintf(stderr, "\nПервые 30 слов стандартного заголовка:\n");
    for (int i = 0; i < 30 && (i+1)*4 <= static_cast<int>(nread); ++i) {
        int32_t val = 0;
        std::memcpy(&val, hdr + i * 4, 4);
        std::fprintf(stderr, "  word[%2d] (byte %3d): %d (0x%08x)\n", i, i*4, val, val);
    }
    
    // Проверяем file number
    int32_t file_num = 0;
    std::memcpy(&file_num, hdr, 4);
    std::fprintf(stderr, "\nFile number (word 0): %d (expected 100 for RST)\n", file_num);
    
    int32_t file_fmt = 0;
    std::memcpy(&file_fmt, hdr + 8, 4);  // word[2] = byte 8
    std::fprintf(stderr, "File format (word 2): %d (expected 12 for RST)\n", file_fmt);
    
    // Смотрим на данные после стандартного заголовка
    if (fsize > 408) {
        std::fseek(f, 400, SEEK_SET);
        std::fprintf(stderr, "\nДанные после стандартного заголовка (byte 400+):\n");
        uint8_t buf[64] = {};
        std::fread(buf, 1, 64, f);
        
        int32_t pre = 0;
        std::memcpy(&pre, buf, 4);
        std::fprintf(stderr, "  Pre-marker at byte 400: %d\n", pre);
        
        if (pre > 0 && pre < 1000000) {
            std::fprintf(stderr, "  -> FORTRAN запись: %d слов (%d байт)\n", pre, pre * 4);
            std::fprintf(stderr, "  Следующая запись на byte: %ld\n", 400 + 4 + pre * 4);
        } else if (pre == 0) {
            std::fprintf(stderr, "  -> Нет FORTRAN записи (возможно padding или данные заголовка)\n");
        }
        
        // Проверяем запись на словесном смещении 103
        long pos103 = 103 * 4;
        if (pos103 + 8 <= fsize) {
            std::fseek(f, pos103, SEEK_SET);
            uint8_t buf103[16] = {};
            std::fread(buf103, 1, 16, f);
            
            int32_t pre103 = 0;
            std::memcpy(&pre103, buf103, 4);
            
            std::fprintf(stderr, "\nЗапись на word offset 103 (byte %ld):\n", pos103);
            std::fprintf(stderr, "  Pre-marker: %d\n", pre103);
            
            if (pre103 > 0 && pre103 < 10000) {
                std::fprintf(stderr, "  -> Похоже на валидный result header (%d слов)\n", pre103);
                std::fprintf(stderr, "  Формат: [int32:N][N слов] БЕЗ post-marker\n");
                std::fprintf(stderr, "  Размер записи: %d байт (4 + %d*4)\n", 
                            4 + pre103 * 4, pre103);
                std::fprintf(stderr, "  Следующая запись на byte: %ld (word %ld)\n",
                            pos103 + 4 + pre103 * 4, (pos103 + 4 + pre103 * 4) / 4);
                
                // Читаем и выводим содержимое result header
                std::fprintf(stderr, "\n=== Содержимое Result Header ===\n");
                std::vector<int32_t> rh_data(pre103);
                std::fseek(f, pos103 + 4, SEEK_SET);  // пропускаем pre-marker
                std::fread(rh_data.data(), 4, pre103, f);
                
                for (int i = 0; i < pre103; ++i) {
                    std::fprintf(stderr, "  hdr[%2d] = %d\n", i, rh_data[i]);
                }
                
                // Пытаемся интерпретировать некоторые поля
                std::fprintf(stderr, "\n=== Интерпретация Result Header ===\n");
                std::fprintf(stderr, "  hdr[2] (nnod) = %d\n", rh_data[2]);
                std::fprintf(stderr, "  hdr[3] (nelm) = %d\n", rh_data[3]);
                std::fprintf(stderr, "  hdr[16] (ptrNOD) = %d\n", rh_data[16]);
                std::fprintf(stderr, "  hdr[17] (ptrELM) = %d\n", rh_data[17]);
                std::fprintf(stderr, "  hdr[18] (ptrGEO) = %d\n", rh_data[18]);
                std::fprintf(stderr, "  hdr[19] (nsets) = %d\n", rh_data[19]);
                std::fprintf(stderr, "  hdr[20] (ptrDSI) = %d\n", rh_data[20]);
                std::fprintf(stderr, "  hdr[21] (ptrTIM) = %d\n", rh_data[21]);
                std::fprintf(stderr, "  hdr[22] (ptrLSP) = %d\n", rh_data[22]);
                std::fprintf(stderr, "  hdr[25] (ptrCOMP) = %d\n", rh_data[25]);
            }
        }
    }
    
    std::fclose(f);
}

// ============================================================================
// Использование
// ============================================================================

static void usage() {
    std::printf(
        "ANSYS RST Reader Demo — чтение файлов результатов ANSYS MAPDL\n"
        "=============================================================\n\n"
        "Использование: rst_reader_demo <file.rst> [опции]\n\n"
        "Опции:\n"
        "  --list              показать информацию о файле\n"
        "  --set N             прочитать набор с индексом N (0-based)\n"
        "  --substep N         найти набор по номеру подшага\n"
        "  --nodes             вывести координаты узлов\n"
        "  --elements          вывести элементы\n"
        "  --displacements     вывести перемещения для выбранного набора\n"
        "  --stresses          вывести узловые напряжения\n"
        "  --mises             добавить напряжения Мизеса\n"
        "  --component NAME    фильтр по Name Selection\n"
        "  --csv FILE          сохранить результат в CSV\n"
        "  --diagnose          показать диагностику формата файла\n"
        "  --help              показать эту справку\n\n"
        "Подход: прямой бинарный парсинг ANSYS RST формата.\n"
        "Стандартный заголовок (100 слов) + FORTRAN записи с флагами.\n"
        "Аналогично pymapdl-reader (ansys-mapdl-reader).\n"
    );
}

// ============================================================================
// Главная функция
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    
    // Парсинг аргументов
    std::string rst_path = argv[1];
    bool do_list = false;
    bool do_nodes = false;
    bool do_elements = false;
    bool do_displacements = false;
    bool do_stresses = false;
    bool do_mises = false;
    bool do_diagnose = false;
    int set_index = -1;
    int find_substep = 0;
    std::string component_name;
    std::string csv_path;
    
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto next_arg = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Ошибка: нужен аргумент после %s\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        
        if (arg == "--list") do_list = true;
        else if (arg == "--set") set_index = std::atoi(next_arg());
        else if (arg == "--substep") find_substep = std::atoi(next_arg());
        else if (arg == "--nodes") do_nodes = true;
        else if (arg == "--elements") do_elements = true;
        else if (arg == "--displacements") do_displacements = true;
        else if (arg == "--stresses") do_stresses = true;
        else if (arg == "--mises") do_mises = true;
        else if (arg == "--component") component_name = next_arg();
        else if (arg == "--csv") csv_path = next_arg();
        else if (arg == "--diagnose") do_diagnose = true;
        else if (arg == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "Неизвестная опция: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    
    // Диагностика
    if (do_diagnose) {
        diagnose_file(rst_path);
    }
    
    // Открытие файла
    RstReader reader;
    std::string err;
    if (!reader.open(rst_path, err)) {
        std::fprintf(stderr, "Ошибка открытия RST: %s\n", err.c_str());
        std::fprintf(stderr, "\nЗапустите с --diagnose для анализа формата файла.\n");
        diagnose_file(rst_path);
        return 1;
    }
    
    // Вывод информации
    print_header_info(reader);
    
    if (do_list || (set_index < 0 && find_substep == 0 && !do_nodes && !do_elements)) {
        print_result_sets(reader);
        print_components(reader);
        if (do_list) { reader.close(); return 0; }
        if (set_index < 0 && find_substep == 0 && !do_nodes && !do_elements) {
            std::printf("\nУкажите --set N или --substep N для чтения данных.\n");
            reader.close();
            return 0;
        }
    }
    
    // Разрешение индекса набора
    if (set_index < 0 && find_substep > 0) {
        set_index = reader.find_set_by_substep(find_substep);
        if (set_index < 0) {
            std::fprintf(stderr, "Набор с подшагом %d не найден.\n", find_substep);
            reader.close();
            return 1;
        }
    }
    
    // Чтение узлов
    if (do_nodes) {
        std::vector<Node> nodes;
        if (reader.read_nodes(nodes, err)) {
            print_nodes(nodes);
        } else {
            std::fprintf(stderr, "Ошибка чтения узлов: %s\n", err.c_str());
        }
    }
    
    // Чтение элементов
    if (do_elements) {
        std::vector<Element> elems;
        if (reader.read_elements(elems, err)) {
            print_elements(elems);
        } else {
            std::fprintf(stderr, "Ошибка чтения элементов: %s\n", err.c_str());
        }
    }
    
    // Чтение перемещений
    if (do_displacements && set_index >= 0) {
        std::vector<Displacement> disps;
        if (reader.read_displacements(set_index, disps, err)) {
            print_displacements(disps);
        } else {
            std::fprintf(stderr, "Ошибка чтения перемещений: %s\n", err.c_str());
        }
    }
    
    // Чтение напряжений
    if (do_stresses && set_index >= 0) {
        std::vector<NodalStress> stresses;
        if (reader.read_nodal_stresses(set_index, stresses, err)) {
            print_stresses(stresses, do_mises);
            
            if (!csv_path.empty()) {
                if (write_csv(csv_path, stresses, do_mises))
                    std::printf("\nСохранено в: %s\n", csv_path.c_str());
                else
                    std::fprintf(stderr, "Ошибка записи CSV: %s\n", err.c_str());
            }
        } else {
            std::fprintf(stderr, "Ошибка чтения напряжений: %s\n", err.c_str());
        }
    }
    
    reader.close();
    return 0;
}
