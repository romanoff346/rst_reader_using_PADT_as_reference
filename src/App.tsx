export default function App() {
  return (
    <div className="min-h-screen bg-gray-900 text-gray-100 p-8">
      <div className="max-w-4xl mx-auto">
        <h1 className="text-3xl font-bold text-blue-400 mb-4">
          ANSYS RST Reader
        </h1>
        <p className="text-gray-300 mb-6">
          Чтение файлов результатов ANSYS MAPDL (.rst) без использования ANSYS.
          Прямой бинарный парсинг формата.
        </p>

        <div className="bg-gray-800 rounded-lg p-6 mb-6">
          <h2 className="text-xl font-semibold text-green-400 mb-3">
            ✅ Обновления (на основе PADT + pymapdl-reader)
          </h2>
          <ul className="space-y-2 text-gray-300">
            <li>
              <strong className="text-yellow-300">Стандартный заголовок</strong> — первые 100 слов (400 байт) читаются как сырые данные БЕЗ FORTRAN маркеров
            </li>
            <li>
              <strong className="text-yellow-300">FORTRAN записи</strong> — формат [int32: N] [N слов] [int32: N] без дополнительных флагов
            </li>
            <li>
              <strong className="text-yellow-300">Result header</strong> — на фиксированной позиции word 103 (byte 412)
            </li>
            <li>
              <strong className="text-yellow-300">Указатели</strong> — все в словах (word = 4 байта) от начала файла
            </li>
            <li>
              <strong className="text-yellow-300">ANSCOMP</strong> — сжатие определяется по магическому слову 'ANS-' в данных
            </li>
          </ul>
        </div>

        <div className="bg-gray-800 rounded-lg p-6 mb-6">
          <h2 className="text-xl font-semibold text-blue-400 mb-3">
            Формат файла
          </h2>
          <pre className="text-sm text-gray-300 overflow-x-auto">
{`┌──────────────────────────────────────────────────┐
│ Стандартный заголовок (400 байт, БЕЗ маркеров)   │
│ word 0: file_number=100                          │
│ word 2: file_format=12 (RST)                     │
│ word 27: system_record_size                      │
├──────────────────────────────────────────────────┤
│ FORTRAN записи (после byte 400):                 │
│ [int32: N][N слов данных][int32: N]              │
│                                                  │
│ Record @word 103 = Result Header (80 слов)       │
│ [nnod, nelm, ptrNOD, ptrELM, ptrGEO, ...]       │
├──────────────────────────────────────────────────┤
│ DSI → указатели на наборы результатов            │
│ GEO → узлы, элементы                             │
│ Для каждого set: Solution Header → NSL → ESL    │
└──────────────────────────────────────────────────┘`}
          </pre>
        </div>

        <div className="bg-gray-800 rounded-lg p-6 mb-6">
          <h2 className="text-xl font-semibold text-purple-400 mb-3">
            Ключевые источники
          </h2>
          <ul className="space-y-2 text-gray-300">
            <li>
              <a href="https://www.padtinc.com/2016/02/19/reading-ansys-mechanical-rst-c-cpp-part1/" 
                 className="text-blue-400 hover:underline" target="_blank" rel="noopener">
                PADT: Reading ANSYS RST from C/C++
              </a> — описание BinLib и подхода к чтению RST
            </li>
            <li>
              <a href="https://github.com/ansys/pymapdl-reader" 
                 className="text-blue-400 hover:underline" target="_blank" rel="noopener">
                pymapdl-reader
              </a> — Python библиотека (основной референс)
            </li>
            <li>
              <a href="https://ansyshelp.ansys.com/public/views/secured/corp/v251/en/ans_prog/datafromRST.html" 
                 className="text-blue-400 hover:underline" target="_blank" rel="noopener">
                ANSYS Programmer's Reference
              </a> — официальная документация формата
            </li>
          </ul>
        </div>

        <div className="bg-gray-800 rounded-lg p-6">
          <h2 className="text-xl font-semibold text-orange-400 mb-3">
            Использование (C++ CLI)
          </h2>
          <pre className="text-sm text-gray-300 overflow-x-auto">
{`# Сборка
cd rst_reader && make

# Диагностика формата
./rst_reader_demo file.rst --diagnose

# Информация о файле
./rst_reader_demo file.rst --list

# Узлы и элементы
./rst_reader_demo file.rst --nodes --elements

# Напряжения с Мизесом
./rst_reader_demo file.rst --set 0 --stresses --mises`}
          </pre>
        </div>
      </div>
    </div>
  );
}
