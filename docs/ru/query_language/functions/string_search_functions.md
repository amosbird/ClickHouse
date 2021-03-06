# Функции поиска в строках

Во всех функциях, поиск регистрозависимый по-умолчанию. Существуют варианты функций для регистронезависимого поиска.

## position(haystack, needle)
Поиск подстроки `needle` в строке `haystack`.
Возвращает позицию (в байтах) найденной подстроки, начиная с 1, или 0, если подстрока не найдена.

Для поиска без учета регистра используйте функцию `positionCaseInsensitive`.

## positionUTF8(haystack, needle)
Так же, как `position`, но позиция возвращается в кодовых точках Unicode. Работает при допущении, что строка содержит набор байт, представляющий текст в кодировке UTF-8. Если допущение не выполнено -- то возвращает какой-нибудь результат (не кидает исключение).

Для поиска без учета регистра используйте функцию `positionCaseInsensitiveUTF8`.

## multiSearchAllPositions(haystack, [needle<sub>1</sub>, needle<sub>2</sub>, ..., needle<sub>n</sub>])
Так же, как и `position`, только возвращает `Array` первых вхождений.

Для поиска без учета регистра и/или в кодировке UTF-8 используйте функции `multiSearchAllPositionsCaseInsensitive, multiSearchAllPositionsUTF8, multiSearchAllPositionsCaseInsensitiveUTF8`.

## multiSearchFirstPosition(haystack, [needle<sub>1</sub>, needle<sub>2</sub>, ..., needle<sub>n</sub>])

Так же, как и `position`, только возвращает оффсет первого вхождения любого из needles.

Для поиска без учета регистра и/или в кодировке UTF-8 используйте функции `multiSearchFirstPositionCaseInsensitive, multiSearchFirstPositionUTF8, multiSearchFirstPositionCaseInsensitiveUTF8`.

## multiSearchFirstIndex(haystack, [needle<sub>1</sub>, needle<sub>2</sub>, ..., needle<sub>n</sub>])
Возвращает индекс `i` (нумерация с единицы) первой найденной строки needle<sub>i</sub> в строке `haystack` и 0 иначе.

Для поиска без учета регистра и/или в кодировке UTF-8 используйте функции `multiSearchFirstIndexCaseInsensitive, multiSearchFirstIndexUTF8, multiSearchFirstIndexCaseInsensitiveUTF8`.

## multiSearchAny(haystack, [needle<sub>1</sub>, needle<sub>2</sub>, ..., needle<sub>n</sub>]) {#function-multisearchany}
Возвращает 1, если хотя бы одна подстрока needle<sub>i</sub> нашлась в строке `haystack` и 0 иначе.

Для поиска без учета регистра и/или в кодировке UTF-8 используйте функции `multiSearchAnyCaseInsensitive, multiSearchAnyUTF8, multiSearchAnyCaseInsensitiveUTF8`.

!!! note "Примечание"
    Во всех функциях `multiSearch*` количество needles должно быть меньше 2<sup>8</sup> из-за особенностей реализации.

## match(haystack, pattern)
Проверка строки на соответствие регулярному выражению pattern. Регулярное выражение **re2**. Синтаксис регулярных выражений **re2** является более ограниченным по сравнению с регулярными выражениями **Perl** ([подробнее](https://github.com/google/re2/wiki/Syntax)).
Возвращает 0 (если не соответствует) или 1 (если соответствует).

Обратите внимание, что для экранирования в регулярном выражении, используется символ `\` (обратный слеш). Этот же символ используется для экранирования в строковых литералах. Поэтому, чтобы экранировать символ в регулярном выражении, необходимо написать в строковом литерале \\ (два обратных слеша).

Регулярное выражение работает со строкой как с набором байт. Регулярное выражение не может содержать нулевые байты.
Для шаблонов на поиск подстроки в строке, лучше используйте LIKE или position, так как они работают существенно быстрее.

## multiMatchAny(haystack, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `match`, но возвращает ноль, если ни одно регулярное выражение не подошло и один, если хотя бы одно. Используется библиотека [hyperscan](https://github.com/intel/hyperscan) для соответствия регулярных выражений. Для шаблонов на поиск многих подстрок в строке, лучше используйте `multiSearchAny`, так как она работает существенно быстрее.

!!! note "Примечание"
    Длина любой строки из `haystack` должна быть меньше 2<sup>32</sup> байт, иначе бросается исключение. Это ограничение связано с ограничением hyperscan API.

## multiMatchAnyIndex(haystack, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `multiMatchAny`, только возвращает любой индекс подходящего регулярного выражения.

## multiMatchAllIndices(haystack, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `multiMatchAny`, только возвращает массив всех индексов всех подходящих регулярных выражений в любом порядке.

## multiFuzzyMatchAny(haystack, distance, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `multiMatchAny`, но возвращает 1 если любой pattern соответствует haystack в пределах константного [редакционного расстояния](https://en.wikipedia.org/wiki/Edit_distance). Эта функция также находится в экспериментальном режиме и может быть очень медленной. За подробностями обращайтесь к [документации hyperscan](https://intel.github.io/hyperscan/dev-reference/compilation.html#approximate-matching).

## multiFuzzyMatchAnyIndex(haystack, distance, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `multiFuzzyMatchAny`, только возвращает любой индекс подходящего регулярного выражения в пределах константного редакционного расстояния.

## multiFuzzyMatchAllIndices(haystack, distance, [pattern<sub>1</sub>, pattern<sub>2</sub>, ..., pattern<sub>n</sub>])

То же, что и `multiFuzzyMatchAny`, только возвращает массив всех индексов всех подходящих регулярных выражений в любом порядке в пределах константного редакционного расстояния.

!!! note "Примечание"
    `multiFuzzyMatch*` функции не поддерживают UTF-8 закодированные регулярные выражения, и такие выражения рассматриваются как байтовые из-за ограничения hyperscan.

!!! note "Примечание"
    Чтобы выключить все функции, использующие hyperscan, используйте настройку `SET allow_hyperscan = 0;`.

## extract(haystack, pattern)
Извлечение фрагмента строки по регулярному выражению. Если haystack не соответствует регулярному выражению pattern, то возвращается пустая строка. Если регулярное выражение не содержит subpattern-ов, то вынимается фрагмент, который подпадает под всё регулярное выражение. Иначе вынимается фрагмент, который подпадает под первый subpattern.

## extractAll(haystack, pattern)
Извлечение всех фрагментов строки по регулярному выражению. Если haystack не соответствует регулярному выражению pattern, то возвращается пустая строка. Возвращается массив строк, состоящий из всех соответствий регулярному выражению. В остальном, поведение аналогично функции extract (по прежнему, вынимается первый subpattern, или всё выражение, если subpattern-а нет).

## like(haystack, pattern), оператор haystack LIKE pattern {#function-like}
Проверка строки на соответствие простому регулярному выражению.
Регулярное выражение может содержать метасимволы `%` и `_`.

`%` обозначает любое количество любых байт (в том числе, нулевое количество символов).

`_` обозначает один любой байт.

Для экранирования метасимволов, используется символ `\` (обратный слеш). Смотрите замечание об экранировании в описании функции match.

Для регулярных выражений вида `%needle%` действует более оптимальный код, который работает также быстро, как функция `position`.
Для остальных регулярных выражений, код аналогичен функции match.

## notLike(haystack, pattern), оператор haystack NOT LIKE pattern {#function-notlike}
То же, что like, но с отрицанием.

## ngramDistance(haystack, needle)

Вычисление 4-граммного расстояния между `haystack` и `needle`: считается симметрическая разность между двумя мультимножествами 4-грамм и нормализается на сумму их мощностей. Возвращает число float от 0 до 1 -- чем ближе к нулю, тем больше строки похожи друг на друга. Если константный `needle` или `haystack` больше чем 32КБ, кидается исключение. Если некоторые строки из неконстантного `haystack` или `needle` больше 32КБ, расстояние всегда равно единице.

Для поиска без учета регистра и/или в формате UTF-8 используйте функции `ngramDistanceCaseInsensitive, ngramDistanceUTF8, ngramDistanceCaseInsensitiveUTF8`.

## ngramSearch(haystack, needle)

То же, что и `ngramDistance`, но вычисляет несимметричную разность между `needle` и `haystack` -- количество n-грамм из `needle` минус количество общих n-грамм, нормированное на количество n-грамм из `needle`. Чем ближе результат к единице, тем вероятнее, что `needle` внутри `haystack`. Может быть использовано для приближенного поиска.

Для поиска без учета регистра и/или в формате UTF-8 используйте функции `ngramSearchCaseInsensitive, ngramSearchUTF8, ngramSearchCaseInsensitiveUTF8`.


!!! note "Примечание"
    Для случая UTF-8 мы используем триграммное расстояние. Вычисление n-граммного расстояния не совсем честное. Мы используем 2-х байтные хэши для хэширования n-грамм, а затем вычисляем (не)симметрическую разность между хэш таблицами -- могут возникнуть коллизии. В формате UTF-8 без учета регистра мы не используем честную функцию `tolower` -- мы обнуляем 5-й бит (нумерация с нуля) каждого байта кодовой точки, а также первый бит нулевого байта, если байтов больше 1 -- это работает для латиницы и почти для всех кириллических букв.

[Оригинальная статья](https://clickhouse.yandex/docs/ru/query_language/functions/string_search_functions/) <!--hide-->
