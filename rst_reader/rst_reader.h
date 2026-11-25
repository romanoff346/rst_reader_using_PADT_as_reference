// rst_reader.h — заголовочный файл ридера ANSYS RST файлов
//
// Формат ANSYS RST:
// 1. Стандартный заголовок (100 слов = 400 байт) — сырые данные без маркеров
// 2. Записи в формате FORTRAN с расширением ANSYS:
//    [int32: N] [int32: flags] [N слов данных] [int32: N]
// 3. Все указатели — в словах (word = 4 байта) от начала файла
//
// Подход аналогичен pymapdl-reader (ansys/pymapdl-reader).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace rst_reader {

// ============================================================================
// Константы
// ============================================================================
constexpr int kWord = 4;                          // размер слова в байтах
constexpr int kStdHeaderWords = 100;              // стандартный заголовок: 100 слов
constexpr int kStdHeaderBytes = kStdHeaderWords * kWord;  // 400 байт
constexpr int kResultHeaderRecord = 103;          // запись результата: слово 103

// ============================================================================
// Платформо-зависимые утилиты ввода-вывода
// ============================================================================
using NativeFd =
#if defined(_WIN32)
    HANDLE;
#else
    int;
#endif

NativeFd fd_open(const std::string& path);
void fd_close(NativeFd fd);
bool fd_valid(NativeFd fd);
bool fd_pread(NativeFd fd, void* dst, size_t count, int64_t offset);

// ============================================================================
// Утилиты чтения бинарных данных
// ============================================================================
inline int64_t rd_i64(const uint8_t* p) {
    int64_t v; std::memcpy(&v, p, 8); return v;
}
inline int32_t rd_i32(const uint8_t* p) {
    int32_t v; std::memcpy(&v, p, 4); return v;
}
inline double rd_f64(const uint8_t* p) {
    double v; std::memcpy(&v, p, 8); return v;
}
inline float rd_f32(const uint8_t* p) {
    float v; std::memcpy(&v, p, 4); return v;
}

// Проверка магического слова сжатой записи ANSCOMP
inline bool is_comp_magic(const void* p) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return std::memcmp(b, "ANS-", 4) == 0 && b[4] >= '1' && b[4] <= '9';
}

// ============================================================================
// Структуры данных
// ============================================================================

// Один набор результатов (load step / substep)
struct ResultSet {
    int32_t step = 0;
    int32_t substep = 0;
    double  time = 0.0;
    int64_t rpointer_words = 0; // указатель на заголовок решения (в словах)
};

// Компонент (Name Selection)
struct Component {
    std::string name;
    std::vector<int32_t> nodes;
    std::vector<int32_t> elems;
    bool is_node = false;
    bool is_elem = false;
};

// Узел модели
struct Node {
    int32_t id = 0;
    double x = 0.0, y = 0.0, z = 0.0;
    double rx = 0.0, ry = 0.0, rz = 0.0;
};

// Элемент модели
struct Element {
    int32_t id = 0;
    int32_t type_id = 0;
    int32_t real_id = 0;
    int32_t material_id = 0;
    std::vector<int32_t> nodes;
};

// Тип элемента
struct ElementType {
    int32_t id = 0;
    std::vector<int32_t> data;
};

// Перемещения узла
struct Displacement {
    int32_t node_id = 0;
    std::vector<double> values;
};

// Реакция в узле
struct ReactionForce {
    int32_t node_id = 0;
    int32_t dof = 0;
    double value = 0.0;
};

// Напряжения в узле
struct NodalStress {
    int32_t node_id = 0;
    double sx = 0, sy = 0, sz = 0;
    double sxy = 0, syz = 0, sxz = 0;
    
    double von_mises() const {
        return std::sqrt(0.5 * ((sx-sy)*(sx-sy) + (sy-sz)*(sy-sz) + (sx-sz)*(sx-sz))
                         + 3.0 * (sxy*sxy + syz*syz + sxz*sxz));
    }
};

// Заголовок файла результатов
struct FileHeader {
    std::string title;
    std::string subtitle;
    std::string jobname;
    std::string version;
    int32_t units = 0;
    int32_t file_format = 0;
};

// Стандартный заголовок файла
struct StandardHeader {
    int32_t file_number = 0;     // word 0: should be 100
    int32_t file_format = 0;     // word 2: 12 for RST
    int32_t time_val = 0;        // word 4
    int32_t date_val = 0;        // word 6
    int32_t units = 0;           // word 8
    std::string version;         // word 11 (4 chars)
    std::string machine;         // words 13-15 (12 chars)
    std::string jobname;         // words 16-17 (8 chars)
    int32_t sys_record_size = 0; // word 27: system record size
};

// Информация о FORTRAN записи
struct RecordInfo {
    int32_t num_words = 0;   // число слов данных
    int32_t flags = 0;       // reserved (для совместимости)
    int64_t total_bytes = 0; // полный размер записи на диске (с маркерами)
};

// ============================================================================
// Основной класс ридера RST файлов
// ============================================================================
class RstReader {
public:
    RstReader() = default;
    ~RstReader();
    
    // Открытие/закрытие файла
    bool open(const std::string& path, std::string& err);
    void close();
    bool is_open() const { return fp_ != nullptr; }
    
    // ---- Метаданные ----
    const FileHeader& header() const { return header_; }
    const StandardHeader& std_header() const { return std_header_; }
    int num_sets() const { return nsets_; }
    int64_t num_nodes() const { return n_nodes_; }
    int64_t num_elements() const { return n_elem_; }
    const std::vector<ResultSet>& result_sets() const { return sets_; }
    const std::vector<Component>& components() const { return components_; }
    
    // ---- Геометрия ----
    bool read_nodes(std::vector<Node>& nodes, std::string& err);
    bool read_elements(std::vector<Element>& elems, std::string& err);
    
    // ---- Результаты ----
    bool read_displacements(int set_index, std::vector<Displacement>& disps, std::string& err);
    bool read_nodal_stresses(int set_index, std::vector<NodalStress>& stresses, std::string& err);
    
    // ---- Поиск ----
    int find_set_by_substep(int substep) const;
    int find_set_by_time(double time, double tol = 1e-9) const;
    
    // ---- Информация о файле ----
    bool is_compressed() const { return compressed_; }
    const std::string& path() const { return path_; }

private:
    // Чтение стандартного заголовка (первые 100 слов без маркеров)
    bool read_std_header(std::string& err);
    
    // Чтение FORTRAN записи по словесному смещению
    // Формат: [int32: N] [int32: flags] [N слов] [int32: N]
    bool read_record_at(int64_t word_offset, std::vector<uint8_t>& data,
                        RecordInfo& info, std::string& err);
    
    // Чтение записи как массива int32
    bool read_record_int32(int64_t word_offset, std::vector<int32_t>& data,
                           RecordInfo& info, std::string& err);
    
    // Чтение записи как массива double
    bool read_record_f64(int64_t word_offset, std::vector<double>& data,
                         RecordInfo& info, std::string& err);
    
    // Пропуск записи (получить размер для перехода к следующей)
    int64_t record_total_words(int64_t word_offset);
    
    // Декомпрессия ANSCOMP
    bool ansc_uncompress(const uint8_t* body, size_t body_bytes,
                         std::vector<uint8_t>& out);
    
    // Парсинг компонентов
    void parse_components(const std::vector<int32_t>& data);
    
    // Чтение заголовка результата (result header)
    bool read_result_header(std::string& err);
    
    // Данные файла
    FILE* fp_ = nullptr;
    std::string path_;
    int64_t file_bytes_ = 0;
    
    // Заголовки
    StandardHeader std_header_;
    FileHeader header_;
    
    // Данные результата
    int nsets_ = 0;
    int64_t n_nodes_ = 0;
    int64_t n_elem_ = 0;
    int32_t maxcomp_ = 0;
    
    // Указатели из заголовка результата
    int64_t ptr_geo_ = 0;      // указатель на геометрию (в словах)
    int64_t ptr_comp_ = 0;     // указатель на компоненты
    int64_t ptr_elem_ = 0;     // указатель на элементы
    int64_t ptr_nod_ = 0;      // nodal equivalence table
    int64_t ptr_elm_ = 0;      // element equivalence table
    int64_t ptr_dsi_ = 0;      // data set index
    int64_t ptr_tim_ = 0;      // time values
    int64_t ptr_lsp_ = 0;      // load step table
    
    // Таблицы
    std::vector<ResultSet> sets_;
    std::vector<Component> components_;
    
    // Эквивалентность узлов/элементов
    std::vector<int32_t> neqv_;  // nodal equivalence
    std::vector<int32_t> eeqv_;  // element equivalence
    
    // Поддержка сжатых записей
    bool compressed_ = false;
    
    // Кэш распакованных записей
    std::map<int64_t, std::vector<uint8_t>> unc_cache_;
    mutable std::mutex unc_mtx_;
};

// ============================================================================
// DEFLATE декодер
// ============================================================================
namespace deflate {

bool inflate_raw(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst);
bool inflate_zlib(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst);

} // namespace deflate

} // namespace rst_reader
