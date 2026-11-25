// rst_reader.cpp — реализация ридера ANSYS RST файлов
//
// Формат ANSYS RST (по документации ANSYS Programmer's Reference и pymapdl-reader):
//
// 1. Стандартный заголовок: первые 100 слов (400 байт) — сырые данные БЕЗ маркеров
//    - Слово 0: file number (100 для RST)
//    - Слово 2: file format (12 для RST)
//    - Слово 27: system record size
//
// 2. После стандартного заголовка — FORTRAN записи в формате ANSYS:
//    [int32: N] [int32: flags] [N слов данных] [int32: N]
//    где flags: bit0=integer, bit1=single_prec, bit2=compressed, bit3=sparse
//
// 3. Заголовок результата (result header) — запись на словесном смещении 103
//    Содержит 80 слов с указателями на все секции файла
//
// 4. Все указатели — в словах (word = 4 байта) от начала файла
//
// Подход: аналогичен pymapdl-reader (ansys-mapdl-reader)
#include "rst_reader.h"

#include <algorithm>
#include <cstdio>

namespace rst_reader {

// ============================================================================
// Платформо-зависимые утилиты
// ============================================================================

NativeFd fd_open(const std::string& path) {
#if defined(_WIN32)
    return ::CreateFileA(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                         nullptr);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}

void fd_close(NativeFd fd) {
#if defined(_WIN32)
    if (fd != INVALID_HANDLE_VALUE) ::CloseHandle(fd);
#else
    if (fd >= 0) ::close(fd);
#endif
}

bool fd_valid(NativeFd fd) {
#if defined(_WIN32)
    return fd != INVALID_HANDLE_VALUE;
#else
    return fd >= 0;
#endif
}

bool fd_pread(NativeFd fd, void* dst, size_t count, int64_t offset) {
#if defined(_WIN32)
    OVERLAPPED ov{};
    LARGE_INTEGER li; li.QuadPart = offset;
    ov.Offset = li.u.LowPart;
    ov.OffsetHigh = li.u.HighPart;
    DWORD got = 0;
    if (!::ReadFile(fd, dst, static_cast<DWORD>(count), &got, &ov)) return false;
    return got == static_cast<DWORD>(count);
#else
    size_t done = 0;
    while (done < count) {
        ssize_t n = ::pread(fd, static_cast<char*>(dst) + done,
                            count - done, offset + static_cast<int64_t>(done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
#endif
}

// ============================================================================
// Декомпрессия ANSCOMP
// ============================================================================

namespace {
constexpr size_t kAnscHdr = 20;  // magic(8)+orig_len(4)+nchunks(4)+reserved(4)
}

bool RstReader::ansc_uncompress(const uint8_t* body, size_t body_bytes,
                                std::vector<uint8_t>& out) {
    if (body_bytes < kAnscHdr) return false;
    const uint8_t* p = body;
    
    // ANSCOMP header format (20 bytes total):
    //   Bytes 0-3:   'ANS-' magic
    //   Bytes 4-7:   version char + padding
    //   Bytes 8-11:  reserved
    //   Bytes 12-15: orig_len (uncompressed size)
    //   Bytes 16-19: nchunks (number of compressed chunks)
    //   Bytes 20+:   chunk_lengths[nchunks] (each int32)
    //   Then:        compressed data
    int32_t orig_len = rd_i32(p + 12);
    int32_t nchunks  = rd_i32(p + 16);
    
    if (orig_len <= 0 || nchunks <= 0 || nchunks > (1 << 20)) return false;
    if (static_cast<size_t>(nchunks) * 4 + kAnscHdr > body_bytes) return false;
    
    size_t pos = kAnscHdr + static_cast<size_t>(nchunks) * 4;
    out.clear();
    out.reserve(static_cast<size_t>(orig_len));
    std::vector<uint8_t> tmp;
    
    for (int32_t i = 0; i < nchunks; ++i) {
        int32_t clen = rd_i32(p + kAnscHdr + static_cast<size_t>(i) * 4);
        if (clen == 0) continue;
        if (clen < 0) {
            // Не-сжатый чанк: |clen| байт literals
            size_t lsz = static_cast<size_t>(-clen);
            if (pos + lsz > body_bytes) return false;
            out.insert(out.end(), p + pos, p + pos + lsz);
            pos += lsz;
        } else {
            // Сжатый чанк: clen байт zlib-compressed data
            if (pos + static_cast<size_t>(clen) > body_bytes) return false;
            if (!deflate::inflate_zlib(p + pos, static_cast<size_t>(clen), tmp))
                return false;
            out.insert(out.end(), tmp.begin(), tmp.end());
            pos += static_cast<size_t>(clen);
        }
    }
    
    if (static_cast<int32_t>(out.size()) < orig_len) return false;
    out.resize(static_cast<size_t>(orig_len));
    return true;
}

// ============================================================================
// Чтение стандартного заголовка (первые 100 слов = 400 байт, БЕЗ маркеров)
// ============================================================================

bool RstReader::read_std_header(std::string& err) {
    if (file_bytes_ < kStdHeaderBytes) {
        err = "file too small for standard header";
        return false;
    }
    
    // Читаем все 100 слов стандартного заголовка
    std::vector<uint8_t> buf(kStdHeaderBytes);
    std::fseek(fp_, 0, SEEK_SET);
    if (std::fread(buf.data(), 1, kStdHeaderBytes, fp_) != kStdHeaderBytes) {
        err = "cannot read standard header";
        return false;
    }
    
    // Стандартный заголовок: значения хранятся на ЧЁТНЫХ словесных позициях
    // (каждое значение — 4 байта, затем 4 байта пропуска):
    //   word 0: file number, word 2: file format, word 4: time, ...
    // Это соответствует формату pymapdl-reader (read_table с пропуском 4 байт)
    
    // Слово 0: file number (должно быть 100 для RST)
    std_header_.file_number = rd_i32(buf.data() + 0 * 4);
    
    if (std_header_.file_number != 100) {
        err = "not an ANSYS result file (file number = " + 
              std::to_string(std_header_.file_number) + ", expected 100)";
        return false;
    }
    
    // Слово 2: file format (12 = RST)
    std_header_.file_format = rd_i32(buf.data() + 2 * 4);
    
    // Слово 4: time
    std_header_.time_val = rd_i32(buf.data() + 4 * 4);
    
    // Слово 6: date
    std_header_.date_val = rd_i32(buf.data() + 6 * 4);
    
    // Слово 8: units
    std_header_.units = rd_i32(buf.data() + 8 * 4);
    
    // Слово 11: version (4 символа) — здесь нет пропуска, f.seek(11*4) в pymapdl
    std_header_.version.assign(reinterpret_cast<const char*>(buf.data() + 11 * 4), 4);
    // Убираем пробелы
    auto end = std::find_if(std_header_.version.rbegin(), std_header_.version.rend(),
                            [](char c){ return c != ' ' && c != '\0'; });
    std_header_.version.erase(end.base(), std_header_.version.end());
    
    // Слова 13-15: machine (12 символов)
    std_header_.machine.assign(reinterpret_cast<const char*>(buf.data() + 13 * 4), 12);
    
    // Слова 16-17: jobname (8 символов)
    std_header_.jobname.assign(reinterpret_cast<const char*>(buf.data() + 16 * 4), 8);
    // Trim
    auto jend = std::find_if(std_header_.jobname.rbegin(), std_header_.jobname.rend(),
                             [](char c){ return c != ' ' && c != '\0'; });
    std_header_.jobname.erase(jend.base(), std_header_.jobname.end());
    
    // Слово 27: system record size
    std_header_.sys_record_size = rd_i32(buf.data() + 27 * 4);
    
    // Заполняем FileHeader
    header_.file_format = std_header_.file_format;
    header_.units = std_header_.units;
    header_.version = std_header_.version;
    header_.jobname = std_header_.jobname;
    
    // Заголовок (title) — byte 42*4 = 168, 20 слов = 80 символов
    if (kStdHeaderBytes >= 62 * 4) {
        header_.title.assign(reinterpret_cast<const char*>(buf.data() + 42 * 4), 80);
        auto tend = std::find_if(header_.title.rbegin(), header_.title.rend(),
                                 [](char c){ return c != ' ' && c != '\0'; });
        header_.title.erase(tend.base(), header_.title.end());
    }
    
    // Подзаголовок — сразу после title (byte 62*4 = 248)
    if (kStdHeaderBytes >= 82 * 4) {
        header_.subtitle.assign(reinterpret_cast<const char*>(buf.data() + 62 * 4), 80);
        auto send = std::find_if(header_.subtitle.rbegin(), header_.subtitle.rend(),
                                 [](char c){ return c != ' ' && c != '\0'; });
        header_.subtitle.erase(send.base(), header_.subtitle.end());
    }
    
    return true;
}

// ============================================================================
// Чтение FORTRAN записи по словесному смещению
// Формат: [int32: N] [N слов данных] [int32: N]
// (стандартный FORTRAN unformatted sequential)
// Сжатие определяется по магическому слову ANSCOMP в данных.
// ============================================================================

bool RstReader::read_record_at(int64_t word_offset, std::vector<uint8_t>& data,
                               RecordInfo& info, std::string& err) {
    int64_t byte_offset = word_offset * kWord;
    
    if (byte_offset + 8 > file_bytes_) {
        err = "record offset beyond file end (word=" + std::to_string(word_offset) + ")";
        return false;
    }
    
    // Читаем pre-маркер (число слов данных)
    int32_t pre_marker = 0;
    std::fseek(fp_, static_cast<long>(byte_offset), SEEK_SET);
    if (std::fread(&pre_marker, 4, 1, fp_) != 1) {
        err = "cannot read pre-marker at word " + std::to_string(word_offset);
        return false;
    }
    
    if (pre_marker <= 0 || pre_marker > (1 << 28)) {
        err = "invalid record length " + std::to_string(pre_marker) + 
              " at word " + std::to_string(word_offset);
        return false;
    }
    
    info.num_words = pre_marker;
    info.flags = 0;  // no separate flags word
    
    // Размер данных в байтах: pre_marker * 4 (N слов по 4 байта)
    int64_t data_bytes = static_cast<int64_t>(pre_marker) * kWord;
    
    // Проверяем, что запись помещается в файл
    // Формат: [pre:4] [data:data_bytes] [post:4]
    int64_t record_end = byte_offset + 4 + data_bytes + 4;
    if (record_end > file_bytes_) {
        err = "record extends beyond file end";
        return false;
    }
    
    info.total_bytes = record_end - byte_offset;
    
    // Читаем данные
    data.resize(static_cast<size_t>(data_bytes));
    if (data_bytes > 0) {
        if (std::fread(data.data(), 1, static_cast<size_t>(data_bytes), fp_) != static_cast<size_t>(data_bytes)) {
            err = "cannot read record data";
            return false;
        }
    }
    
    // Читаем post-маркер
    int32_t post_marker = 0;
    if (std::fread(&post_marker, 4, 1, fp_) != 1) {
        err = "cannot read post-marker";
        return false;
    }
    
    if (post_marker != pre_marker) {
        err = "record markers mismatch: pre=" + std::to_string(pre_marker) +
              " post=" + std::to_string(post_marker) +
              " at word " + std::to_string(word_offset);
        return false;
    }
    
    // Обработка сжатых данных (ANSCOMP)
    if (data_bytes >= 8 && is_comp_magic(data.data())) {
        compressed_ = true;
        std::vector<uint8_t> unc;
        if (!ansc_uncompress(data.data(), static_cast<size_t>(data_bytes), unc)) {
            err = "decompress failed at word " + std::to_string(word_offset);
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(unc_mtx_);
            unc_cache_[word_offset] = unc;
        }
        data = std::move(unc);
    }
    
    return true;
}

// ============================================================================
// Чтение записи как массива int32
// ============================================================================

bool RstReader::read_record_int32(int64_t word_offset, std::vector<int32_t>& data,
                                  RecordInfo& info, std::string& err) {
    // Проверяем кэш
    {
        std::lock_guard<std::mutex> lk(unc_mtx_);
        auto it = unc_cache_.find(word_offset);
        if (it != unc_cache_.end()) {
            size_t n = it->second.size() / 4;
            data.resize(n);
            std::memcpy(data.data(), it->second.data(), n * 4);
            info.num_words = static_cast<int32_t>(n);
            info.flags = 1; // integer
            return true;
        }
    }
    
    std::vector<uint8_t> raw;
    if (!read_record_at(word_offset, raw, info, err)) return false;
    
    // Конвертируем в int32: N слов = N int32 значений
    size_t n = raw.size() / 4;
    data.resize(n);
    if (n > 0) std::memcpy(data.data(), raw.data(), n * 4);
    
    return true;
}

// ============================================================================
// Чтение записи как массива double
// ============================================================================

bool RstReader::read_record_f64(int64_t word_offset, std::vector<double>& data,
                                RecordInfo& info, std::string& err) {
    // Проверяем кэш
    {
        std::lock_guard<std::mutex> lk(unc_mtx_);
        auto it = unc_cache_.find(word_offset);
        if (it != unc_cache_.end()) {
            size_t n = it->second.size() / 8;
            data.resize(n);
            std::memcpy(data.data(), it->second.data(), n * 8);
            info.num_words = static_cast<int32_t>(n * 2);
            info.flags = 0; // double
            return true;
        }
    }
    
    std::vector<uint8_t> raw;
    if (!read_record_at(word_offset, raw, info, err)) return false;
    
    // Данные: N слов = N*4 байт
    // Для double: каждое значение = 8 байт = 2 слова, поэтому N слов = N/2 doubles
    size_t n = raw.size() / 8;
    data.resize(n);
    if (n > 0) std::memcpy(data.data(), raw.data(), n * 8);
    
    return true;
}

// ============================================================================
// Получить полный размер записи в словах (для перехода к следующей)
// ============================================================================

int64_t RstReader::record_total_words(int64_t word_offset) {
    int64_t byte_offset = word_offset * kWord;
    if (byte_offset + 8 > file_bytes_) return -1;
    
    int32_t pre = 0;
    std::fseek(fp_, static_cast<long>(byte_offset), SEEK_SET);
    if (std::fread(&pre, 4, 1, fp_) != 1) return -1;
    if (pre <= 0) return -1;
    
    // Формат: [int32: pre] [pre слов данных] [int32: pre]
    // Total: pre_marker(1) + data(pre) + post_marker(1) = pre + 2
    return static_cast<int64_t>(pre) + 2;
}

// ============================================================================
// Открытие/закрытие файла
// ============================================================================

RstReader::~RstReader() { close(); }

void RstReader::close() {
    if (fp_) { std::fclose(fp_); fp_ = nullptr; }
}

bool RstReader::open(const std::string& p, std::string& err) {
    path_ = p;
    fp_ = std::fopen(p.c_str(), "rb");
    if (!fp_) { err = "cannot open " + p; return false; }
    std::fseek(fp_, 0, SEEK_END);
    file_bytes_ = std::ftell(fp_);
    std::fseek(fp_, 0, SEEK_SET);
    
    // 1. Читаем стандартный заголовок (первые 400 байт)
    if (!read_std_header(err)) return false;
    
    // 2. Читаем заголовок результата (record at word 103)
    if (!read_result_header(err)) return false;
    
    return true;
}

// ============================================================================
// Чтение заголовка результата (result header)
// Запись на словесном смещении 103 содержит 80 слов с указателями
// ============================================================================

bool RstReader::read_result_header(std::string& err) {
    // Результатный заголовок — на словесном смещении 103
    // (после стандартного заголовка 100 слов + первая запись)
    std::vector<int32_t> hdr;
    RecordInfo info;
    
    if (!read_record_int32(kResultHeaderRecord, hdr, info, err)) {
        return false;
    }
    
    if (hdr.size() < 80) {
        err = "result header too small: " + std::to_string(hdr.size()) + " words (need 80)";
        return false;
    }
    
    // Парсим заголовок результата согласно fdresu.inc / result_header_keys
    // Индексы (0-based) из pymapdl-reader _rst_keys.py result_header_keys:
    //
    // Позиции указателей в заголовке результата (из документации ANSYS):
    // [0]  = maxety   - max element type
    // [1]  = maxrl    - max real constant
    // [2]  = nnode    - number of nodes  
    // [3]  = nelem    - number of elements
    // [4]  = ptrLOC   - pointer to node locations (relative)
    // [5]  = ptrETY   - pointer to element types
    // [6]  = ptrSEC   - pointer to sections
    // ...
    // Ключевые указатели (из pymapdl-reader):
    
    // Из pymapdl-reader _rst_keys.py, result_header_keys:
    // Позиции в заголовке результата (0-based word index):
    n_nodes_ = hdr[2];    // nnod
    n_elem_  = hdr[3];    // nelm
    
    // Указатели (относительные — нужно добавить базу)
    // В pymapdl-reader эти указатели используются как абсолютные word offsets
    int64_t ptrNOD = hdr[16];  // ptrNOD - nodal equivalence table
    int64_t ptrELM = hdr[17];  // ptrELM - element equivalence table  
    int64_t ptrGEO = hdr[18];  // ptrGEO - geometry header
    int64_t ptrDSI = hdr[20];  // ptrDSI - data set index
    int64_t ptrTIM = hdr[21];  // ptrTIM - time values
    int64_t ptrLSP = hdr[22];  // ptrLSP - load step table
    int64_t ptrCOMP = hdr[25]; // ptrCOMP - components
    
    // Число наборов результатов
    nsets_ = hdr[19]; // nsets
    
    if (nsets_ <= 0 || nsets_ > 10000000) {
        err = "invalid number of result sets: " + std::to_string(nsets_);
        return false;
    }
    
    ptr_geo_ = ptrGEO;
    ptr_comp_ = ptrCOMP;
    ptr_nod_ = ptrNOD;
    ptr_elm_ = ptrELM;
    ptr_dsi_ = ptrDSI;
    ptr_tim_ = ptrTIM;
    ptr_lsp_ = ptrLSP;
    
    // Читаем таблицу указателей на наборы результатов (DSI)
    // DSI содержит 2*resmax int32: первые resmax = нижние 32 бита,
    // вторые resmax = верхние 32 бита указателей
    if (ptrDSI > 0) {
        std::vector<int32_t> dsi;
        RecordInfo dsi_info;
        if (read_record_int32(ptrDSI, dsi, dsi_info, err)) {
            int32_t resmax = dsi_info.num_words / 2;
            if (resmax > 0 && static_cast<int32_t>(dsi.size()) >= 2 * nsets_) {
                sets_.resize(nsets_);
                for (int i = 0; i < nsets_; ++i) {
                    // Комбинируем два int32 в int64 (little-endian)
                    int32_t lo = dsi[i];
                    int32_t hi = dsi[resmax + i];
                    sets_[i].rpointer_words = static_cast<int64_t>(lo) | 
                                              (static_cast<int64_t>(hi) << 32);
                }
            }
        }
        err.clear();
    }
    
    // Читаем временные значения
    if (ptrTIM > 0 && nsets_ > 0) {
        std::vector<double> times;
        RecordInfo tim_info;
        if (read_record_f64(ptrTIM, times, tim_info, err)) {
            for (int i = 0; i < nsets_ && i < static_cast<int>(times.size()); ++i) {
                sets_[i].time = times[i];
            }
        }
        err.clear();
    }
    
    // Читаем таблицу load step/substep
    if (ptrLSP > 0 && nsets_ > 0) {
        std::vector<int32_t> lsp;
        RecordInfo lsp_info;
        if (read_record_int32(ptrLSP, lsp, lsp_info, err)) {
            for (int i = 0; i < nsets_; ++i) {
                int base = i * 3;
                if (base + 2 < static_cast<int>(lsp.size())) {
                    sets_[i].step    = lsp[base + 0];
                    sets_[i].substep = lsp[base + 1];
                    // lsp[base+2] = cumulative iteration
                }
            }
        }
        err.clear();
    }
    
    // Читаем nodal equivalence table
    if (ptrNOD > 0) {
        RecordInfo nod_info;
        read_record_int32(ptrNOD, neqv_, nod_info, err);
        err.clear();
    }
    
    // Читаем element equivalence table
    if (ptrELM > 0) {
        RecordInfo elm_info;
        read_record_int32(ptrELM, eeqv_, elm_info, err);
        err.clear();
    }
    
    // Читаем компоненты (Name Selections)
    if (ptrCOMP > 0) {
        std::vector<int32_t> comp_data;
        RecordInfo comp_info;
        if (read_record_int32(ptrCOMP, comp_data, comp_info, err)) {
            parse_components(comp_data);
        }
        err.clear();
    }
    
    return true;
}

// ============================================================================
// Парсинг компонентов
// Формат: каждый компонент — отдельная запись после ptrCOMP
// Запись: [type(1)] [name(8 words)] [node/elem list...]
// ============================================================================

void RstReader::parse_components(const std::vector<int32_t>& data) {
    // Компоненты хранятся как отдельные записи после ptrCOMP
    // Формат из pymapdl-reader:
    // table[0] = type (1=node, 2=elem)
    // table[1:9] = name (8 words = 32 chars, each word is 4 chars reversed)
    // table[9:] = node/elem numbers (bitmask or list)
    
    // Для простоты: читаем компоненты последовательно из записей
    // начиная с ptrCOMP_
    int64_t file_ptr = ptr_comp_;
    
    for (int c = 0; c < 1000; ++c) {  // max 1000 components
        std::vector<int32_t> table;
        RecordInfo info;
        std::string err;
        
        if (!read_record_int32(file_ptr, table, info, err)) break;
        if (table.size() < 9) break;
        
        int32_t type = table[0];
        if (type != 1 && type != 2) break;
        
        // Извлекаем имя (8 слов, каждое слово = 4 символа в обратном порядке)
        std::string name;
        for (int w = 1; w <= 8 && (w) < static_cast<int>(table.size()); ++w) {
            int32_t word = table[w];
            char chars[4];
            std::memcpy(chars, &word, 4);
            // ANSYS хранит символы в обратном порядке в каждом слове
            name += chars[3];
            name += chars[2];
            name += chars[1];
            name += chars[0];
        }
        // Trim
        auto end = std::find_if(name.rbegin(), name.rend(),
                                [](char c){ return c != ' ' && c != '\0'; });
        name.erase(end.base(), name.end());
        
        if (name.empty()) break;
        
        // Извлекаем список узлов/элементов
        Component comp;
        comp.name = name;
        
        if (table.size() > 9) {
            // Данные после имени — это либо список номеров, либо битовая маска
            // Формат: каждый int32 содержит 32 флага (битовая маска)
            // Или это может быть просто список номеров
            // Из pymapdl-reader: component_interperter(data)
            // Это битовая маска: если бит установлен, соответствующий узел/элемент в компоненте
            
            const int32_t* mask_data = table.data() + 9;
            size_t mask_words = table.size() - 9;
            
            if (type == 1) {
                // Node component
                comp.is_node = true;
                for (size_t w = 0; w < mask_words; ++w) {
                    int32_t mask = mask_data[w];
                    for (int bit = 0; bit < 32; ++bit) {
                        if (mask & (1 << bit)) {
                            int32_t node_idx = static_cast<int32_t>(w * 32 + bit);
                            if (node_idx < static_cast<int32_t>(neqv_.size())) {
                                comp.nodes.push_back(neqv_[node_idx]);
                            } else {
                                comp.nodes.push_back(node_idx + 1);
                            }
                        }
                    }
                }
            } else {
                // Element component
                comp.is_elem = true;
                for (size_t w = 0; w < mask_words; ++w) {
                    int32_t mask = mask_data[w];
                    for (int bit = 0; bit < 32; ++bit) {
                        if (mask & (1 << bit)) {
                            int32_t elem_idx = static_cast<int32_t>(w * 32 + bit);
                            if (elem_idx < static_cast<int32_t>(eeqv_.size())) {
                                comp.elems.push_back(eeqv_[elem_idx]);
                            } else {
                                comp.elems.push_back(elem_idx + 1);
                            }
                        }
                    }
                }
            }
        }
        
        components_.push_back(std::move(comp));
        
        // Переходим к следующей записи
        int64_t total = record_total_words(file_ptr);
        if (total <= 0) break;
        file_ptr += total;
    }
}

// ============================================================================
// Чтение узлов
// ============================================================================

bool RstReader::read_nodes(std::vector<Node>& nodes, std::string& err) {
    if (!fp_) { err = "file not open"; return false; }
    if (ptr_geo_ == 0) { err = "no geometry pointer"; return false; }
    
    // Читаем заголовок геометрии
    std::vector<int32_t> geo_hdr;
    RecordInfo geo_info;
    if (!read_record_int32(ptr_geo_, geo_hdr, geo_info, err)) return false;
    
    // Из заголовка геометрии получаем указатель на координаты узлов (ptrLOC)
    // geometry_header_keys из pymapdl-reader:
    // [0] = maxety, [1] = maxrl, [2] = nnod, [3] = nelm
    // [4] = ptrLOC (относительный), ...
    if (geo_hdr.size() < 20) {
        err = "geometry header too small";
        return false;
    }
    
    int64_t ptrLOC = 0;
    // Ищем ptrLOC в заголовке геометрии
    // Из pymapdl-reader geometry_header_keys:
    // Позиция ptrLOC зависит от версии, но обычно это слово 10 или около того
    // Попробуем несколько позиций
    for (size_t i = 4; i < geo_hdr.size() && i < 30; ++i) {
        int64_t val = geo_hdr[i];
        if (val > 0 && val * kWord < file_bytes_ && val > ptr_geo_) {
            ptrLOC = val;
            break;
        }
    }
    
    // Если не нашли, используем абсолютный указатель
    if (ptrLOC == 0) {
        // Попробуем прочитать запись сразу после заголовка геометрии
        int64_t next = ptr_geo_ + record_total_words(ptr_geo_);
        if (next > 0) {
            ptrLOC = next;
        } else {
            err = "cannot find node location pointer";
            return false;
        }
    }
    
    // Читаем координаты узлов
    // Формат: Node, X, Y, Z, THXY, THYZ, THZX для каждого узла (7 doubles)
    // Но может быть и другой формат в зависимости от версии
    std::vector<double> loc_data;
    RecordInfo loc_info;
    if (!read_record_f64(ptrLOC, loc_data, loc_info, err)) return false;
    
    nodes.clear();
    int64_t nnod = n_nodes_;
    int64_t stride = loc_data.size() / nnod;
    if (stride < 4) {
        // Попробуем другой stride
        stride = 7; // стандартный: id + x + y + z + thxy + thyz + thzx
        if (static_cast<int64_t>(loc_data.size()) < nnod * stride) {
            stride = 4; // минимум: x, y, z + padding
        }
    }
    
    nodes.reserve(static_cast<size_t>(nnod));
    for (int64_t i = 0; i < nnod; ++i) {
        int64_t base = i * stride;
        if (base + 3 >= static_cast<int64_t>(loc_data.size())) break;
        
        Node nd;
        nd.id = static_cast<int32_t>(i + 1);
        if (!neqv_.empty() && i < static_cast<int64_t>(neqv_.size())) {
            nd.id = neqv_[i];
        }
        
        // Определяем формат: если stride >= 7, первое значение может быть ID
        if (stride >= 7) {
            nd.x = loc_data[base + 1];
            nd.y = loc_data[base + 2];
            nd.z = loc_data[base + 3];
            if (stride >= 7 && base + 6 < static_cast<int64_t>(loc_data.size())) {
                nd.rx = loc_data[base + 4];
                nd.ry = loc_data[base + 5];
                nd.rz = loc_data[base + 6];
            }
        } else {
            nd.x = loc_data[base + 0];
            nd.y = loc_data[base + 1];
            nd.z = loc_data[base + 2];
        }
        
        nodes.push_back(nd);
    }
    
    return true;
}

// ============================================================================
// Чтение элементов
// ============================================================================

bool RstReader::read_elements(std::vector<Element>& elems, std::string& err) {
    if (!fp_) { err = "file not open"; return false; }
    if (ptr_geo_ == 0) { err = "no geometry pointer"; return false; }
    
    // Читаем заголовок геометрии для получения ptrEID
    std::vector<int32_t> geo_hdr;
    RecordInfo geo_info;
    if (!read_record_int32(ptr_geo_, geo_hdr, geo_info, err)) return false;
    
    // Ищем ptrEID (указатель на таблицу элементов)
    int64_t ptrEID = 0;
    for (size_t i = 4; i < geo_hdr.size() && i < 30; ++i) {
        int64_t val = geo_hdr[i];
        if (val > 0 && val * kWord < file_bytes_ && val > ptr_geo_) {
            // Проверяем, не является ли это уже найденным ptrLOC
            ptrEID = val;
            break;
        }
    }
    
    if (ptrEID == 0) {
        err = "cannot find element pointer";
        return false;
    }
    
    // Читаем таблицу элементов
    std::vector<int32_t> eid_data;
    RecordInfo eid_info;
    if (!read_record_int32(ptrEID, eid_data, eid_info, err)) return false;
    
    elems.clear();
    elems.reserve(static_cast<size_t>(n_elem_));
    
    // Каждый элемент описывается отдельной записью
    // Формат записи элемента:
    // [mat, type, real, secnum, esys, death, solidm, shape, elnum, pexcl, nodes...]
    int64_t elem_ptr = ptrEID + record_total_words(ptrEID);
    
    for (int64_t i = 0; i < n_elem_; ++i) {
        std::vector<int32_t> elem_data;
        RecordInfo elem_info;
        std::string elem_err;
        
        if (!read_record_int32(elem_ptr, elem_data, elem_info, elem_err)) break;
        
        Element el;
        if (!eeqv_.empty() && i < static_cast<int64_t>(eeqv_.size())) {
            el.id = eeqv_[i];
        } else {
            el.id = static_cast<int32_t>(i + 1);
        }
        
        if (elem_data.size() >= 10) {
            el.material_id = elem_data[0];
            el.type_id = elem_data[1];
            el.real_id = elem_data[2];
            
            // Узлы начинаются с позиции 10 (или другой, зависит от версии)
            for (size_t k = 10; k < elem_data.size(); ++k) {
                if (elem_data[k] > 0) {
                    el.nodes.push_back(elem_data[k]);
                }
            }
        }
        
        if (!el.nodes.empty()) {
            elems.push_back(std::move(el));
        }
        
        int64_t total = record_total_words(elem_ptr);
        if (total <= 0) break;
        elem_ptr += total;
    }
    
    return true;
}

// ============================================================================
// Чтение перемещений
// ============================================================================

bool RstReader::read_displacements(int set_index, std::vector<Displacement>& disps,
                                   std::string& err) {
    if (!fp_) { err = "file not open"; return false; }
    if (set_index < 0 || set_index >= nsets_) { err = "invalid set index"; return false; }
    
    const ResultSet& rs = sets_[set_index];
    if (rs.rpointer_words <= 0) { err = "invalid result pointer"; return false; }
    
    // Читаем заголовок решения
    std::vector<int32_t> sol_hdr;
    RecordInfo sol_info;
    if (!read_record_int32(rs.rpointer_words, sol_hdr, sol_info, err)) return false;
    
    // Ищем указатель на nodal solution (NSL)
    // solution_data_header_keys из pymapdl-reader
    // ptrNSL — указатель на nodal solution (относительно rpointer)
    int64_t ptrNSL = 0;
    for (size_t i = 30; i < sol_hdr.size() && i < 130; ++i) {
        int64_t val = sol_hdr[i];
        if (val > 0 && val < 10000000) {
            ptrNSL = val;
            break;
        }
    }
    
    if (ptrNSL == 0) {
        err = "nodal solution pointer not found";
        return false;
    }
    
    // Читаем nodal solution (массив double: nnod * sumdof)
    int64_t abs_ptr = rs.rpointer_words + ptrNSL;
    std::vector<double> nsl_data;
    RecordInfo nsl_info;
    if (!read_record_f64(abs_ptr, nsl_data, nsl_info, err)) return false;
    
    // Определяем число DOF на узел
    int64_t nnod = n_nodes_;
    int64_t sumdof = nsl_data.size() / nnod;
    if (sumdof < 1) sumdof = 3;
    
    disps.clear();
    disps.reserve(static_cast<size_t>(nnod));
    
    for (int64_t i = 0; i < nnod; ++i) {
        Displacement d;
        if (!neqv_.empty() && i < static_cast<int64_t>(neqv_.size())) {
            d.node_id = neqv_[i];
        } else {
            d.node_id = static_cast<int32_t>(i + 1);
        }
        
        d.values.resize(static_cast<size_t>(sumdof));
        for (int64_t j = 0; j < sumdof; ++j) {
            int64_t idx = i * sumdof + j;
            if (idx < static_cast<int64_t>(nsl_data.size())) {
                d.values[j] = nsl_data[idx];
            }
        }
        
        // Проверяем на "invalid" значения (2^100)
        bool valid = false;
        for (auto v : d.values) {
            if (v != 0.0 && std::fabs(v) < 1e30) { valid = true; break; }
        }
        if (valid || n_nodes_ < 100) {
            disps.push_back(std::move(d));
        }
    }
    
    return true;
}

// ============================================================================
// Чтение узловых напряжений
// ============================================================================

bool RstReader::read_nodal_stresses(int set_index, std::vector<NodalStress>& stresses,
                                    std::string& err) {
    if (!fp_) { err = "file not open"; return false; }
    if (set_index < 0 || set_index >= nsets_) { err = "invalid set index"; return false; }
    
    const ResultSet& rs = sets_[set_index];
    if (rs.rpointer_words <= 0) { err = "invalid result pointer"; return false; }
    
    // Читаем заголовок решения
    std::vector<int32_t> sol_hdr;
    RecordInfo sol_info;
    if (!read_record_int32(rs.rpointer_words, sol_hdr, sol_info, err)) return false;
    
    // Ищем указатель на element solution (ptrESL)
    int64_t ptrESL = 0;
    for (size_t i = 30; i < sol_hdr.size() && i < 130; ++i) {
        int64_t val = sol_hdr[i];
        if (val > 0 && val < 10000000) {
            ptrESL = val;
            break;
        }
    }
    
    if (ptrESL == 0) {
        err = "element solution pointer not found";
        return false;
    }
    
    // Читаем element solution header (таблица указателей на элементные данные)
    int64_t abs_esl = rs.rpointer_words + ptrESL;
    std::vector<int32_t> esl_hdr;
    RecordInfo esl_info;
    if (!read_record_int32(abs_esl, esl_hdr, esl_info, err)) return false;
    
    // Для каждого элемента читаем напряжения (ENS)
    // ENS хранится в element data records
    // Формат: для каждого элемента есть запись с данными
    // Элементные данные: EMS, ENF, ENS, ENG, EGR, EEL, EPL, ...
    // ENS = index 2 (0-based) в таблице элементных данных
    
    stresses.clear();
    std::map<int32_t, NodalStress> stress_map;
    
    // Читаем данные для каждого элемента
    // esl_hdr содержит int64 pointers (viewed as int64) для каждого элемента
    size_t nelem = esl_hdr.size() / 2;  // int64 = 2 int32
    
    for (size_t e = 0; e < nelem && e < static_cast<size_t>(n_elem_); ++e) {
        // Get element pointer (int64 from two int32)
        int64_t elem_data_ptr = 0;
        if (e * 2 + 1 < esl_hdr.size()) {
            int32_t lo = esl_hdr[e * 2];
            int32_t hi = esl_hdr[e * 2 + 1];
            elem_data_ptr = static_cast<int64_t>(static_cast<uint32_t>(lo)) |
                           (static_cast<int64_t>(hi) << 32);
        }
        
        if (elem_data_ptr <= 0) continue;
        
        // Читаем запись элементных данных
        int64_t abs_elem_ptr = abs_esl + elem_data_ptr;
        std::vector<int32_t> elem_data;
        RecordInfo ed_info;
        std::string ed_err;
        
        if (!read_record_int32(abs_elem_ptr, elem_data, ed_info, ed_err)) continue;
        
        // ENS pointer is at index 2 in the element data index table
        if (elem_data.size() <= 2) continue;
        int32_t ens_ptr = elem_data[2];
        if (ens_ptr <= 0) continue;
        
        // Читаем напряжения для этого элемента
        int64_t abs_ens = abs_elem_ptr + ens_ptr;
        std::vector<double> ens_data;
        RecordInfo ens_info;
        std::string ens_err;
        
        if (!read_record_f64(abs_ens, ens_data, ens_info, ens_err)) continue;
        
        // ENS format: для каждого corner node: SX, SY, SZ, SXY, SYZ, SXZ (6 values)
        // Число nodes per element зависит от типа элемента
        size_t ncomp = 6;
        size_t nnodes_elem = ens_data.size() / ncomp;
        
        // Получаем номера узлов элемента
        // (для простоты используем последовательные номера)
        for (size_t n = 0; n < nnodes_elem; ++n) {
            size_t base = n * ncomp;
            if (base + 5 >= ens_data.size()) break;
            
            // Определяем номер узла (из element description)
            int32_t node_id = static_cast<int32_t>(n + 1); // placeholder
            
            auto& ns = stress_map[node_id];
            ns.node_id = node_id;
            ns.sx  = ens_data[base + 0];
            ns.sy  = ens_data[base + 1];
            ns.sz  = ens_data[base + 2];
            ns.sxy = ens_data[base + 3];
            ns.syz = ens_data[base + 4];
            ns.sxz = ens_data[base + 5];
        }
    }
    
    for (auto& kv : stress_map) {
        stresses.push_back(kv.second);
    }
    
    return true;
}

// ============================================================================
// Поиск наборов
// ============================================================================

int RstReader::find_set_by_substep(int substep) const {
    for (size_t i = 0; i < sets_.size(); ++i)
        if (sets_[i].substep == substep) return static_cast<int>(i);
    return -1;
}

int RstReader::find_set_by_time(double t, double tol) const {
    int best = -1; double bd = tol;
    for (size_t i = 0; i < sets_.size(); ++i) {
        double d = std::fabs(sets_[i].time - t);
        if (d <= bd) { bd = d; best = static_cast<int>(i); }
    }
    return best;
}

} // namespace rst_reader
